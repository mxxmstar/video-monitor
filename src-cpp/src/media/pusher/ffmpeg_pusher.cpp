#include "media/pusher/ffmpeg_pusher.h"

#include <cerrno>
#include <cstring>
#include <utility>

namespace {

/// @brief 统一构造 Pusher 失败结果，避免每个分支遗漏错误分类。
PusherResult MakeFailure(PusherErrorCategory category, std::string message, bool retryable = false) {
    return PusherResult::Failed(PusherError{category, std::move(message), retryable});
}

bool IsNetworkOutput(const std::string& output_url) {
    const char* protocol = avio_find_protocol_name(output_url.c_str());
    bool network_output = false;
    for (const char* name : {"tcp", "udp", "http", "https", "rtmp", "rtmps",
                             "rtmpt", "rtmpts", "srt", "rist"}) {
        if (protocol && std::strcmp(protocol, name) == 0) network_output = true;
    }
    // RTSP is an AVFMT_NOFILE muxer, not an AVIO protocol.
    if (output_url.rfind("rtsp://", 0) == 0 || output_url.rfind("rtsps://", 0) == 0) {
        network_output = true;
    }
    return network_output;
}

}  // namespace

/// @brief 将 Muxer 中的错误映射到 Pusher 中的错误分类
/// @param error Muxer 中的错误
/// @param network_output 是否为网络输出
/// @return Pusher 中的错误分类
PusherResult MapMuxerError(const MuxerError& error, bool network_output) {
    using Category = PusherErrorCategory;
    Category category = Category::Internal;
    const char* operation = "unknown";
    switch (error.operation) {
    case MuxerOperation::AllocateContext: operation = "AllocateContext"; category = Category::OpenFailed; break;
    case MuxerOperation::CreateStream: operation = "CreateStream"; category = Category::OpenFailed; break;
    case MuxerOperation::ConfigureStream: operation = "ConfigureStream"; category = Category::OpenFailed; break;
    case MuxerOperation::OpenIo: operation = "OpenIo"; category = Category::OpenFailed; break;
    case MuxerOperation::WriteHeader: operation = "WriteHeader"; category = Category::OpenFailed; break;
    case MuxerOperation::WritePacket: operation = "WritePacket"; category = Category::WriteFailed; break;
    case MuxerOperation::WriteTrailer: operation = "WriteTrailer"; category = Category::CloseFailed; break;
    case MuxerOperation::CloseIo: operation = "CloseIo"; category = Category::CloseFailed; break;
    }
    const int code = error.native_code;
    bool retryable = false;
    if (error.category == MuxerErrorCategory::Cancelled) {
        category = Category::Cancelled;
    } else if (error.category == MuxerErrorCategory::Timeout || code == AVERROR(ETIMEDOUT)) {
        category = Category::Timeout;
        retryable = network_output;
    } else if (code == AVERROR_EXIT) {
        category = Category::Cancelled;
    } else if (error.category == MuxerErrorCategory::Internal || code == AVERROR(ENOMEM)) {
        category = Category::Internal;
    } else if (code == AVERROR_HTTP_UNAUTHORIZED || code == AVERROR_HTTP_FORBIDDEN ||
               code == AVERROR(EACCES) || code == AVERROR(EPERM)) {
        category = Category::Authentication;
    } else if (code == AVERROR_HTTP_NOT_FOUND || code == AVERROR(ENOENT)) {
        category = Category::NotFound;
    } else if (code == AVERROR_PROTOCOL_NOT_FOUND) {
        category = Category::UnsupportedProtocol;
    } else if (code == AVERROR_MUXER_NOT_FOUND) {
        category = Category::UnsupportedMedia;
    } else if ((code == AVERROR(EINVAL) || code == AVERROR_INVALIDDATA ||
                code == AVERROR_HTTP_BAD_REQUEST || code == AVERROR_HTTP_OTHER_4XX) &&
               category == Category::OpenFailed) {
        category = Category::InvalidConfiguration;
    } else if (network_output &&
               (code == AVERROR(EAGAIN) || code == AVERROR(EIO) ||
                code == AVERROR(ECONNRESET) || code == AVERROR(ECONNREFUSED) ||
                code == AVERROR(ECONNABORTED) || code == AVERROR(ENETDOWN) ||
                code == AVERROR(ENETUNREACH) || code == AVERROR(EHOSTUNREACH) ||
                code == AVERROR(EPIPE) || code == AVERROR_EOF ||
                code == AVERROR_HTTP_TOO_MANY_REQUESTS || code == AVERROR_HTTP_SERVER_ERROR)) {
        category = Category::Network;
        retryable = true;
    }
    // Closing is finalization, not an invitation to create another output.
    if (error.operation == MuxerOperation::WriteTrailer || error.operation == MuxerOperation::CloseIo) {
        retryable = false;
    }
    return MakeFailure(category, std::string(operation) + " [" + std::to_string(code) + "]: " + error.message, retryable);
}

FFmpegPusher::~FFmpegPusher() {
    // 显式关闭使得 Pusher 的生命周期契约独立于 FFmpegMuxer 的析构细节。
    // Close() 是幂等的，因此用户已主动 Close() 时这里不会重复写 trailer。
    Close();
}

PusherResult FFmpegPusher::Open(const PusherConfig& config) {
    // 一个 Pusher 同一时刻只对应一个输出目标。
    // 重复 Open 时先完整结束旧的容器，避免新输出错误地复用旧的 AVFormatContext 或 AVIOContext。
    const auto close_result = Close();
    if (!close_result.Succeed()) return close_result;

    if (!config.is_valid()) {
        return MakeFailure(PusherErrorCategory::InvalidConfiguration,
                           "FFmpegPusher received an invalid output configuration");
    }

    // 在触及底层输出之前明确检查媒体能力。
    if (config.video_track.codec_type != CodecType::H264) {
        return MakeFailure(PusherErrorCategory::UnsupportedMedia,
                           "FFmpegPusher currently supports H264 video only");
    }

    network_output_ = IsNetworkOutput(config.output_url);
    MuxerOptions muxer_op = {};
    if (config.ffmpeg.rtsp.has_value()) {
        muxer_op.protocol = "rtsp";
        muxer_op.extra_muxer_options["transport"] = config.ffmpeg.rtsp->transport;
    } else if (config.ffmpeg.rtmp.has_value()) {
        // muxer_op["app"] = config.ffmpeg.rtmp->app;
        // muxer_op["playpath"] = config.ffmpeg.rtmp->playpath;
        // muxer_op["tcp_nodelay"] = config.ffmpeg.rtmp->tcp_nodelay;
    }

    for (const auto& option : config.ffmpeg.extra_muxer_options) {
        muxer_op.extra_muxer_options[option.first] = option.second;
    }
    MuxerResult muxer_result = muxer_.Open(config.output_url, config.video_track,
        MuxerIoOptions{config.io.connect_timeout, config.io.write_timeout}, muxer_op);
    if (!muxer_result.Succeed()) {
        return MapMuxerError(*muxer_result.error, network_output_);
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

    const auto* av_packet = static_cast<const AVPacket*>(packet.backend.ptr);
    if (!av_packet->data || av_packet->size <= 0 ||
        (IsValidTimestamp(packet.duration) && packet.duration < 0)) {
        return MakeFailure(PusherErrorCategory::InvalidPacket, "Invalid AVPacket payload or duration");
    }

    // FFmpegMuxer 使用消费式写入：成功进入 av_interleaved_write_frame()
    // 后，packet.backend.ptr 指向的 AVPacket 不能再被当前调用方重试或复用。
    // 未来 PusherSession 遇到写失败时必须等待下一关键帧，或将 Muxer 改为
    // 非消费式写入后再实现重试。
    const auto result = muxer_.Write(packet);
    if (!result.Succeed()) {
        opened_ = false;
        return MapMuxerError(*result.error, network_output_);
    }

    return PusherResult::Success();
}

PusherResult FFmpegPusher::Close() {
    const auto result = muxer_.Close();

    opened_ = false;
    config_ = {};
    return result.Succeed() ? PusherResult::Success() : MapMuxerError(*result.error, network_output_);
}

bool FFmpegPusher::IsOpen() const {
    return opened_;
}

void FFmpegPusher::SetEventCallback(EventCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    event_cb_ = std::move(cb);
}
