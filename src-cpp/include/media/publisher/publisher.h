#pragma once

#include <memory>

#include "media/pusher/async_pusher_session.h"
#include "media/publisher/publisher_config.h"

/// @brief Publisher 的对外状态。
///
/// 这个枚举与 PusherSessionState 保持相同语义，但不直接把会话实现类型
/// 暴露给调用方。将来 Publisher 同时管理 pushersession 和 mediaserver 时，仍可维持稳定的
/// 应用层状态接口。
enum class PublisherState {
    Closed,
    WaitingForKeyframe,
    Running,
    Failed,
};

/// @brief 发布门面。
///
/// Publisher 是应用 Pipeline 的输出入口：它根据配置在同步和异步两种
/// Session 中直接选择其一，并提供统一 Open/Publish/Close 接口。它不调用 FFmpeg API，
/// 不实现关键帧等待、重连或时间戳换算；这些分别由 PusherSession 和 Pusher
/// 负责，从而保证上层不会与具体输出协议耦合。
class Publisher {
public:
    /// @brief 创建默认的 FFmpeg 文件发布门面。
    Publisher();

    /// @brief 注入同步 Session
    explicit Publisher(std::unique_ptr<PusherSession> session);
    /// @brief 注入异步 Session
    explicit Publisher(std::unique_ptr<AsyncPusherSession> session);
    ~Publisher();

    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;

    /// @brief 打开一次发布任务。重复调用会先由 Session 结束旧输出。
    PusherResult Open(const PublisherConfig& config);

    /// @brief 发布一个已经编码的媒体包。
    PusherPublishResult Publish(const MediaPacket& packet);

    /// @brief 将一个已经编码的媒体包交给异步 Session 的有界队列。
    PusherEnqueueResult Enqueue(const MediaPacket& packet);

    /// @brief 幂等结束当前发布任务。
    PusherResult Close();

    /// @brief 发布状态。
    PublisherState State() const noexcept;

    /// @brief 请求结束当前发布任务。
    PusherResult RequestStop();

private:
    static PusherError MakeError(PusherErrorCategory category, const char* message);

    // 按 Open() 的配置惰性创建，避免为未选中的 Session 启动事件线程。
    std::unique_ptr<PusherSession> session_;
    std::unique_ptr<AsyncPusherSession> async_session_;
    PusherSessionMode active_mode_{PusherSessionMode::Synchronous};
};
