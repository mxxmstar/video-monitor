#include "media/pusher/pusher_config.h"

#include <variant>

bool PusherConfig::is_valid() const {
    if (output_url.empty()) {
        return false;
    }

    // MediaTrackConfig::video() 使用 std::get<VideoTrackConfig>()。先检查
    // variant 实际保存的视频配置类型，能让错误配置直接返回 false，而不是
    // 在配置校验阶段抛出 std::bad_variant_access。
    if (video_track.media_type != MediaType::VIDEO ||
        !std::holds_alternative<VideoTrackConfig>(video_track.track_config)) {
        return false;
    }

    return video_track.is_valid() &&
           IsValidTimeBase(
               Rational{video_track.time_base_num, video_track.time_base_den});
}
