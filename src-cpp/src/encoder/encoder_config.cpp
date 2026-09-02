#include "media/encoder/encoder_config.h"
#include "common/log/logger.h"

bool EncoderConfig::is_video() const {
    return media_type == MediaType::VIDEO;
}

bool EncoderConfig::is_audio() const {
    return media_type == MediaType::AUDIO;
}

AudioEncoderConfig EncoderConfig::audio() const {
    if (is_audio()) {
        return std::get<AudioEncoderConfig>(specific);
    }
    return AudioEncoderConfig{};
}

VideoEncoderConfig EncoderConfig::video() const {
    if (is_video()) {
        return std::get<VideoEncoderConfig>(specific);
    }
    return VideoEncoderConfig{};
}

bool EncoderConfig::is_valid() const {
    if (media_type == MediaType::AUDIO) {
        if (audio().sample_rate <= 0 || audio().channels <= 0) {
            LOG_ERROR("Invalid audio config: sample_rate or channels is <= 0");
            return false;
        }
    } else if (media_type == MediaType::VIDEO) {
        if (video().width <= 0 || video().height <= 0) {
            LOG_ERROR("Invalid video config: width or height is <= 0");
            return false;
        }
        if (video().fps_num <= 0 || video().fps_den <= 0) {
            LOG_ERROR("Invalid video config: fps_num or fps_den is <= 0");
            return false;
        }
    } else {
        LOG_ERROR("Invalid media_type: {}", static_cast<int>(media_type));
        return false;
    }
    return true;
}

