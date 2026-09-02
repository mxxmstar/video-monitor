#if 1
#include "media/encoder/ffmpeg_encoder.h"
#include "media/converter/media_frame_converter.h"

#include "common/log/logger.h"
#include "media/ffmpeg_packet_buffer.h"
#include "media/ffmpeg_format.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

extern "C" {
#include <libavutil/dict.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
using namespace Media;
namespace {

/// @brief 获取 FFmpeg 错误码对应的错误字符串
std::string AvErrorString(int ret) {
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_make_error_string(buf, AV_ERROR_MAX_STRING_SIZE, ret);
    return buf;
}

/// @brief 从音频编码器支持的采样格式列表中，选择第一个可用的格式
/// @param codec 音频编码器
/// @param fmt 输出的采样格式
/// @return 是否成功选择格式
bool pickFirstSupportedSampleFormat(const AVCodec* codec, AVSampleFormat& fmt) {
    if (!codec) {
        return false;
    }

#if LIBAVCODEC_VERSION_MAJOR >= 61
    const void* configs = nullptr;
    int config_count = 0;
    const int ret = avcodec_get_supported_config(
        nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configs, &config_count);
    if (ret < 0 || !configs || config_count <= 0) {
        return false;
    }
    // 遍历找到第一个有效格式
    const auto* sample_fmts = static_cast<const AVSampleFormat*>(configs);
    for (int i = 0; i < config_count; ++i) {
        if (sample_fmts[i] != AV_SAMPLE_FMT_NONE) {
            fmt = sample_fmts[i];
            return true;
        }
    }
#else
    if (!codec->sample_fmts) {
        return false;
    }
    for (const AVSampleFormat* p = codec->sample_fmts; *p != AV_SAMPLE_FMT_NONE; ++p) {
        fmt = *p;
        return true;
    }
#endif

    return false;
}
}


FFmpegEncoder::~FFmpegEncoder() {
    Close();
}


bool FFmpegEncoder::Open(const EncoderConfig& cfg) {
    Close();

    // 参数校验    
    if (!cfg.is_valid()) {
        LOG_ERROR("Invalid encoder config");
        return false;
    }

    // 转换编码器类型
    const AVCodecID codec_id = ToAVCodecID(cfg.codec_type);
    if (codec_id == AV_CODEC_ID_NONE) {
        LOG_WARN("FFmpegEncoder:Open: unsupported codec type {}",
                          static_cast<int>(cfg.codec_type));
        return false;
    }

    // 查找合适的编码器
    AVPixelFormat encoder_pix_fmt = AV_PIX_FMT_NONE;
    AVSampleFormat encoder_sample_fmt = AV_SAMPLE_FMT_NONE;
    const AVCodec* codec = nullptr;

    if (cfg.media_type == MediaType::VIDEO) {
        const AVPixelFormat pix_fmt = ToAVPixelFormat(cfg.video().pixel_format);
        if (pix_fmt == AV_PIX_FMT_NONE) {
            LOG_ERROR("Invalid video pixel format {}", static_cast<int>(cfg.video().pixel_format));
            return false;
        }
        codec = findVideoEncoder(codec_id, pix_fmt, cfg.encoder_name, encoder_pix_fmt);
        if (codec == nullptr) {
            LOG_WARN("video encoder not found for codec={}, name={}, fmt={}",
                              static_cast<int>(codec_id), cfg.encoder_name,
                              static_cast<int>(pix_fmt));
            return false;
        }
    } else {
        const AVSampleFormat sample_fmt = ToAVSampleFormat(cfg.audio().sample_format);
        if (sample_fmt == AV_SAMPLE_FMT_NONE) {
            LOG_ERROR("Invalid audio sample format {}", static_cast<int>(cfg.audio().sample_format));
            return false;
        }
        if (sample_fmt == AV_SAMPLE_FMT_NONE) {
            LOG_WARN("audio encoder not found for codec={}, name={}, fmt={}",
                              static_cast<int>(codec_id), cfg.encoder_name,
                              static_cast<int>(sample_fmt));
            return false;
        }
        codec = findAudioEncoder(codec_id, sample_fmt, cfg.encoder_name, encoder_sample_fmt);
        if (codec == nullptr) {
            LOG_WARN("audio encoder not found for codec={}, name={}, fmt={}",
                              static_cast<int>(codec_id), cfg.encoder_name,
                              static_cast<int>(sample_fmt));
            return false;
        }
    }
    
    // 初始化编码器上下文
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (codec_ctx_ == nullptr) {
        LOG_ERROR("avcodec_alloc_context3 failed");
        return false;
    }

    // 设置编码器上下文参数
    codec_ctx_->codec_id = codec_id;
    // time base 完整时直接使用，否则按照媒体类型进行推导
    // 视频帧率是 fps_num/fps_den，对应时间基必须是 fps_den/fps_num。
    const int default_time_base_num = cfg.media_type == MediaType::VIDEO ? cfg.video().fps_den : 1;
    const int default_time_base_den = cfg.media_type == MediaType::VIDEO
            ? cfg.video().fps_num : cfg.audio().sample_rate;
    codec_ctx_->time_base = AVRational{
        cfg.time_base_num > 0 ? cfg.time_base_num : default_time_base_num,
        cfg.time_base_den > 0 ? cfg.time_base_den : default_time_base_den,
    };
    codec_ctx_->bit_rate = cfg.bitrate;
    codec_ctx_->thread_count = cfg.thread_count;
    if (cfg.global_header) {
        codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    AVDictionary* opts = nullptr;
    if (cfg.media_type == MediaType::VIDEO) {
        codec_ctx_->codec_type = AVMEDIA_TYPE_VIDEO;
        codec_ctx_->width = cfg.video().width;
        codec_ctx_->height = cfg.video().height;
        codec_ctx_->pix_fmt = encoder_pix_fmt;
        codec_ctx_->framerate = AVRational{cfg.video().fps_num, cfg.video().fps_den};
        codec_ctx_->gop_size = cfg.video().gop_size;
        codec_ctx_->max_b_frames = cfg.video().max_b_frames;

        // 设置编码器选项字典（preset/tune/crf）        
        if (!cfg.video().preset.empty()) {
            av_dict_set(&opts, "preset", cfg.video().preset.c_str(), 0);
        }
        if (!cfg.video().tune.empty()) {
            av_dict_set(&opts, "tune", cfg.video().tune.c_str(), 0);
        }
        if (cfg.video().crf >= 0) {
            av_dict_set_int(&opts, "crf", cfg.video().crf, 0);
        }
    } else {
        codec_ctx_->codec_type = AVMEDIA_TYPE_AUDIO;
        codec_ctx_->sample_rate = cfg.audio().sample_rate;
        if (cfg.audio().channel_layout != 0) {
            av_channel_layout_from_mask(&codec_ctx_->ch_layout, cfg.audio().channel_layout);
        } else {
            av_channel_layout_default(&codec_ctx_->ch_layout, cfg.audio().channels);
        }
        codec_ctx_->sample_fmt = encoder_sample_fmt;
    }

    // 打开编码器
    const int ret = avcodec_open2(codec_ctx_, codec, &opts);
    av_dict_free(&opts);
    if (ret < 0) {        
        LOG_WARN("FFmpegEncoder:Open: avcodec_open2 failed: {}", AvErrorString(ret));
        avcodec_free_context(&codec_ctx_);
        return false;
    }

    config_ = cfg;
    converter_ = std::make_unique<MediaFrameConverter>();
    MediaFrameConverterConfig frame_converter_config;

    if (cfg.media_type == MediaType::VIDEO) {
        frame_converter_config.video.width = cfg.video().width;
        frame_converter_config.video.height = cfg.video().height;
        frame_converter_config.video.sws_flags = cfg.video().sws_flags;
        frame_converter_config.video.pixel_format = cfg.video().pixel_format;
    } else {
        frame_converter_config.audio.sample_rate = cfg.audio().sample_rate;
        frame_converter_config.audio.channels = cfg.audio().channels;
        frame_converter_config.audio.channel_layout = cfg.audio().channel_layout;
        frame_converter_config.audio.sample_format = cfg.audio().sample_format;
    }

    if (!converter_->Open(frame_converter_config)) {
        LOG_ERROR("Failed to open frame converter: {}", MediaFrameConverter::LastError());
        return false;
    }

    if (cfg.media_type == MediaType::VIDEO) {
        LOG_INFO("Open video encoder success: codec={}, encoder={}, {}x{}, fps={}/{}, input_fmt={}, encoder_fmt={}",
                         static_cast<int>(cfg.codec_type),
                         codec->name ? codec->name : "<unknown>",
                         cfg.video().width, cfg.video().height, cfg.video().fps_num, cfg.video().fps_den,
                         static_cast<int>(cfg.video().pixel_format),
                         "todo"
                         /*static_cast<int>(encoder_pix_fmt_)*/);
    } else {
        LOG_INFO("Open audio encoder success: codec={}, encoder={}, rate={}, ch={}, input_fmt={}, encoder_fmt={}",
                         static_cast<int>(cfg.codec_type),
                         codec->name ? codec->name : "<unknown>",
                         cfg.audio().sample_rate, cfg.audio().channels,
                         static_cast<int>(cfg.audio().sample_format),
                         "todo"
                         /*static_cast<int>(encoder_sample_fmt_)*/);
    }

    return true;
}

bool FFmpegEncoder::Encode(FramePtr frame, std::vector<PacketPtr>& packets) {
    if (codec_ctx_ == nullptr) {
        LOG_ERROR("codec_ctx_ is nullptr");
        return false;
    }

    if (config_.media_type == MediaType::VIDEO) {
        return encodeVideoFrame(frame, packets);
    } else {
        return encodeAudioFrame(frame, packets);
    }
}

bool FFmpegEncoder::Flush(std::vector<PacketPtr>& packets) {
    return Encode(nullptr, packets);
}

EncodedTrackInfo FFmpegEncoder::GetOutputInfo() const {
    EncodedTrackInfo info;
    if (!codec_ctx_) {
        return info;
    }

    info.media_type = config_.media_type;
    info.codec_type = config_.codec_type;
    info.time_base = {codec_ctx_->time_base.num, codec_ctx_->time_base.den};

    if (config_.is_video()) {
        info.width = codec_ctx_->width;
        info.height = codec_ctx_->height;
        if (codec_ctx_->framerate.den > 0 && codec_ctx_->framerate.num > 0) {
            info.fps = static_cast<float>(codec_ctx_->framerate.num) /
                       static_cast<float>(codec_ctx_->framerate.den);
        }
    } else if (config_.is_audio()) {
        info.sample_rate = codec_ctx_->sample_rate;
        info.channels = codec_ctx_->ch_layout.nb_channels;
    }

    if (codec_ctx_->extradata && codec_ctx_->extradata_size > 0) {
        info.extra_data.assign(codec_ctx_->extradata,
                               codec_ctx_->extradata + codec_ctx_->extradata_size);
    }

    return info;
}

void FFmpegEncoder::Close() {
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    next_pts_ = 0;
    config_ = EncoderConfig{};
    if (converter_) {
        converter_->Close();
    }
}

bool FFmpegEncoder::encodeVideoFrame(FramePtr frame, std::vector<PacketPtr>& packets) {    
    // flush: 发送 nullptr 触发 flush 操作
    if (frame == nullptr) {
        const int ret = avcodec_send_frame(codec_ctx_, nullptr);
        if (ret < 0 && ret != AVERROR_EOF) {
            LOG_WARN("FFmpegEncoder:EncodeVideoFrame: avcodec_send_frame failed: {}", AvErrorString(ret));
            return false;
        }
        return receivePackets(packets);
    }

    const int frame_width = frame->Width() > 0 ? frame->Width() : config_.video().width;
    const int frame_height = frame->Height() > 0 ? frame->Height() : config_.video().height;
    if (frame_width != config_.video().width || frame_height != config_.video().height) {
        LOG_WARN("frame size mismatch {}x{}, expected {}x{}", frame_width, frame_height, config_.video().width, config_.video().height);
        return false;
    }
    if (frame->PixelFormat() != PixelFormat::kUnknown && frame->PixelFormat() != config_.video().pixel_format) {
        LOG_WARN("pixel format mismatch {}, expected {}", static_cast<int>(frame->PixelFormat()),
            static_cast<int>(config_.video().pixel_format));
        return false;
    }

    // 构造编码器输入的 AVFrame
    AVFrame* input = av_frame_alloc();
    if (!input) {
        LOG_WARN("av_frame_alloc failed");
        return false;
    }
     
    bool ok = converter_->MediaFrameToAVFrame(*frame, input);
    if (!ok) {
        LOG_WARN("MediaFrameToAVFrame failed: {}", converter_->LastError());
        av_frame_free(&input);
        return false;
    }

    // 发送帧到编码器，若编码器输出队列满（EAGAIN）则先取包再重试
    int ret = avcodec_send_frame(codec_ctx_, input);
    if (ret == AVERROR(EAGAIN)) {
        if (!receivePackets(packets)) {
            av_frame_free(&input);
            return false;
        }
        ret = avcodec_send_frame(codec_ctx_, input);
    }
    av_frame_free(&input);

    if (ret < 0) {        
        LOG_WARN("avcodec_send_frame failed: {}", AvErrorString(ret));
        return false;
    }

    return receivePackets(packets);
}

bool FFmpegEncoder::encodeAudioFrame(FramePtr frame, std::vector<PacketPtr>& packets) {
    // 判断编码器是否需要固定帧大小
    //      frame_size        可变帧大小
    // AAC	   1024	        必须固定 1024 样本
    // MP3	   1152	        必须固定 1152 样本
    // PCM	   0	        任意大小
    // Opus	   960	        支持可变大小
    // const bool fixed_frame_size = codec_ctx_->fixed_frame_size > 0 &&
    //     (!codec_ctx_->codec ||
    //      (codec_ctx_->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) == 0);
    
    // // 可变帧大小，直接送到编码器中，不需要fifo缓冲
    // if (!fixed_frame_size) {
    //     if (!frame) {
    //         return sendFrameToEncoder(nullptr, packets);
    //     }
    //     AVFrame* input = buildInputFrame(frame);
    //     if (!input) {
    //         return false;
    //     }
    //     const bool ok = sendFrameToEncoder(input, packets);
    //     av_frame_free(&input);
    //     return ok;
    // }

    return true;
}


bool FFmpegEncoder::sendFrameToEncoder(AVFrame* frame, std::vector<PacketPtr>& packets) {
    int ret = avcodec_send_frame(codec_ctx_, frame);
    if (ret == AVERROR(EAGAIN)) {
        if (!receivePackets(packets)) {
            return false;
        }
        ret = avcodec_send_frame(codec_ctx_, frame);
    }
    if (ret < 0) {
        LOG_WARN("FFmpegEncoder:SendFrameToEncoder: avcodec_send_frame failed: {}",
                 AvErrorString(ret));
        return false;
    }
    return receivePackets(packets);
}

bool FFmpegEncoder::receivePackets(std::vector<PacketPtr>& packets) {
    if (codec_ctx_ == nullptr) {
        LOG_ERROR("codec_ctx_ is nullptr");
        return false;
    }

    while (true) {        
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) {
            LOG_ERROR("av_packet_alloc failed");
            return false;
        }

        const int ret = avcodec_receive_packet(codec_ctx_, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&pkt);
            return true;
        }
        if (ret < 0) {            
            LOG_WARN("FFmpegEncoder:ReceivePackets: avcodec_receive_packet failed: {}", AvErrorString(ret));
            av_packet_free(&pkt);
            return false;
        }

        // 包装为 MediaPacket，保持 FFmpeg 后端引用
        auto pkt_buffer = std::make_shared<FFmpegPacketBuffer>(pkt);
        auto media_packet = std::make_shared<MediaPacket>();
        media_packet->type = config_.media_type;
        media_packet->codec = config_.codec_type;
        
        // AV_NOPTS_VALUE 映射为公共层统一的 kNoTimestamp。
        media_packet->pts = pkt->pts == AV_NOPTS_VALUE ? kNoTimestamp : pkt->pts;
        media_packet->dts = pkt->dts == AV_NOPTS_VALUE ? kNoTimestamp : pkt->dts;
        media_packet->duration = pkt->duration == AV_NOPTS_VALUE
            ? kNoTimestamp
            : pkt->duration;
        media_packet->stream_index = 0;
        // 编码器输出的 packet 已经位于 codec_ctx_->time_base 中，直接保留tick
        media_packet->time_base = Rational{codec_ctx_->time_base.num,
                                           codec_ctx_->time_base.den};
        media_packet->keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        media_packet->buffer = pkt_buffer;
        media_packet->backend.type = BackendHandle::FFMPEG;
        media_packet->backend.ptr = pkt_buffer->GetPacket();

        packets.emplace_back(std::move(media_packet));
    }
}

bool FFmpegEncoder::isPixelFormatSupported(const AVCodec* codec, AVPixelFormat fmt) const {
    if (codec == nullptr) {        
        return false;
    }

#if LIBAVCODEC_VERSION_MAJOR >= 61
    // FFmpeg 6.1+ 使用 avcodec_get_supported_config API
    const void* configs = nullptr;
    int config_count = 0;
    const int ret = avcodec_get_supported_config(
        nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &config_count);
    if (ret < 0 || !configs || config_count <= 0) {
        return true;
    }

    const auto* pix_fmts = static_cast<const AVPixelFormat*>(configs);
    for (int i = 0; i < config_count; ++i) {
        if (pix_fmts[i] == fmt) {
            return true;
        }
    }
    return false;
#else
    // 旧版本直接遍历 codec->pix_fmts 数组
    if (!codec->pix_fmts) {
        return true;
    }
    for (const AVPixelFormat* p = codec->pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == fmt) {
            return true;
        }
    }
    return false;
#endif    
}

bool FFmpegEncoder::isSampleFormatSupported(const AVCodec* codec, AVSampleFormat fmt) const {
    if (!codec) {
        return false;
    }

#if LIBAVCODEC_VERSION_MAJOR >= 61
    const void* configs = nullptr;
    int config_count = 0;
    const int ret = avcodec_get_supported_config(
        nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configs, &config_count);
    if (ret < 0 || !configs || config_count <= 0) {
        return true;
    }

    const auto* sample_fmts = static_cast<const AVSampleFormat*>(configs);
    for (int i = 0; i < config_count; ++i) {
        if (sample_fmts[i] == fmt) {
            return true;
        }
    }
    return false;
#else
    if (!codec->sample_fmts) {
        return true;
    }
    for (const AVSampleFormat* p = codec->sample_fmts; *p != AV_SAMPLE_FMT_NONE; ++p) {
        if (*p == fmt) {
            return true;
        }
    }
    return false;
#endif
}

const AVCodec* FFmpegEncoder::findVideoEncoder(AVCodecID codec_id, AVPixelFormat input_fmt,
            const std::string& encoder_name, AVPixelFormat& encoder_fmt) const {
    // 检查编码器是否支持指定像素格式
    auto accepts = [&](const AVCodec* codec, AVPixelFormat fmt) {
        return codec && codec->id == codec_id && av_codec_is_encoder(codec) &&
               isPixelFormatSupported(codec, fmt);
    };

    // 按名称查找支持的编码器
    if (!encoder_name.empty()) {
        const AVCodec* named = avcodec_find_encoder_by_name(encoder_name.c_str());
        // 名称无效
        if (!named || named->id != codec_id || !av_codec_is_encoder(named)) {
            return nullptr;
        }
        if (isPixelFormatSupported(named, input_fmt)) {
            encoder_fmt = input_fmt;
            return named;
        }
         // 输入 YUV420P 时尝试 NV12 作为备选
        if (input_fmt == AV_PIX_FMT_YUV420P && isPixelFormatSupported(named, AV_PIX_FMT_NV12)) {
            encoder_fmt = AV_PIX_FMT_NV12;
            return named;
        }
        return nullptr;
    }

    // 优先使用 FFmpeg 为该 codec 注册的默认编码器。Windows 上编码器枚举
    // 顺序可能把 aac_mf 放在软件 AAC 前面，而 MFT 对采样率支持不完整，
    // 直接遍历会让同一配置在不同机器上得到不同结果。
    if (const AVCodec* preferred = avcodec_find_encoder(codec_id)) {
        if (accepts(preferred, input_fmt)) {
            encoder_fmt = input_fmt;
            return preferred;
        }
        if (input_fmt == AV_PIX_FMT_YUV420P && accepts(preferred, AV_PIX_FMT_NV12)) {
            encoder_fmt = AV_PIX_FMT_NV12;
            return preferred;
        }
    }

    // 遍历所有编码器，优先找直接支持输入格式的编码器
    const AVCodec* fallback = nullptr;
    void* iter = nullptr;
    while (const AVCodec* codec = av_codec_iterate(&iter)) {
        if (accepts(codec, input_fmt)) {
            encoder_fmt = input_fmt;
            return codec;
        }
        // YUV420P -> NV12 回退
        if (!fallback && input_fmt == AV_PIX_FMT_YUV420P &&
            accepts(codec, AV_PIX_FMT_NV12)) {
            fallback = codec;
        }
    }
    if (fallback) {
        encoder_fmt = AV_PIX_FMT_NV12;
        return fallback;
    }
    return nullptr;
}

const AVCodec* FFmpegEncoder::findAudioEncoder(AVCodecID codec_id, AVSampleFormat input_fmt,
            const std::string& encoder_name, AVSampleFormat& encoder_fmt) const {
    // 检查编码器是否支持指定采样格式
    auto accepts = [&](const AVCodec* codec, AVSampleFormat fmt) {
        return codec && codec->id == codec_id && av_codec_is_encoder(codec) &&
               isSampleFormatSupported(codec, fmt);
    };

    auto choose_format = [&](const AVCodec* codec) {
        if (!codec || codec_id != codec->id || !av_codec_is_encoder(codec)) {
            return false;
        }
        if (isSampleFormatSupported(codec, input_fmt)) {
            encoder_fmt = input_fmt;
            return true;
        }
        return pickFirstSupportedSampleFormat(codec, encoder_fmt);
    };

    // 按名称查找支持的编码器
    if (!encoder_name.empty()) {
        const AVCodec* named = avcodec_find_encoder_by_name(encoder_name.c_str());        
        return choose_format(named) ? named : nullptr;
    }

    // 与视频编码器一致，先采用 FFmpeg 默认实现，避免平台专用 MFT 因
    // 采样率/布局限制抢占同 codec 的通用软件实现。
    if (const AVCodec* preferred = avcodec_find_encoder(codec_id)) {
        if (choose_format(preferred)) {
            return preferred;
        }
    }

    // 遍历所有编码器
    const AVCodec* fallback = nullptr;
    void* iter = nullptr;
    while (const AVCodec* codec = av_codec_iterate(&iter)) {
        if (accepts(codec, input_fmt)) {
            encoder_fmt = input_fmt;
            return codec;
        }
        if (!fallback && choose_format(codec)) {
            fallback = codec;
        }
    }

    return fallback;    
}



int64_t FFmpegEncoder::resolveFramePts(const MediaFrame& frame) {
    if (IsValidTimestamp(frame.time.pts_us)) {
        // MediaFrame 明确使用微秒，送入 FFmpeg 前必须换算到编码器 time_base。
        const int64_t pts = av_rescale_q(frame.time.pts_us, AVRational{1, 1'000'000},
            codec_ctx_ ? codec_ctx_->time_base : AVRational{1, 1'000'000});        
        // 递增 pts 计数器
        next_pts_ = std::max(next_pts_, pts + 1);
        return pts;
    }
    // 未指定 pts，递增分配
    return next_pts_++;
}
#endif