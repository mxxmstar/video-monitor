#pragma once

#include "media/pusher/ffmpeg_muxer.h"
#include "media/pusher/i_pusher.h"

/// @brief 基于 FFmpeg 的最小输出实现。
///
/// 初版只将单路 H.264 视频包同步交给 FFmpegMuxer，主要用于建立
/// Muxer 与后续 PusherSession 之间清晰的职责边界。它不缓存 packet，
/// 不等待关键帧，也不在写失败后自行重连。
class FFmpegPusher final : public IPusher {
public:
    FFmpegPusher() = default;
    ~FFmpegPusher() override;

    FFmpegPusher(const FFmpegPusher&) = delete;
    FFmpegPusher& operator=(const FFmpegPusher&) = delete;

    PusherResult Open(const PusherConfig& config) override;
    PusherResult Push(const MediaPacket& packet) override;
    PusherResult Close() override;
    bool IsOpen() const override;

private:
    PusherConfig config_{};
    FFmpegMuxer muxer_;
    bool opened_{false};
};
