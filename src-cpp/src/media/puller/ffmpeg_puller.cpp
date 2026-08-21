#include "media/puller/ffmpeg_puller.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <string>
#include <utility>

#include "common/log/logger.h"
#include "media/ffmpeg_packet_buffer.h"

extern "C" {
#include <libavutil/error.h>
}

namespace {

bool IsRtspUri(const std::string& uri) {
    const auto separator = uri.find("://");
    if (separator == std::string::npos) {
        return false;
    }

    std::string scheme = uri.substr(0, separator);
    std::transform(
        scheme.begin(), scheme.end(), scheme.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return scheme == "rtsp" || scheme == "rtsps";
}

std::string FfmpegErrorText(int error_code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error_code, buffer, sizeof(buffer));
    return buffer;
}

PullOpenResult OpenFailure(PullErrorCategory category,
    int native_code,
    std::string message,
    bool retryable) {
    return PullOpenResult::Failed(PullError{
        category,
        native_code,
        std::move(message),
        retryable,
    });
}

void SetBoolOption(AVDictionary** options, const char* name, bool value) {
    av_dict_set(options, name, value ? "1" : "0", 0);
}

void BuildRtspOptions(AVDictionary** options, const RtspInputOptions& rtsp) {
    if (rtsp.transport.has_value()) {
        av_dict_set(options, "rtsp_transport", rtsp.transport->c_str(), 0);
    }
    if (rtsp.prefer_tcp.has_value() && *rtsp.prefer_tcp) {
        av_dict_set(options, "rtsp_flags", "prefer_tcp", 0);
    }
    if (rtsp.socket_timeout.has_value()) {
        av_dict_set_int(options, "timeout", rtsp.socket_timeout->count(), 0);
    }
    if (rtsp.reorder_queue_size.has_value()) {
        av_dict_set_int(
            options,
            "reorder_queue_size",
            *rtsp.reorder_queue_size,
            0);
    }
    if (rtsp.receive_buffer_bytes.has_value()) {
        av_dict_set_int(
            options,
            "buffer_size",
            *rtsp.receive_buffer_bytes,
            0);
    }
    if (rtsp.min_port.has_value()) {
        av_dict_set_int(options, "min_port", *rtsp.min_port, 0);
    }
    if (rtsp.max_port.has_value()) {
        av_dict_set_int(options, "max_port", *rtsp.max_port, 0);
    }
    if (rtsp.user_agent.has_value()) {
        av_dict_set(options, "user_agent", rtsp.user_agent->c_str(), 0);
    }
    if (rtsp.ca_file.has_value()) {
        av_dict_set(options, "ca_file", rtsp.ca_file->c_str(), 0);
    }
    if (rtsp.tls_verify.has_value()) {
        SetBoolOption(options, "tls_verify", *rtsp.tls_verify);
    }
    if (rtsp.username.has_value()) {
        av_dict_set(options, "user", rtsp.username->c_str(), 0);
    }
    if (rtsp.password.has_value()) {
        av_dict_set(options, "password", rtsp.password->c_str(), 0);
    }
}

} // namespace

FFmpegPuller::FFmpegPuller(FFmpegPullerConfig config)
    : config_(std::move(config)) {
}

FFmpegPuller::~FFmpegPuller() {
    Close();
}

PullOpenResult FFmpegPuller::Open(const InputEndpointConfig& endpoint) {
    Close();

    if (endpoint.uri.empty()) {
        return OpenFailure(PullErrorCategory::InvalidConfiguration, 0,
            "FFmpeg input URI is empty", false);
    }

    if (endpoint.puller_kind != PullerKind::FFmpeg) {
        return OpenFailure(PullErrorCategory::UnsupportedProtocol, 0,
            "FFmpegPuller cannot open this type of endpoint", false);
    }

    fmt_ctx_ = avformat_alloc_context();
    if (!fmt_ctx_) {
        return OpenFailure(
            PullErrorCategory::Internal, AVERROR(ENOMEM),
            "avformat_alloc_context failed", false);
    }

    interrupt_ctx_.stop_requested.store(false);
    interrupt_ctx_.timed_out.store(false);
    if (config_.io.connect_timeout.count() > 0) {
        interrupt_ctx_.deadline = std::chrono::steady_clock::now() + config_.io.connect_timeout;
    } else {
        interrupt_ctx_.deadline = std::chrono::steady_clock::time_point::max();
    }

    fmt_ctx_->interrupt_callback.callback = [](void* opaque) -> int {
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
    fmt_ctx_->interrupt_callback.opaque = &interrupt_ctx_;

    AVDictionary* options = nullptr;

    if (config_.probe.probe_size_bytes.has_value()) {
        av_dict_set_int(&options, "probesize", *config_.probe.probe_size_bytes, 0);
    }
    if (config_.probe.analyze_duration.has_value()) {
        av_dict_set_int(&options, "analyzeduration", config_.probe.analyze_duration->count(), 0);
    }

    if (config_.latency == LatencyMode::Low) {
        av_dict_set(&options, "fflags", "nobuffer", 0);
        av_dict_set(&options, "flags", "low_delay", 0);
    }

    // RTSP 协议选项
    if (IsRtspUri(endpoint.uri) && config_.rtsp.has_value()) {
        const RtspInputOptions& rtsp = *config_.rtsp;
        BuildRtspOptions(&options, rtsp);        
    }

    // 扩展选项
    for (const auto& [name, value] : config_.extra_av_options) {
        av_dict_set(&options, name.c_str(), value.c_str(), 0);
    }

    const AVInputFormat* input_format = nullptr;
    if (config_.input_format.has_value()) {
        input_format = av_find_input_format(config_.input_format->c_str());
        if (!input_format) {
            // 输入格式未知
            av_dict_free(&options);
            const auto result = OpenFailure(
                PullErrorCategory::InvalidConfiguration, 0,
                "unknown FFmpeg input format: " + *config_.input_format, false);
            Close();
            return result;
        }
    }

    const int open_result = avformat_open_input(
        &fmt_ctx_,
        endpoint.uri.c_str(),
        input_format,
        &options);

    // 检查是否有未被消耗的选项
    for (AVDictionaryEntry* entry = nullptr;
         (entry = av_dict_get(options, "", entry, AV_DICT_IGNORE_SUFFIX)) != nullptr;) {
        LOG_WARN("FFmpeg option was not consumed: {}", entry->key);
    }
    av_dict_free(&options);

    if (open_result < 0) {
        const bool timed_out = interrupt_ctx_.timed_out.load();
        const auto result = OpenFailure(
            timed_out ? PullErrorCategory::Timeout : PullErrorCategory::Network,
            open_result,
            "avformat_open_input failed: " + FfmpegErrorText(open_result),
            timed_out);
        Close();
        return result;
    }

    interrupt_ctx_.timed_out.store(false); 
    if (config_.io.connect_timeout.count() > 0) {
        interrupt_ctx_.deadline = std::chrono::steady_clock::now() + config_.io.connect_timeout;
    } else {
        interrupt_ctx_.deadline = std::chrono::steady_clock::time_point::max();
    }

    const int stream_info_result = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (stream_info_result < 0) {
        const bool timed_out = interrupt_ctx_.timed_out.load();
        const auto result = OpenFailure(
            timed_out ? PullErrorCategory::Timeout : PullErrorCategory::InvalidMedia,
            stream_info_result,
            "avformat_find_stream_info failed: " + FfmpegErrorText(stream_info_result),
            timed_out);
        Close();
        return result;
    }

    MultiStreamInfo discovered_info;
    for (unsigned int index = 0; index < fmt_ctx_->nb_streams; ++index) {
        AVStream* stream = fmt_ctx_->streams[index];
        AVCodecParameters* codecpar = stream->codecpar;

        if (codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }

        const int stream_index = static_cast<int>(index);
        codecpars_[stream_index] = codecpar;

        MediaStreamInfo info;
        info.stream_index = stream_index;
        info.codec_type = MapCodecID(codecpar->codec_id);
        info.time_base = {stream->time_base.num, stream->time_base.den};

        if (codecpar->extradata && codecpar->extradata_size > 0) {
            info.extra_data.assign(
                codecpar->extradata,
                codecpar->extradata + codecpar->extradata_size);
        }

        const int info_index = static_cast<int>(discovered_info.stream_infos.size());
        if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            info.media_type = MediaType::VIDEO;
            VideoStreamInfo video;
            video.width = codecpar->width;
            video.height = codecpar->height;
            if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
                video.fps = static_cast<float>(stream->avg_frame_rate.num) /
                            static_cast<float>(stream->avg_frame_rate.den);
            } else if (stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0) {
                video.fps = static_cast<float>(stream->r_frame_rate.num) /
                            static_cast<float>(stream->r_frame_rate.den);
            }
            info.detail = video;
            discovered_info.video_stream_idx_ = info_index;
        } else {
            info.media_type = MediaType::AUDIO;
            AudioStreamInfo audio;
            audio.sample_rate = codecpar->sample_rate;
            audio.channels = codecpar->ch_layout.nb_channels;
            audio.channel_layout = codecpar->ch_layout.u.mask;
            info.detail = audio;
            discovered_info.audio_stream_idx_ = info_index;
        }

        discovered_info.stream_infos.push_back(std::move(info));        
    }
    discovered_info.DumpStreamInfo();

    if (!discovered_info.HasVideoStream() && !discovered_info.HasAudioStream()) {
        const auto result = OpenFailure(
            PullErrorCategory::InvalidMedia,
            AVERROR_INVALIDDATA,
            "input contains no audio or video stream",
            false);
        Close();
        return result;
    }

    {
        std::lock_guard<std::mutex> lock(info_mutex_);
        cached_info_ = discovered_info;
    }

    // 为缺失视频尺寸的流准备探测上下文
    for (const auto& info : discovered_info.stream_infos) {
        if (info.media_type != MediaType::VIDEO ||
            !std::holds_alternative<VideoStreamInfo>(info.detail)) {
            continue;
        }

        const auto& video = info.get_detail<VideoStreamInfo>();
        if (video.width > 0 && video.height > 0) {
            continue;
        }

        auto probe = std::make_unique<VideoProbeContext>();
        probe->started = std::chrono::steady_clock::now();
        const auto codec = codecpars_.find(info.stream_index);
        if (codec != codecpars_.end()) {
            probe->parser = av_parser_init(codec->second->codec_id);
            probe->codec = avcodec_alloc_context3(nullptr);
            if (probe->codec) {
                avcodec_parameters_to_context(probe->codec, codec->second);
            }
        }
        video_probes_[info.stream_index] = std::move(probe);
    }

    interrupt_ctx_.stop_requested.store(false);
    return PullOpenResult::Success();
}

void FFmpegPuller::Close() {
    interrupt_ctx_.stop_requested.store(true);

    std::lock_guard<std::mutex> io_lock(io_mutex_);
    resetVideoProbes();

    if (fmt_ctx_) {
        avformat_close_input(&fmt_ctx_);
    }
    codecpars_.clear();

    {
        std::lock_guard<std::mutex> info_lock(info_mutex_);
        cached_info_ = {};
    }

    std::lock_guard<std::mutex> pool_lock(pool_mutex_);
    for (AVPacket* packet : packet_pool_) {
        av_packet_free(&packet);
    }
    packet_pool_.clear();
}

PullReadResult FFmpegPuller::ReadPacket() {
    std::lock_guard<std::mutex> io_lock(io_mutex_);

    if (!fmt_ctx_) {
        return {
            PullError{
                PullErrorCategory::Cancelled,
                0,
                "FFmpeg puller is not open",
                false,
            },
            nullptr,
            PullReadStatus::Stopped,
        };
    }

    AVPacket* packet = nullptr;
    {
        std::lock_guard<std::mutex> pool_lock(pool_mutex_);
        if (!packet_pool_.empty()) {
            packet = packet_pool_.back();
            packet_pool_.pop_back();
        }
    }

    if (!packet) {
        packet = av_packet_alloc();
    } else {
        av_packet_unref(packet);
    }

    if (!packet) {
        return {
            PullError{
                PullErrorCategory::Internal,
                AVERROR(ENOMEM),
                "temporary AVPacket allocation failed",
                false,
            },
            nullptr,
            PullReadStatus::FatalError,
        };
    }

    interrupt_ctx_.timed_out.store(false);
    if (config_.io.read_timeout.count() > 0) {
        interrupt_ctx_.deadline = std::chrono::steady_clock::now() + config_.io.read_timeout;
    } else {
        interrupt_ctx_.deadline = std::chrono::steady_clock::time_point::max();
    }

    const int read_result = av_read_frame(fmt_ctx_, packet);
    if (read_result < 0) {
        av_packet_unref(packet);
        {
            std::lock_guard<std::mutex> pool_lock(pool_mutex_);
            packet_pool_.push_back(packet);
        }

        if (read_result == AVERROR_EOF) {
            return {
                PullError{
                    PullErrorCategory::EndOfInput,
                    read_result,
                    "end of input",
                    false,
                },
                nullptr,
                PullReadStatus::EOS,
            };
        }

        if (interrupt_ctx_.stop_requested.load()) {
            return {
                PullError{
                    PullErrorCategory::Cancelled,
                    read_result,
                    "FFmpeg read interrupted",
                    false,
                },
                nullptr,
                PullReadStatus::Stopped,
            };
        }

        const bool retryable =
            read_result == AVERROR(EAGAIN) || interrupt_ctx_.timed_out.load();
#ifdef ETIMEDOUT
        const bool timed_out = read_result == AVERROR(ETIMEDOUT);
#else
        const bool timed_out = false;
#endif

        if (retryable || timed_out) {
            return {
                PullError{
                    timed_out || interrupt_ctx_.timed_out.load()
                        ? PullErrorCategory::Timeout
                        : PullErrorCategory::Network,
                    read_result,
                    "temporary FFmpeg read error: " +
                        FfmpegErrorText(read_result),
                    true,
                },
                nullptr,
                PullReadStatus::RetryableError,
            };
        }

        return {
            PullError{
                PullErrorCategory::Network,
                read_result,
                "FFmpeg read error: " + FfmpegErrorText(read_result),
                false,
            },
            nullptr,
            PullReadStatus::FatalError,
        };
    }

    const int stream_index = packet->stream_index;
    const auto codec = codecpars_.find(stream_index);
    if (codec == codecpars_.end()) {
        // 未知流，比如字幕流，则放回池中
        av_packet_unref(packet);
        std::lock_guard<std::mutex> pool_lock(pool_mutex_);
        packet_pool_.push_back(packet);
        return {
            std::nullopt,
            nullptr,
            PullReadStatus::NoData,
        };
    }

    AVCodecParameters* codecpar = codec->second;
    if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
        !isVideoStreamInfoComplete(stream_index)) {
        const bool recovered = probeVideoStream(stream_index, packet);
        if (!recovered) {
            const bool exhausted = isVideoProbeExhausted(stream_index);
            av_packet_unref(packet);
            {
                std::lock_guard<std::mutex> pool_lock(pool_mutex_);
                packet_pool_.push_back(packet);
            }

            if (exhausted) {
                return {
                    PullError{
                        PullErrorCategory::InvalidMedia,
                        AVERROR_INVALIDDATA,
                        "video stream metadata probe exhausted",
                        false,
                    },
                    nullptr,
                    PullReadStatus::FatalError,
                };
            }

            return {
                std::nullopt,
                nullptr,
                PullReadStatus::NoData,
            };
        }
    }

    // 1. 分配新的 AVPacket（用于传递给调用者）
    AVPacket* owned_packet = av_packet_alloc();
    if (!owned_packet) {
        av_packet_unref(packet);
        std::lock_guard<std::mutex> pool_lock(pool_mutex_);
        packet_pool_.push_back(packet);
        return {
            PullError{
                PullErrorCategory::Internal,
                AVERROR(ENOMEM),
                "delivered AVPacket allocation failed",
                false,
            },
            nullptr,
            PullReadStatus::FatalError,
        };
    }

    // 2. 转移数据所有权（零拷贝）
    av_packet_move_ref(owned_packet, packet);
    av_packet_unref(packet);
    {
        std::lock_guard<std::mutex> pool_lock(pool_mutex_);
        packet_pool_.push_back(packet);
    }

    // 3. 提取流信息（用于填充 MediaPacket）
    const AVStream* stream = fmt_ctx_->streams[stream_index];
    auto media_packet = std::make_shared<MediaPacket>();
    media_packet->type = codecpar->codec_type == AVMEDIA_TYPE_VIDEO
        ? MediaType::VIDEO
        : MediaType::AUDIO;
    media_packet->codec = MapCodecID(codecpar->codec_id);
    media_packet->stream_index = stream_index;
    media_packet->pts = owned_packet->pts == AV_NOPTS_VALUE
        ? kNoTimestamp
        : owned_packet->pts;
    media_packet->dts = owned_packet->dts == AV_NOPTS_VALUE
        ? kNoTimestamp
        : owned_packet->dts;
    media_packet->duration = owned_packet->duration == AV_NOPTS_VALUE
        ? kNoTimestamp
        : owned_packet->duration;
    media_packet->time_base = {
        stream->time_base.num,
        stream->time_base.den,
    };
    media_packet->keyframe = (owned_packet->flags & AV_PKT_FLAG_KEY) != 0;
    media_packet->buffer = std::make_shared<FFmpegPacketBuffer>(owned_packet);
    media_packet->backend.type = BackendHandle::FFMPEG;
    media_packet->backend.ptr = std::static_pointer_cast<FFmpegPacketBuffer>(media_packet->buffer)
        ->GetPacket();

    return {
        std::nullopt,
        std::move(media_packet),
        PullReadStatus::Packet,
    };
}

MultiStreamInfo FFmpegPuller::GetStreamInfo() const {
    std::lock_guard<std::mutex> lock(info_mutex_);
    return cached_info_;
}

void FFmpegPuller::SetEventCallback(EventCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    event_cb_ = std::move(cb);
}

void FFmpegPuller::resetVideoProbes() {
    for (auto& [stream_index, probe] : video_probes_) {
        (void)stream_index;
        if (!probe) {
            continue;
        }
        if (probe->parser) {
            av_parser_close(probe->parser);
            probe->parser = nullptr;
        }
        if (probe->codec) {
            avcodec_free_context(&probe->codec);
        }
    }
    video_probes_.clear();
}

bool FFmpegPuller::probeVideoStream(int stream_index, const AVPacket* packet) {
    const auto probe_it = video_probes_.find(stream_index);
    if (probe_it == video_probes_.end() || !probe_it->second || !packet) {
        return false;
    }

    VideoProbeContext& probe = *probe_it->second;
    ++probe.packets;
    if (probe.parser && probe.codec && packet->data && packet->size > 0) {
        std::uint8_t* parsed_data = nullptr;
        int parsed_size = 0;
        const int parse_result = av_parser_parse2(
            probe.parser,
            probe.codec,
            &parsed_data,
            &parsed_size,
            packet->data,
            packet->size,
            packet->pts,
            packet->dts,
            packet->pos);
        if (parse_result < 0) {
            return false;
        }

        const int width = probe.codec->width > 0
            ? probe.codec->width
            : probe.parser->width;
        const int height = probe.codec->height > 0
            ? probe.codec->height
            : probe.parser->height;
        if (width > 0 && height > 0) {
            updateVideoStreamInfo(stream_index, width, height);
        }
    }

    return isVideoStreamInfoComplete(stream_index);
}

bool FFmpegPuller::isVideoStreamInfoComplete(int stream_index) const {
    std::lock_guard<std::mutex> lock(info_mutex_);
    for (const auto& info : cached_info_.stream_infos) {
        if (info.stream_index != stream_index ||
            info.media_type != MediaType::VIDEO ||
            !std::holds_alternative<VideoStreamInfo>(info.detail)) {
            continue;
        }
        const auto& video = info.get_detail<VideoStreamInfo>();
        return video.width > 0 && video.height > 0;
    }
    return false;
}

bool FFmpegPuller::isVideoProbeExhausted(int stream_index) const {
    const auto probe = video_probes_.find(stream_index);
    if (probe == video_probes_.end() || !probe->second) {
        return true;
    }

    const auto& context = *probe->second;
    const bool packet_limit = config_.probe.max_video_probe_packets > 0 &&
        context.packets >= config_.probe.max_video_probe_packets;
    const bool time_limit = config_.probe.video_probe_timeout.count() > 0 &&
        std::chrono::steady_clock::now() - context.started >=
            config_.probe.video_probe_timeout;
    return packet_limit || time_limit;
}

void FFmpegPuller::updateVideoStreamInfo(int stream_index, int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(info_mutex_);
    for (auto& info : cached_info_.stream_infos) {
        if (info.stream_index == stream_index &&
            info.media_type == MediaType::VIDEO &&
            std::holds_alternative<VideoStreamInfo>(info.detail)) {
            auto& video = std::get<VideoStreamInfo>(info.detail);
            video.width = width;
            video.height = height;
            return;
        }
    }
}

CodecType FFmpegPuller::MapCodecID(AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_H264: return CodecType::H264;
        case AV_CODEC_ID_HEVC: return CodecType::H265;
        case AV_CODEC_ID_AAC: return CodecType::AAC;
        case AV_CODEC_ID_OPUS: return CodecType::OPUS;
        case AV_CODEC_ID_PCM_ALAW: return CodecType::G711A;
        case AV_CODEC_ID_PCM_MULAW: return CodecType::G711U;
        case AV_CODEC_ID_ADPCM_G726: return CodecType::G726;
        case AV_CODEC_ID_MJPEG: return CodecType::JPEG;
        default: return CodecType::UNKNOWN;
    }
}
