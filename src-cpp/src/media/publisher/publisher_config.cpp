#include "media/publisher/publisher_config.h"
#include "common/log/logger.h"

bool MediaTrackConfig::is_video() const {
    return media_type == MediaType::VIDEO;
}

bool MediaTrackConfig::is_audio() const {
    return media_type == MediaType::AUDIO;
}

AudioTrackConfig& MediaTrackConfig::audio() {
    return std::get<AudioTrackConfig>(track_config);
}

VideoTrackConfig& MediaTrackConfig::video() {
    return std::get<VideoTrackConfig>(track_config);
}

const AudioTrackConfig& MediaTrackConfig::audio() const {
    if (is_audio()) {
        return std::get<AudioTrackConfig>(track_config);
    }
    static AudioTrackConfig empty;
    return empty;
}

const VideoTrackConfig& MediaTrackConfig::video() const {
    if (is_video()) {
        return std::get<VideoTrackConfig>(track_config);
    }
    static VideoTrackConfig empty;
    return empty;
}

bool MediaTrackConfig::is_valid() const {
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
        if (video().fps <= 0) {
            LOG_ERROR("Invalid video config: fps is <= 0");
            return false;
        }
    } else {
        LOG_ERROR("Invalid media_type: {}", static_cast<int>(media_type));
        return false;
    }
    return true;
}