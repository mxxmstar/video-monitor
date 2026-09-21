#pragma once

#include <memory>
#include <optional>

#include "media/pusher/i_pusher.h"

enum class PusherTimestampMode {
    Preserved, ///< 保留编码器输出的视频源原始时间戳，不进行转换
    StartAtZero, ///< 转换为推流器从零开始的时间戳，从第一个被接纳的关键帧开始计时
};

enum class PusherTimestampEpochScope {
    Session,     ///< 自动重连后沿用本次会话中的原 epoch，保持逻辑时间连续
    Connection,  ///< 自动重连后推流器以新连接的首包重新建立 epoch，即时间轴从零开始计时
};

struct PusherTimestampPolicy {
    PusherTimestampMode mode{PusherTimestampMode::Preserved};
    PusherTimestampEpochScope scope{PusherTimestampEpochScope::Session};
};

/// @brief 一次输出会话的状态。
///
/// 初版只有同步写入，因此不引入 Opening、Stopping 等瞬时状态。后续加入
/// 异步打开或重连时，再按实际需要扩展状态机。
enum class PusherSessionState {
    Closed,              ///< 尚未打开，或已完成关闭
    WaitingForKeyframe,  ///< 输出已打开，等待第一个视频关键帧
    Running,             ///< 已写入首个关键帧，正常转发编码包
    Failed,              ///< 底层写入失败，必须先 Close 再重新 Open
};

/// @brief PusherSession 的配置入口。
///
/// 时间轴、关键帧门控和重连属于 Session；具体协议、封装格式及 FFmpeg 参数
/// 仍由 PusherConfig 持有。
struct PusherSessionConfig {
    PusherConfig pusher;
    PusherTimestampPolicy timestamp_policy;

    bool is_valid() const { return pusher.is_valid(); }
};

/// @brief 一次 Publish 调用的结果状态。
///
/// WaitingForKeyframe 阶段丢弃非关键帧是预期策略，不是 I/O 错误，因此必须
/// 与真正的写入失败区分开；否则上层将无法正确统计丢包或决定是否重连。
enum class PusherPublishStatus {
    Published,                ///< packet 已交给 Pusher 写出
    DroppedAwaitingKeyframe,  ///< 会话尚未收到首个关键帧，packet 被策略性丢弃
    Failed,                   ///< 不允许发布，或 Pusher 写入失败
};

/// @brief 一次 Publish 调用的结构化结果。
struct PusherPublishResult {
    PusherPublishStatus status{PusherPublishStatus::Failed};
    std::optional<PusherError> error;

    /// @brief 返回本次调用是否按会话策略正常完成。
    ///
    /// 等待关键帧时的丢弃也属于正常完成，因此该场景返回 true；调用方若
    /// 需要统计实际写出数量，应使用 WasPublished()。
    bool Succeed() const noexcept {
        return status != PusherPublishStatus::Failed;
    }

    /// @brief 本次调用是否实际写出 packet。
    bool WasPublished() const noexcept {
        return status == PusherPublishStatus::Published;
    }

    /// @brief 成功写出 packet。
    static PusherPublishResult Published() {
        return {PusherPublishStatus::Published, std::nullopt};
    }

    /// @brief 本次调用被策略性丢弃，等待关键帧。
    ///
    /// 会话尚未收到首个关键帧，packet 被策略性丢弃，不被写入。
    /// 该场景下的 packet 会统计在丢包统计中，但不会被写入输出容器。
    static PusherPublishResult DroppedAwaitingKeyframe() {
        return {PusherPublishStatus::DroppedAwaitingKeyframe, std::nullopt};
    }

    /// @brief 本次调用失败，携带具体错误。
    static PusherPublishResult Failed(PusherError error) {
        return {PusherPublishStatus::Failed, std::move(error)};
    }
};


/// @brief 输出会话策略层。
/// 该层负责编排“什么时候允许写”，不直接接触 FFmpeg API，也不管理 AVPacket 的生命周期。
/// 目前只做了：
/// 1. 成功 Open 后等待关键帧；
/// 2. 丢弃等待期间的非关键视频包；
/// 3. 按时间轴策略保留时间戳或从首个接纳包建立 epoch；
/// 4. 首个关键帧成功写入后进入 Running；
/// 5. 底层写入失败后停止继续写入。
/// 待实现：自动重连、退避和多轨同步。
class PusherSession {
public:
    /// @brief 创建默认使用 FFmpegPusher 的输出会话。
    PusherSession();

    /// @brief 注入具体 Pusher，主要用于单元测试和后续 Publisher 工厂。
    explicit PusherSession(std::unique_ptr<IPusher> pusher);
    ~PusherSession();

    PusherSession(const PusherSession&) = delete;
    PusherSession& operator=(const PusherSession&) = delete;

    /// @brief 打开新的输出会话。若当前会话尚未关闭，先结束旧会话。
    PusherResult Open(const PusherSessionConfig& config);

    /// @brief 根据当前会话状态决定丢弃或转发一个已经编码的视频包。
    PusherPublishResult Publish(const MediaPacket& packet);

    /// @brief 幂等结束输出会话。
    PusherResult Close();

    /// @brief 当前会话状态。
    PusherSessionState State() const noexcept { return state_; }

private:
    PusherPublishResult forwardAcceptedPacket(const MediaPacket& packet);
    PusherResult applyPusherTimestampPolicy(MediaPacket& packet);
    static PusherError MakeError(PusherErrorCategory category, const char* message);

    std::unique_ptr<IPusher> pusher_;
    PusherSessionState state_{PusherSessionState::Closed};
    PusherTimestampPolicy timestamp_policy_{};
    /// @brief 时间戳基准点，用于后续包的时间戳归一化或偏移计算
    std::optional<std::int64_t> timestamp_epoch_;
    /// @brief 时间戳时间基，用于后续包的时间戳单位转换
    std::optional<Rational> timestamp_time_base_;
};
