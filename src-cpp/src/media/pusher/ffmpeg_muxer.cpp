#include "media/pusher/ffmpeg_muxer.h"
#include "common/log/logger.h"
#include "media/pusher/pusher_config.h"
#include <cerrno>
#include <cstring>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/dict.h>
}
namespace {
/// @brief 获取 FFmpeg 错误码对应的错误字符串
std::string AvErrorString(int ret) {
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_make_error_string(buf, AV_ERROR_MAX_STRING_SIZE, ret);
    return buf;
}

/// @brief 根据输出 URL 判断是否为本地文件。
///
/// 相对路径和绝对路径按文件处理；带网络协议的 URL 不做时间戳归零。
bool IsFileOutputUrl(const std::string& url) {
    const char* protocol = avio_find_protocol_name(url.c_str());
    return protocol != nullptr && std::strcmp(protocol, "file") == 0;
}

bool IsRtspOutputUrl(const std::string& url) {
    return url.find("rtsp") != std::string::npos;
}

}


FFmpegMuxer::FFmpegMuxer() {

}

FFmpegMuxer::~FFmpegMuxer() {
    Close();
}



MuxerResult FFmpegMuxer::Open(const std::string& output_url, const MediaTrackConfig& config, 
    const MuxerIoOptions& io, const MuxerOptions& muxer_options) {
    // Open 可以重复调用。先关闭旧的输出，保证旧的 AVIO 和 AVFormatContext
    // 不会泄漏，也避免新的 stream 挂到旧 context 上。
    const auto close_result = Close();
    if (!close_result.Succeed()) return close_result;

    io_ = io;
    interrupt_ctx_.stop_requested.store(false);
    interrupt_ctx_.timed_out.store(false);
    output_url_ = output_url;
    normalize_timestamps_ = IsFileOutputUrl(output_url_);
    timestamp_offset_set_ = false;
    timestamp_offset_ = 0;

    // 分配 AVFormatContext
    // 第三个参数 format_name：对于 RTSP/RTMP 等无扩展名的 URL，需要手动指定格式
    const char* format_name = nullptr;
    // RTSP/RTMP 是 AVFMT_NOFILE muxer，avio_find_protocol_name 检测不到，需要手动判断
    if (IsRtspOutputUrl(output_url_)) {
        format_name = "rtsp";
    } else if (output_url_.rfind("rtmp://", 0) == 0 || output_url_.rfind("rtmps://", 0) == 0) {
        format_name = "flv";
    }
    
    int ret = avformat_alloc_output_context2(&format_ctx_, nullptr, format_name, output_url_.c_str());
    if (ret < 0 || !format_ctx_) {
        LOG_ERROR("avformat_alloc_output_context2 failed: {}", AvErrorString(ret));
        Close();
        return failure(MuxerOperation::AllocateContext, MuxerErrorCategory::OpenFailed, ret < 0 ? ret : AVERROR(ENOMEM));
    }

    // 创建视频流
    video_stream_ = avformat_new_stream(format_ctx_, nullptr);
    if (!video_stream_) {
        LOG_ERROR("avformat_new_stream failed: codec not found");
        Close();
        return failure(MuxerOperation::CreateStream, MuxerErrorCategory::Internal, AVERROR(ENOMEM));
    }

    // 配置视频流参数
    auto video_config = config.video();
    video_stream_->codecpar->codec_id = AV_CODEC_ID_H264;
    video_stream_->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    video_stream_->codecpar->width = video_config.width;
    video_stream_->codecpar->height = video_config.height;
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
            return failure(MuxerOperation::ConfigureStream, MuxerErrorCategory::Internal, AVERROR(ENOMEM));
        }
        std::memcpy(video_stream_->codecpar->extradata, config.extra_data.data(), extra_data_size);
        video_stream_->codecpar->extradata_size = static_cast<int>(extra_data_size);
    }

    // 设置中断回调函数，用于在写入时检查是否需要停止或超时写入
    format_ctx_->interrupt_callback.callback = [](void* opaque) -> int {
        auto* context = static_cast<InterruptContext*>(opaque);
        if (context->stop_requested.load()) {
            return 1;   ///< 返回 1 强制停止读取
        }
        if (std::chrono::steady_clock::now() >= context->deadline) {
            context->timed_out.store(true);
            return 1;
        }
        return 0;
    };
    format_ctx_->interrupt_callback.opaque = &interrupt_ctx_;

    beginOperation(io_.open_timeout);
    if (!(format_ctx_->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open2(&format_ctx_->pb, output_url_.c_str(), AVIO_FLAG_WRITE, &format_ctx_->interrupt_callback, nullptr);
        if (ret < 0) {
            auto result = failure(MuxerOperation::OpenIo, MuxerErrorCategory::OpenFailed, ret);
            Close();
            return result;
        }
    }

    AVDictionary* options = nullptr;

    for (const auto& option : muxer_options.extra_muxer_options) {
        ret = av_dict_set(&options, option.first.c_str(), option.second.c_str(), 0);
        if (ret < 0) {
            // auto result = failure(MuxerOperation::SetOption, MuxerErrorCategory::OpenFailed, ret);
            // Close();
            // return result;
            LOG_WARN("av_dict_set option [{}, {}] failed: {}", option.first, option.second, AvErrorString(ret));
        }
    }


    // 打开过程包括由 write_header（例如 RTSP）执行的协议协商。
    ret = avformat_write_header(format_ctx_, nullptr);
    if (ret < 0) {
        auto result = failure(MuxerOperation::WriteHeader, MuxerErrorCategory::OpenFailed, ret);
        Close();
        return result;
    }
    header_written_ = true;

    LOG_INFO("Muxer Opend: {}", output_url_);
    LOG_INFO("Video Config: {}x{} {}fps", video_config.width, video_config.height, video_config.fps);
    return MuxerResult::Success();
}

MuxerResult FFmpegMuxer::Close() {
    auto result = MuxerResult::Success();
    beginOperation(io_.write_timeout);
    if (format_ctx_ && header_written_) {
        const int ret = av_write_trailer(format_ctx_);
        if (ret < 0) {
            result = failure(MuxerOperation::WriteTrailer, MuxerErrorCategory::CloseFailed, ret);
        }
    }

    if (format_ctx_) {
        if (format_ctx_->pb &&
            !(format_ctx_->oformat->flags & AVFMT_NOFILE)) {
            beginOperation(io_.write_timeout);
            const int ret = avio_closep(&format_ctx_->pb);
            if (ret < 0) {
                auto close_result = failure(MuxerOperation::CloseIo, MuxerErrorCategory::CloseFailed, ret);
                if (result.Succeed()) result = std::move(close_result);
            }
        }
        avformat_free_context(format_ctx_);
        format_ctx_ = nullptr;
    }

    video_stream_ = nullptr;
    header_written_ = false;
    normalize_timestamps_ = false;
    timestamp_offset_set_ = false;
    timestamp_offset_ = 0;
    output_url_.clear();
    return result;
}

MuxerResult FFmpegMuxer::Write(const MediaPacket& packet) {
    if (!format_ctx_ || !video_stream_ || !header_written_) {
        LOG_ERROR("FFmpegMuxer is not open");
        return failure(MuxerOperation::WritePacket, MuxerErrorCategory::Internal, AVERROR(EINVAL));
    }

    // packet 的校验（buffer、time_base、backend 类型、AVPacket 数据等）已由
    // FFmpegPusher::Push 完成。Muxer 只负责纯粹的写入操作，不再重复校验。

    if (interrupt_ctx_.stop_requested.load()) {
        return failure(MuxerOperation::WritePacket, MuxerErrorCategory::Cancelled, AVERROR_EXIT);
    }
    auto* av_packet = static_cast<AVPacket*>(packet.backend.ptr);

    // MediaPacket 是跨模块传递的统一元数据，使用它覆盖 AVPacket 中的时间戳，
    // 避免两个表示不一致时，Muxer 靠一个未说明来源的值写包。
    av_packet->pts = IsValidTimestamp(packet.pts) ? packet.pts : AV_NOPTS_VALUE;
    av_packet->dts = IsValidTimestamp(packet.dts) ? packet.dts : AV_NOPTS_VALUE;
    av_packet->duration = IsValidTimestamp(packet.duration) ? packet.duration : 0;

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
    av_packet_rescale_ts(av_packet, AVRational{packet.time_base.num, packet.time_base.den}, video_stream_->time_base);

    if (normalize_timestamps_) {
        // 偏移量保存在输出流时间基中，后续包即使使用不同的输入时间基，
        // 也能在统一刻度下归零。只使用第一包决定偏移；若它没有有效
        // PTS/DTS，则偏移保持为 0，不能在后续包中途改变时间轴。
        if (!timestamp_offset_set_) {
            timestamp_offset_ = 0;
            // 拿到第一个有效的 PTS，使用它作为偏移量
            // I P帧的PTS >= DTS
            if (av_packet->pts != AV_NOPTS_VALUE) {
                timestamp_offset_ = av_packet->pts;
            }
            if (av_packet->dts != AV_NOPTS_VALUE &&
                (av_packet->pts == AV_NOPTS_VALUE || av_packet->dts < timestamp_offset_)) {
                timestamp_offset_ = av_packet->dts;
            }
            timestamp_offset_set_ = true;

            if (av_packet->pts != AV_NOPTS_VALUE || av_packet->dts != AV_NOPTS_VALUE) {
                LOG_INFO("FFmpegMuxer normalizes local-file timestamps by {} stream tick(s)", timestamp_offset_);
            }
        }

        if (av_packet->pts != AV_NOPTS_VALUE) {
            av_packet->pts -= timestamp_offset_;
        }
        if (av_packet->dts != AV_NOPTS_VALUE) {
            av_packet->dts -= timestamp_offset_;
        }
    }

    beginOperation(io_.write_timeout);
    const int ret = av_interleaved_write_frame(format_ctx_, av_packet);
    if (ret < 0) {
        return failure(MuxerOperation::WritePacket, MuxerErrorCategory::WriteFailed, ret);
    }
    return MuxerResult::Success();
}

void FFmpegMuxer::RequestStop() {
    interrupt_ctx_.stop_requested.store(true);
}

void FFmpegMuxer::beginOperation(std::chrono::milliseconds timeout) {
    interrupt_ctx_.timed_out.store(false);
    const auto now = std::chrono::steady_clock::now();
    // 防止溢出
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::time_point::max() - now);
    interrupt_ctx_.deadline = timeout.count() > 0 && timeout < remaining
        ? now + timeout : std::chrono::steady_clock::time_point::max();
    // if (timeout.count() > 0) {
    //     interrupt_ctx_.deadline = now + timeout;
    // } else {
    //     interrupt_ctx_.deadline = now + std::chrono::milliseconds::max();
    // }
}

MuxerResult FFmpegMuxer::failure(MuxerOperation operation, MuxerErrorCategory category, int code) {
    if (interrupt_ctx_.stop_requested.load()) {
        category = MuxerErrorCategory::Cancelled;
    } else if (interrupt_ctx_.timed_out.load()) {
        category = MuxerErrorCategory::Timeout;
    }
    return MuxerResult::Failed({category, code, AvErrorString(code), operation});
}