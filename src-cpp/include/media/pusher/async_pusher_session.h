#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "common/queue/spsc_queue.h"
#include "media/pusher/pusher_session.h"

/// @brief 异步入队结果状态
///
/// Accepted 只表示 AsyncPusherSession 已经取得了包的独立所有权，不能表示
/// 底层 Pusher 已经写出该包。实际写入结果通过 Stats、State 和 LastEvent 观察。
enum class PusherEnqueueStatus {
    Accepted,   ///< 已经成功入队，等待写入
    DroppedAwaitingKeyframe,    ///< 已经入队，等待关键帧
    QueueFull,  ///< 队列已满，无法入队
    Rejected,   ///< 入队错误，无法入队
};

/// @brief 异步入队结果结构体
struct PusherEnqueueResult {
    PusherEnqueueStatus status{PusherEnqueueStatus::Rejected};
    std::optional<PusherError> error;

    bool Succeed() const noexcept {
        return status == PusherEnqueueStatus::Accepted ||
               status == PusherEnqueueStatus::DroppedAwaitingKeyframe;
    }

    bool Accepted() const noexcept {
        return status == PusherEnqueueStatus::Accepted;
    }

    static PusherEnqueueResult AcceptedResult() {
        return {PusherEnqueueStatus::Accepted, std::nullopt};
    }

    /// @brief 已经入队，等待关键帧
    static PusherEnqueueResult DroppedAwaitingKeyframe() {
        return {PusherEnqueueStatus::DroppedAwaitingKeyframe, std::nullopt};
    }

    /// @brief 队列已满，无法入队
    static PusherEnqueueResult QueueFull() {
        return {PusherEnqueueStatus::QueueFull, std::nullopt};
    }

    /// @brief 入队错误，无法入队
    static PusherEnqueueResult Rejected(PusherError error) {
        return {PusherEnqueueStatus::Rejected, std::move(error)};
    }
};

/// @brief 异步 Session 的累计统计。
struct AsyncPusherStats {
    std::uint64_t accepted{0}; ///< 已经成功入队的包数量
    std::uint64_t written{0}; ///< 已经成功写入的包数量
    std::uint64_t dropped_awaiting_keyframe{0}; ///< 因等待关键帧而丢弃的包数量
    std::uint64_t dropped_queue_full{0}; ///< 因队列满而丢弃的包数量
    std::uint64_t dropped_on_failure{0}; ///< 因失败而丢弃的包数量
    std::uint64_t dropped_on_close{0}; ///< 因关闭而丢弃的包数量
    std::uint64_t write_failures{0}; ///< 写入失败的包数量
    std::size_t queued_packets{0}; ///< 队列中的包数量
    std::size_t high_water_mark{0}; ///< 队列高水位标记
};

/// @brief 有界异步 Session。在同步 PusherSession 的基础上实现。
///
/// AsyncPusherSession 只增加入队和写入线程，不复制关键帧门控、时间轴、错误分类或重连逻辑。
/// 内部 PusherSession 是唯一调用底层 Pusher 的对象。采用单生产者/单消费者队列。
class AsyncPusherSession {
public:
    AsyncPusherSession();
    explicit AsyncPusherSession(std::unique_ptr<PusherSession> session);
    ~AsyncPusherSession();

    AsyncPusherSession(const AsyncPusherSession&) = delete;
    AsyncPusherSession& operator=(const AsyncPusherSession&) = delete;

    PusherResult Open(const PusherSessionConfig& config);
    PusherEnqueueResult Enqueue(const MediaPacket& packet);
    PusherResult Close();
    PusherSessionState State() const;
    std::optional<PusherEvent> LastEvent() const;
    PusherResult RequestStop();
    AsyncPusherStats Stats() const;

private:
    struct QueuedPacket {
        MediaPacket packet;
        std::uint64_t generation{0};
    };

    static PusherError makeError(PusherErrorCategory category, const char* message);
    static std::optional<MediaPacket> shallowClonePacket(const MediaPacket& packet, PusherError& error);

    void workerLoop(std::uint64_t generation);
    void clearQueue(bool count_as_close);

    /// @brief 记录统计结构体中的当前队列中的包数量
    void recordQueueSize(std::size_t queue_size);
    void notifyWorker();

    std::unique_ptr<PusherSession> session_;     ///< 内部 PusherSession
    mutable std::mutex lifecycle_mutex_;        ///< 生命周期互斥锁
    std::unique_ptr<BoundedSpscQueue<QueuedPacket>> queue_; ///< 待入包队列
    std::size_t queued_count_{0};   ///< 当前队列中确切存在的待发送包数量
    std::size_t pending_keyframes_{0};  ///< 当前入队待处理的关键帧数量
    std::thread worker_thread_; ///< 写入线程
    std::condition_variable worker_cv_; ///< 写入线程条件变量
    std::mutex worker_wait_mutex_;   ///< 写入线程互斥锁

    std::atomic<bool> accepting_{false}; ///< 是否接受入队请求
    std::atomic<bool> stop_requested_{false}; ///< 停止请求
    std::atomic<bool> close_requested_{false}; ///< 关闭请求
    std::atomic<bool> discard_requested_{false}; ///< 请求丢弃队列中的包
    std::atomic<bool> waiting_for_keyframe_{true}; ///< 是否等待关键帧
    std::uint64_t generation_{0}; ///< 当前入队的包的代际号
    PusherAsyncCloseMode close_mode_{PusherAsyncCloseMode::Discard};

    mutable std::mutex stats_mutex_;
    AsyncPusherStats stats_;
};
