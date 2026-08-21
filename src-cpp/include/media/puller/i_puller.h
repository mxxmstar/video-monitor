#pragma once

#include <functional>
#include <memory>
#include <string>
#include <optional>
#include <utility>

#include "media/media_packet.h"
#include "media/stream/stream_info.h"
#include "media/puller/puller_config.h"

/// @brief 拉流器错误分类
/// @details 定义了不同类型的错误，包括取消、超时、认证、未找到、不支持协议、无效配置、无效媒体、网络、流结束、内部错误。
enum class PullErrorCategory {
    // None,    // 被 nullopt 代替
    Cancelled,
    Timeout,
    Authentication,
    NotFound,
    UnsupportedProtocol,
    InvalidConfiguration,
    InvalidMedia,
    Network,
    EndOfInput,
    Internal,
};

/// @brief 拉流器错误结构体
/// @details 包含错误分类、原生错误码、错误消息和是否可重试的标志，给到 session 处理。
/// @note 错误分类为 None 时，原生错误码为 0，错误消息为空，是否可重试为 false。
struct PullError {
    PullErrorCategory category;   ///< 错误分类
    int native_code;   ///< 原生错误码
    std::string message;   ///< 错误消息
    bool retryable;   ///< 是否可重试
};

/// @brief 打开流结果结构体
struct PullOpenResult {
    std::optional<PullError> error;   ///< 错误信息
    bool Succeed() const noexcept { return !error.has_value(); }
    static PullOpenResult Success() { return {}; }
    static PullOpenResult Failed(PullError error) { return {std::move(error)}; }
};

/// @brief 一次读取的结构化结果。
///
/// 旧接口只有 bool，无法区分“暂时没有数据”“本地文件结束”和“网络错误”。
/// MediaFlow SourceNode 必须依靠这些状态决定继续读取、发送 EOS 还是重连。
enum class PullReadStatus {
    Packet,          ///< 成功取得 packet，result.packet 非空
    NoData,          ///< 本次没有可交付 packet，可稍后继续读取
    EOS,             ///< 输入正常结束，不应按网络故障重连
    RetryableError,  ///< 暂时性 I/O 错误或超时，可以重连
    FatalError,      ///< 不可恢复的读取错误
    Stopped,         ///< Puller 已被关闭或读取被主动中断
};

/// @brief 读取媒体包结果结构体
struct PullReadResult {
    std::optional<PullError> error;   ///< 错误信息
    std::shared_ptr<MediaPacket> packet;   ///< 媒体包（成功时为有效对象，失败时为 nullptr）
    PullReadStatus status{PullReadStatus::FatalError};   ///< 读取状态
};



/// @brief 拉流器接口，定义了单次底层输入连接的最小协议契约。

/// 负责：
///  - 使用已经保存的具体配置打开一个 endpoint；
///  - 同步读取一个编码媒体包或返回结构化状态；
///  - 提供当前连接的 `MultiStreamInfo` 快照；
///  - 非阻塞请求终止当前阻塞 I/O；
///  - 幂等关闭并释放底层资源；
///  - 报告底层 native error 和必要的异步协议事件。
/// 不负责：
/// - 重连次数、退避或主备地址；
/// - Session 状态机和 generation；
/// - 上层订阅者；
/// - Graph 消息、EOS message 或 Edge 背压；
/// - Decoder/Encoder；
/// - 通用业务统计；
/// - 对不属于自身的配置做默认空处理。
class IPuller {
public:
    virtual ~IPuller() = default;

    // ==================== 生命周期 ====================

    /// @brief 打开流
    /// @param endpoint 输入端点配置
    /// @return 打开流结果结构体
    virtual PullOpenResult Open(const InputEndpointConfig& endpoint) = 0;

    /// @brief 关闭流
    virtual void Close() = 0;

    /// @brief 读取一个媒体包
    /// @return 读取媒体包结果结构体
    virtual PullReadResult ReadPacket() = 0;

    // ==================== 元数据 ====================

    /// @brief 获取流信息
    virtual MultiStreamInfo GetStreamInfo() const = 0;

    // ==================== 回调 ====================

    /// @brief 拉流器层事件回调（协议异常等）
    using EventCallback = std::function<void(const std::string&)>;

    /// @brief 设置事件回调
    virtual void SetEventCallback(EventCallback cb) = 0;
};
