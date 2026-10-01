#pragma once

#include <optional>
#include <string>
#include <utility>
#include <functional>

#include "media/media_packet.h"
#include "media/pusher/pusher_config.h"

/// @brief 输出端错误分类。
///
/// Pusher 映射底层错误并决定 retryable，Session 不需要理解 FFmpeg 错误码。
enum class PusherErrorCategory {
    InvalidConfiguration,  ///< 输出 URL 或轨道配置不完整
    InvalidState,          ///< 在未打开状态 Push 等生命周期错误
    UnsupportedMedia,      ///< Pusher 不支持当前媒体类型或编码
    InvalidPacket,         ///< packet 的元数据、载荷或后端句柄无效
    OpenFailed,            ///< 底层输出容器打开失败
    WriteFailed,           ///< 底层输出容器写包失败
    Internal,              ///< Pusher 实现或会话依赖未正确初始化
    Cancelled,
    Timeout,
    Authentication,
    NotFound,
    UnsupportedProtocol,
    Network,
    CloseFailed,
};

inline bool IsRetryablePusherError(PusherErrorCategory category) {
    switch (category) {
    case PusherErrorCategory::OpenFailed:
    case PusherErrorCategory::WriteFailed:
    case PusherErrorCategory::Timeout:
    case PusherErrorCategory::Network:
        return true;
    default:
        return false;
    }
}

/// @brief Pusher 对外暴露的结构化错误。
struct PusherError {
    PusherErrorCategory category;  ///< 错误分类
    std::string message;            ///< 面向调用方的错误说明
    bool retryable{false};          ///< 当前错误是否明确可以重试
};

/// @brief Pusher 调用结果。
///
/// 与 PullOpenResult 保持同一表达方式：成功时 error 为空，失败时携带
/// 一份具体错误，避免额外维护一个没有业务含义的 None 枚举值。
struct PusherResult {
    std::optional<PusherError> error;

    bool Succeed() const noexcept { return !error.has_value(); }
    static PusherResult Success() { return {}; }
    static PusherResult Failed(PusherError error) {
        return {std::move(error)};
    }
};

enum class PusherEventStage { Open, Write, Close };

struct PusherEvent {
    PusherEventStage stage;
    PusherError error;
};


/// @brief 单个底层输出连接的最小接口。
///
/// IPusher 负责把已经编码好的 packet 写入一个确定输出目标；它不决定
/// 哪个 packet 可以开始写入，也不管理重连。前述会话策略由后续的
/// PusherSession 统一实现，避免不同协议 Pusher 重复实现一套状态机。
class IPusher {
public:
    virtual ~IPusher() = default;

    /// @brief 打开一个输出目标。重复调用会结束此前的输出连接。
    virtual PusherResult Open(const PusherConfig& config) = 0;

    /// @brief 写入一个已经编码的媒体包。
    virtual PusherResult Push(const MediaPacket& packet) = 0;

    /// @brief 幂等关闭输出目标，并尽力完成容器尾部写入。
    virtual PusherResult Close() = 0;

    /// @brief 返回当前输出目标是否已经成功打开。
    virtual bool IsOpen() const = 0;

    /// @brief 线程安全、非阻塞且可重复；仅中断 I/O，不释放资源或同步触发事件回调。
    virtual PusherResult RequestStop() = 0;

    /// @brief 异步错误由 Session 串行处理；同步失败仍通过调用结果返回。
    using EventCallback = std::function<void(const PusherEvent&)>;

    /// @brief 设置事件回调。Close 后须停止旧连接的异步回调；清空回调返回后
    /// 不得再调用旧回调，以便 Session 安全析构。
    virtual void SetEventCallback(EventCallback cb) { (void)cb; }
};
