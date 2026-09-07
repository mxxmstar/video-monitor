#include "media/pusher/ffmpeg_muxer.h"
#include "common/log/logger.h"
#include <cstring>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
}
namespace {
/// @brief 获取 FFmpeg 错误码对应的错误字符串
std::string AvErrorString(int ret) {
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_make_error_string(buf, AV_ERROR_MAX_STRING_SIZE, ret);
    return buf;
}


}


FFmpegMuxer::FFmpegMuxer() {

}

FFmpegMuxer::~FFmpegMuxer() {
    Close();
}



bool FFmpegMuxer::Open(const std::string& output_url, const MediaTrackConfig& config) {
    // Open 可以重复调用。先关闭旧的输出，保证旧的 AVIO 和 AVFormatContext
    // 不会泄漏，也避免新的 stream 挂到旧 context 上。
    Close();

    if (output_url.empty() || !config.is_video() || !config.is_valid() ||
        !IsValidTimeBase(Rational{config.time_base_num, config.time_base_den})) {
        LOG_ERROR("FFmpegMuxer received an invalid video configuration");
        return false;
    }

    // 当前教学版只实现 H.264 视频写出；后续支持 H.265 时再扩展 codec 映射。
    if (config.codec_type != CodecType::H264) {
        LOG_ERROR("FFmpegMuxer currently supports H264 video only");
        return false;
    }

    output_url_ = output_url;

    // 分配 AVFormatContext
    // 第三个参数 format_name 这里暂时不管，让 FFmpeg 自动选择
    int ret = avformat_alloc_output_context2(&format_ctx_, nullptr, nullptr, output_url_.c_str());
    if (ret < 0 || !format_ctx_) {
        LOG_ERROR("avformat_alloc_output_context2 failed: {}", AvErrorString(ret));
        Close();
        return false;
    }

    // 创建视频流
    video_stream_ = avformat_new_stream(format_ctx_, nullptr);
    if (!video_stream_) {
        LOG_ERROR("avformat_new_stream failed: codec not found");
        Close();
        return false;
    }

    // 配置视频流参数
    auto video_config = config.video();
    video_stream_->codecpar->codec_id = AV_CODEC_ID_H264;
    video_stream_->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    video_stream_->codecpar->width = video_config.width;
    video_stream_->codecpar->height = video_config.height;
    // 轨道时间基来自配置，而不是把 float fps 强行转换成整数分母。
    // 例如配置为 1/1,000,000，表示轨道时间戳以微秒为单位；写 header 后
    // FFmpeg 可能根据目标容器调整 video_stream_->time_base，Write() 会读取
    // 调整后的实际值进行重标定。
    video_stream_->time_base = {config.time_base_num, config.time_base_den};

    // AVCodecParameters::extradata 由调用方负责分配，不能直接 memcpy 到空指针。
    // 末尾的 padding 是 FFmpeg 对 codec extradata 的常规要求。
    if (!config.extra_data.empty()) {
        const auto extra_data_size = config.extra_data.size();
        video_stream_->codecpar->extradata = static_cast<std::uint8_t*>(
            av_mallocz(extra_data_size + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!video_stream_->codecpar->extradata) {
            LOG_ERROR("failed to allocate video codec extradata");
            Close();
            return false;
        }
        std::memcpy(video_stream_->codecpar->extradata,
                    config.extra_data.data(),
                    extra_data_size);
        video_stream_->codecpar->extradata_size =
            static_cast<int>(extra_data_size);
    }

    // 对文件、RTSP 等需要 AVIO 的输出，必须先打开写端；内存 muxer 等
    // AVFMT_NOFILE 输出则由 FFmpeg 自己管理 IO。
    if (!(format_ctx_->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&format_ctx_->pb,
                        output_url_.c_str(),
                        AVIO_FLAG_WRITE);
        if (ret < 0) {
            LOG_ERROR("avio_open failed: {}", AvErrorString(ret));
            Close();
            return false;
        }
    }

    // 写入文件头
    ret = avformat_write_header(format_ctx_, nullptr);
    if (ret < 0) {
        LOG_ERROR("avformat_write_header failed: {}", AvErrorString(ret));
        Close();
        return false;
    }
    header_written_ = true;
    LOG_INFO("Muxer Opend: {}", output_url_);
    LOG_INFO("Video Config: {}x{} {}fps", video_config.width, video_config.height, video_config.fps);
    return true;
}

void FFmpegMuxer::Close() {
    if (format_ctx_ && header_written_) {
        const int ret = av_write_trailer(format_ctx_);
        if (ret < 0) {
            LOG_WARN("av_write_trailer failed: {}", AvErrorString(ret));
        }
    }

    if (format_ctx_) {
        if (format_ctx_->pb &&
            !(format_ctx_->oformat->flags & AVFMT_NOFILE)) {
            const int ret = avio_closep(&format_ctx_->pb);
            if (ret < 0) {
                LOG_WARN("avio_closep failed: {}", AvErrorString(ret));
            }
        }
        avformat_free_context(format_ctx_);
        format_ctx_ = nullptr;
    }

    video_stream_ = nullptr;
    header_written_ = false;
    output_url_.clear();
}

bool FFmpegMuxer::Write(const MediaPacket& packet) {
    if (!format_ctx_ || !video_stream_ || !header_written_ || !packet.buffer) {
        LOG_ERROR("FFmpegMuxer is not open or packet.buffer is null");
        return false;
    }

    // 当前 FFmpegMuxer 只创建了一个视频输出流，因此不能把音频包或未知类型
    // 的包写入这里。后续扩展多轨时，应由上层先完成 track 到 AVStream 的映射。
    if (packet.type != MediaType::VIDEO) {
        LOG_ERROR("FFmpegMuxer only supports video packets");
        return false;
    }

    // MediaPacket 的时间戳单位由 packet.time_base 描述。编码器通常使用
    // 1/fps，例如 1/25；而输出容器可能在写 header 时把 AVStream::time_base
    // 调整为 1/90000 或其它值，所以写包时必须读取 video_stream_->time_base。
    if (!IsValidTimeBase(packet.time_base) ||
        !IsValidTimeBase(Rational{video_stream_->time_base.num,
                                  video_stream_->time_base.den})) {
        LOG_ERROR("FFmpegMuxer received an invalid packet or stream time base");
        return false;
    }

    // 这里采用“消费式写入”语义：编码器产生的 MediaPacket 已经通过
    // FFmpegPacketBuffer 持有 AVPacket，Muxer 不再复制 AVPacket 或其 payload。
    // av_interleaved_write_frame() 会接管/消费这个 AVPacket 的数据引用，返回
    // 后原 packet 不可再次写入，也不能拿同一个 packet 重试。
    if (packet.backend.type != BackendHandle::FFMPEG ||
        packet.backend.ptr == nullptr) {
        LOG_ERROR("FFmpegMuxer requires a packet backed by an AVPacket");
        return false;
    }

    auto* av_packet = static_cast<AVPacket*>(packet.backend.ptr);
    if (av_packet->data == nullptr || av_packet->size <= 0) {
        LOG_ERROR("FFmpegMuxer received an empty AVPacket");
        return false;
    }

    // MediaPacket 是跨模块传递的统一元数据，使用它覆盖 AVPacket 中的时间戳，
    // 避免两个表示不一致时，Muxer 靠一个未说明来源的值写包。
    av_packet->pts = IsValidTimestamp(packet.pts) ? packet.pts : AV_NOPTS_VALUE;
    av_packet->dts = IsValidTimestamp(packet.dts) ? packet.dts : AV_NOPTS_VALUE;
    av_packet->duration = IsValidTimestamp(packet.duration)
        ? packet.duration
        : AV_NOPTS_VALUE;

    // stream_index 必须是当前 AVFormatContext 的输出流索引。它不一定等于
    // 上游输入流索引；本简化版只有一个输出视频流，直接使用该流的 index。
    av_packet->stream_index = video_stream_->index;
    av_packet->pos = -1;
    if (packet.keyframe) {
        av_packet->flags |= AV_PKT_FLAG_KEY;
    } else {
        av_packet->flags &= ~AV_PKT_FLAG_KEY;
    }

    // av_packet_rescale_ts() 会同时转换 pts、dts 和 duration，并正确保留
    // AV_NOPTS_VALUE。转换关系是：
    //
    //   目标 tick = 源 tick * 源 time_base / 目标 time_base
    //
    // 例如源为 1/25、pts=1，目标为 1/90000，则结果为 3600。
    av_packet_rescale_ts(
        av_packet,
        AVRational{packet.time_base.num, packet.time_base.den},
        video_stream_->time_base);

    const int ret = av_interleaved_write_frame(format_ctx_, av_packet);
    if (ret < 0) {
        LOG_ERROR("av_interleaved_write_frame failed: {}", AvErrorString(ret));
        return false;
    }

    return true;
}
