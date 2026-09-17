#pragma once
#include <atomic>
#include <chrono>
#include <mutex>

#include "media/pusher/ffmpeg_muxer.h"
#include "media/pusher/i_pusher.h"

struct MuxerError;
PusherResult MapMuxerError(const MuxerError& error, bool network_output);

/// @brief 基于 FFmpeg 的最小输出实现
/// @details 负责校验传到 Muxer 的内容; 将可重试的错误给到 Session.
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
    void RequestStop() { muxer_.RequestStop(); }

    void SetEventCallback(EventCallback cb) override;

private:


    PusherConfig config_{};
    FFmpegMuxer muxer_;
    bool opened_{false};
    bool network_output_{false};

    EventCallback event_cb_;                   ///< 事件回调
    mutable std::mutex callback_mutex_; ///< 保护 event_cb_ 的互斥锁
};
