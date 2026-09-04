#include "media/encoder/encoder_config.h"
#include "common/log/logger.h"


void VideoEncoderConfig::Dump() const {
    LOG_INFO("VideoEncoderConfig: width={}, height={}, fps={}/{}, pixel_format={}, gop_size={}, max_b_frames={}, max_bitrate={}, rc_buffer_size={}, sws_flags={}, preset={}, tune={}, crf={}",
                width, height, fps_num, fps_den, static_cast<int>(pixel_format), gop_size, max_b_frames, max_bitrate, rc_buffer_size, sws_flags, preset, tune, crf);
}

void AudioEncoderConfig::Dump() const {
    LOG_INFO("AudioEncoderConfig: sample_rate={}, channels={}, channel_layout={}, sample_format={}",
                sample_rate, channels, channel_layout, static_cast<int>(sample_format));
}

bool EncoderConfig::is_video() const {
    return media_type == MediaType::VIDEO;
}

bool EncoderConfig::is_audio() const {
    return media_type == MediaType::AUDIO;
}

AudioEncoderConfig& EncoderConfig::audio() {
    return std::get<AudioEncoderConfig>(specific);
}

VideoEncoderConfig& EncoderConfig::video() {
    return std::get<VideoEncoderConfig>(specific);
}

const AudioEncoderConfig& EncoderConfig::audio() const {
    if (is_audio()) {
        return std::get<AudioEncoderConfig>(specific);
    }
    static AudioEncoderConfig empty;
    return empty;
}

const VideoEncoderConfig& EncoderConfig::video() const {
    if (is_video()) {
        return std::get<VideoEncoderConfig>(specific);
    }
    static VideoEncoderConfig empty;
    return empty;
}

bool EncoderConfig::is_valid() const {
    if (media_type == MediaType::AUDIO) {
        if (audio().sample_rate <= 0 || audio().channels <= 0) {
            LOG_ERROR("Invalid audio config: sample_rate or channels is <= 0");
            audio().Dump();
            return false;
        }
    } else if (media_type == MediaType::VIDEO) {
        video().Dump();
        if (video().width <= 0 || video().height <= 0) {
            LOG_ERROR("Invalid video config: width or height is <= 0");
            video().Dump();
            return false;
        }
        if (video().fps_num <= 0 || video().fps_den <= 0) {
            LOG_ERROR("Invalid video config: fps_num or fps_den is <= 0");
            video().Dump();
            return false;
        }
    } else {
        LOG_ERROR("Invalid media_type: {}", static_cast<int>(media_type));
        return false;
    }
    return true;
}