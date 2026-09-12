#include "media/pusher/ffmpeg_pusher.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

PusherConfig MakeValidConfig(const std::string& output_url) {
    PusherConfig config;
    config.output_url = output_url;
    config.video_track.media_type = MediaType::VIDEO;
    config.video_track.codec_type = CodecType::H264;
    config.video_track.time_base_num = 1;
    config.video_track.time_base_den = 25;
    config.video_track.video().width = 1280;
    config.video_track.video().height = 720;
    config.video_track.video().fps = 25.0f;
    return config;
}

bool IsFailure(const PusherResult& result, PusherErrorCategory expected) {
    return !result.Succeed() && result.error.has_value() &&
           result.error->category == expected;
}

}  // namespace

int main() {
    FFmpegPusher pusher;

    // Push 必须先经过 Open。这里不构造真实 AVPacket，也能稳定验证输出端的
    // 生命周期保护不会把未初始化的数据交给 FFmpeg。
    MediaPacket unopened_packet;
    if (!IsFailure(pusher.Push(unopened_packet),
                   PusherErrorCategory::InvalidState)) {
        std::cerr << "Push before Open did not return InvalidState" << std::endl;
        return 1;
    }

    // 空 URL 是基础配置错误，应在创建底层 AVFormatContext 前被拒绝。
    if (!IsFailure(pusher.Open(MakeValidConfig("")),
                   PusherErrorCategory::InvalidConfiguration)) {
        std::cerr << "Empty output URL did not return InvalidConfiguration" << std::endl;
        return 1;
    }

    // 配置完整但采用当前版本没有实现的编码格式，应返回“不支持媒体”，
    // 而不是让调用方误以为输出路径或 FFmpeg 初始化失败。
    PusherConfig unsupported_config = MakeValidConfig("unused.mp4");
    unsupported_config.video_track.codec_type = CodecType::H265;
    if (!IsFailure(pusher.Open(unsupported_config),
                   PusherErrorCategory::UnsupportedMedia)) {
        std::cerr << "Unsupported codec did not return UnsupportedMedia" << std::endl;
        return 1;
    }

    const std::filesystem::path output_path =
        std::filesystem::current_path() / "ffmpeg_pusher_lifecycle_test.mp4";
    std::error_code file_error;
    std::filesystem::remove(output_path, file_error);

    if (!pusher.Open(MakeValidConfig(output_path.string())).Succeed() ||
        !pusher.IsOpen()) {
        std::cerr << "Failed to open FFmpegPusher" << std::endl;
        return 1;
    }

    // 已打开后，缺少 FFmpeg AVPacket 句柄的包属于输入数据错误。该检查在
    // 调用 Muxer 前完成，所以不会污染刚刚创建的输出文件。
    MediaPacket invalid_packet;
    invalid_packet.type = MediaType::VIDEO;
    invalid_packet.codec = CodecType::H264;
    invalid_packet.time_base = {1, 25};
    if (!IsFailure(pusher.Push(invalid_packet),
                   PusherErrorCategory::InvalidPacket)) {
        std::cerr << "Invalid packet did not return InvalidPacket" << std::endl;
        pusher.Close();
        return 1;
    }

    // Close 需要幂等，便于后续 PusherSession 在正常停止、打开失败清理和
    // 析构路径中都安全调用它。
    if (!pusher.Close().Succeed() || pusher.IsOpen() ||
        !pusher.Close().Succeed()) {
        std::cerr << "FFmpegPusher Close is not idempotent" << std::endl;
        return 1;
    }

    std::filesystem::remove(output_path, file_error);
    std::cout << "FFmpegPusher lifecycle test passed" << std::endl;
    return 0;
}
