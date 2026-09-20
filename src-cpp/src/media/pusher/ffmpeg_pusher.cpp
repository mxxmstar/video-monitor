#include "media/pusher/ffmpeg_pusher.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <utility>

namespace {

/// @brief 统一构造 Pusher 失败结果，避免每个分支遗漏错误分类。
PusherResult MakeFailure(PusherErrorCategory category, std::string message, bool retryable = false) {
    return PusherResult::Failed(PusherError{category, std::move(message), retryable});
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::optional<std::string> UrlScheme(const std::string& url) {
    const auto colon = url.find(':');
    if (colon == std::string::npos || colon == 0) return std::nullopt;

    // A Windows drive path is a local file, not a one-character URI scheme.
    if (colon == 1 && std::isalpha(static_cast<unsigned char>(url[0])) &&
        url.size() > 2 && (url[2] == '\\' || url[2] == '/')) {
        return std::nullopt;
    }

    if (!std::isalpha(static_cast<unsigned char>(url[0]))) return std::nullopt;
    for (std::size_t index = 1; index < colon; ++index) {
        const unsigned char c = static_cast<unsigned char>(url[index]);
        if (!std::isalnum(c) && c != '+' && c != '-' && c != '.') return std::nullopt;
    }
    return ToLowerAscii(url.substr(0, colon));
}

bool IsLocalFileOutput(const std::string& output_url) {
    const auto scheme = UrlScheme(output_url);
    return !scheme.has_value() || *scheme == "file";
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

bool IsRtspScheme(const std::optional<std::string>& scheme) {
    return scheme.has_value() && (*scheme == "rtsp" || *scheme == "rtsps");
}

bool IsRtmpScheme(const std::optional<std::string>& scheme) {
    return scheme.has_value() && (*scheme == "rtmp" || *scheme == "rtmps");
}

/// @brief 添加 FFmpeg 输出选项
/// @details 会检查选项是否已存在，避免重复添加。
/// @param options 输出选项映射
/// @param name 选项名称
/// @param value 选项值
/// @param error 输出错误结果
/// @return 是否成功添加选项
bool AddOption(std::map<std::string, std::string>& options, const std::string& name,
               const std::string& value, PusherResult& error) {
    if (options.find(name) != options.end()) {
        error = MakeFailure(PusherErrorCategory::InvalidConfiguration,
                            "Conflicting FFmpeg output option: " + name);
        return false;
    }
    options.emplace(name, value);
    return true;
}

struct ResolvedMuxerOutput {
    MuxerOpenOptions options;
    bool network_output{false};
};

std::optional<ResolvedMuxerOutput> ResolveMuxerOutput(const PusherConfig& config,
                                                       PusherResult& error) {
    const auto scheme = UrlScheme(config.output_url);
    const bool is_rtsp = IsRtspScheme(scheme);
    const bool is_rtmp = IsRtmpScheme(scheme);

    if (config.ffmpeg.rtsp.has_value() && (!is_rtsp)) {
        error = MakeFailure(PusherErrorCategory::InvalidConfiguration,
                            "Output URL mismatch with output rtsp options");
        return std::nullopt;
    }
    if (config.ffmpeg.rtmp.has_value() && (!is_rtmp)) {
        error = MakeFailure(PusherErrorCategory::InvalidConfiguration,
                            "Output URL mismatch with output rtmp options");
        return std::nullopt;
    }
    if (is_rtsp && !config.ffmpeg.extra_io_options.empty()) {
        error = MakeFailure(PusherErrorCategory::InvalidConfiguration,
                            "RTSP output does not accept AVIO output options");
        return std::nullopt;
    }

    ResolvedMuxerOutput resolved;
    resolved.options.output_url = config.output_url;
    resolved.options.io = {config.io.connect_timeout, config.io.write_timeout};
    resolved.options.normalize_timestamps = IsLocalFileOutput(config.output_url);
    resolved.network_output = IsNetworkOutput(config.output_url);

    if (config.ffmpeg.output_format.has_value()) {
        if (config.ffmpeg.output_format->empty()) {
            error = MakeFailure(PusherErrorCategory::InvalidConfiguration,
                                "FFmpeg output_format cannot be empty when specified");
            return std::nullopt;
        }
        resolved.options.format_name = ToLowerAscii(*config.ffmpeg.output_format);
    } else if (is_rtsp) {
        resolved.options.format_name = "rtsp";
    } else if (is_rtmp) {
        resolved.options.format_name = "flv";
    }

    const std::string required_format = is_rtsp ? "rtsp" : (is_rtmp ? "flv" : "");
    if (!required_format.empty() && !resolved.options.format_name.empty() &&
        resolved.options.format_name != required_format) {
        error = MakeFailure(PusherErrorCategory::InvalidConfiguration,
                            "The selected output format is incompatible with the output URL");
        return std::nullopt;
    }

    resolved.options.io_options = config.ffmpeg.extra_io_options;
    resolved.options.muxer_options = config.ffmpeg.extra_muxer_options;

    if (config.ffmpeg.rtsp.has_value()) {
        const std::string transport = ToLowerAscii(config.ffmpeg.rtsp->transport);
        if (transport != "tcp" && transport != "udp") {
            error = MakeFailure(PusherErrorCategory::InvalidConfiguration,
                                "RTSP transport must be tcp or udp");
            return std::nullopt;
        }
        if (!AddOption(resolved.options.muxer_options, "rtsp_transport", transport, error)) {
            return std::nullopt;
        }
    }

    if (config.ffmpeg.rtmp.has_value()) {
        const RtmpOutputOptions& rtmp = *config.ffmpeg.rtmp;
        if (rtmp.app.has_value() &&
            !AddOption(resolved.options.io_options, "rtmp_app", *rtmp.app, error)) {
            return std::nullopt;
        }
        if (rtmp.playpath.has_value() &&
            !AddOption(resolved.options.io_options, "rtmp_playpath", *rtmp.playpath, error)) {
            return std::nullopt;
        }
        if (rtmp.tcp_nodelay.has_value() &&
            !AddOption(resolved.options.io_options, "tcp_nodelay", *rtmp.tcp_nodelay ? "1" : "0", error)) {
            return std::nullopt;
        }
    }

    return resolved;
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
                code == AVERROR_OPTION_NOT_FOUND ||
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

    PusherResult resolve_error;
    const auto resolved_output = ResolveMuxerOutput(config, resolve_error);
    if (!resolved_output.has_value()) return resolve_error;

    network_output_ = resolved_output->network_output;
    MuxerResult muxer_result = muxer_.Open(resolved_output->options, config.video_track);
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
