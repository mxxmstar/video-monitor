#include "media/pusher/ffmpeg_pusher.h"

#include <utility>

namespace {

/// @brief 统一构造 Pusher 失败结果，避免每个分支遗漏错误分类。
PusherResult MakeFailure(PusherErrorCategory category, std::string message, bool retryable = false) {
    return PusherResult::Failed(PusherError{category, std::move(message), retryable});
}

}  // namespace

FFmpegPusher::~FFmpegPusher() {
    // 显式关闭使得 Pusher 的生命周期契约独立于 FFmpegMuxer 的析构细节。
    // Close() 是幂等的，因此用户已主动 Close() 时这里不会重复写 trailer。
    Close();
}

PusherResult FFmpegPusher::Open(const PusherConfig& config) {
    // 一个 Pusher 同一时刻只对应一个输出目标。
    // 重复 Open 时先完整结束旧的容器，避免新输出错误地复用旧的 AVFormatContext 或 AVIOContext。
    Close();

    if (!config.is_valid()) {
        return MakeFailure(PusherErrorCategory::InvalidConfiguration,
                           "FFmpegPusher received an invalid output configuration");
    }

    // Pusher 的媒体能力应明确暴露在这一层。当前 Muxer 也会做同样的保护，
    // 但此处可以向调用方返回 UnsupportedMedia，而不是模糊的打开失败。
    if (config.video_track.codec_type != CodecType::H264) {
        return MakeFailure(PusherErrorCategory::UnsupportedMedia,
                           "FFmpegPusher currently supports H264 video only");
    }

    if (!muxer_.Open(config.output_url, config.video_track)) {
        return MakeFailure(PusherErrorCategory::OpenFailed,
                           "FFmpegMuxer failed to open the output target");
    }

    config_ = config;
    opened_ = true;
    return PusherResult::Success();
}

PusherResult FFmpegPusher::Push(const MediaPacket& packet) {
    if (!opened_) {
        return MakeFailure(PusherErrorCategory::InvalidState,
                           "FFmpegPusher::Push called before a successful Open");
    }

    // 初版输出链路只建立了一条 H.264 视频轨道。音频、多轨路由和编码
    // 格式协商将随 Publisher/PusherSession 的扩展一并加入，不能在这里
    // 静默丢弃或改写不匹配的数据。
    if (packet.type != MediaType::VIDEO || packet.codec != CodecType::H264) {
        return MakeFailure(PusherErrorCategory::UnsupportedMedia,
                           "FFmpegPusher accepts H264 video packets only");
    }

    // Muxer 最终需要直接操作 AVPacket。这里先将公共 MediaPacket 契约
    // 转换为可读的错误结果，避免调用方只能从 Muxer 日志推断输入问题。
    if (!packet.buffer || packet.buffer->Size() == 0 || !IsValidTimeBase(packet.time_base) ||
        packet.backend.type != BackendHandle::FFMPEG || packet.backend.ptr == nullptr) {
        return MakeFailure(PusherErrorCategory::InvalidPacket,
                           "FFmpegPusher received an invalid FFmpeg media packet");
    }

    // FFmpegMuxer 使用消费式写入：成功进入 av_interleaved_write_frame()
    // 后，packet.backend.ptr 指向的 AVPacket 不能再被当前调用方重试或复用。
    // 未来 PusherSession 遇到写失败时必须等待下一关键帧，或将 Muxer 改为
    // 非消费式写入后再实现重试。
    if (!muxer_.Write(packet)) {
        return MakeFailure(PusherErrorCategory::WriteFailed,
                           "FFmpegMuxer failed to write the media packet");
    }

    return PusherResult::Success();
}

PusherResult FFmpegPusher::Close() {
    if (opened_) {
        muxer_.Close();
    }

    opened_ = false;
    config_ = {};
    return PusherResult::Success();
}

bool FFmpegPusher::IsOpen() const {
    return opened_;
}
