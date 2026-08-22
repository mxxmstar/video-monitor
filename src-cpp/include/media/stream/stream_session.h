#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <boost/asio/io_context.hpp>

#include "media/media_packet.h"
#include "media/puller/i_puller.h"
#include "media/stream/jitterbuffer/adaptive_jitter_buffer.h"
#include "media/stream/source_config.h"
#include "media/stream/stream_info.h"


/// @brief 持有 puller 的媒体流会话
///
/// 管理一次拉流连接的生命周期，职责包括：
///   - 连接 / 断开 / 自动重连
///   - 读循环（通过 boost::asio::post 调度到 io_context）
///   - Watchdog 读超时检测（steady_timer）
///   - 码率统计（原子计数器 + 定时汇总）
///
/// 自身不持有 StreamInfo，通过回调向上层（MediaStreamSource）报告。
/// 自身不持有解码器 / pipeline / sink。
///
/// 不持有线程 —— ReadLoop 被 post 到构造时传入的 io_context 上，
/// 由外部线程池或 io_context::run() 驱动。方便配合线程池 / io_context 池统一管理。
class MediaStreamSession : public std::enable_shared_from_this<MediaStreamSession> {
public:
    enum class State {
        KIDLE,         ///< 空闲，未启动
        KCONNECTING,   ///< 连接中
        KCONNECTED,    ///< 已连接，正常拉流
        KRECONNECTING, ///< 断线重连中
        KSTOPPED,      ///< 已停止
        KERROR,        ///< 不可恢复错误
    };

    /// @brief 会话统计信息
    struct Stats {
        uint64_t bytes_received{0};   ///< 累计接收字节
        uint64_t packets_received{0}; ///< 累计接收包数
        double   bitrate{0.0};        ///< 当前码率（kbps）
        uint32_t reconnect_count{0};  ///< 累计重连尝试次数
    };

    explicit MediaStreamSession(boost::asio::io_context& io);
    ~MediaStreamSession();

    bool Start();
    void Stop();

    void SetPuller(std::unique_ptr<IPuller> puller);
    void SetEndpoint(const InputEndpointConfig& endpoint);
    void SetSessionConfig(const SessionConfig& config);

    // 对外接口，用于设置会话配置参数

    /// @brief 设置会话 URL
    void SetUrl(const std::string& url);
    /// @brief 设置重连间隔（毫秒）
    void SetReconnectIntervalMs(int ms);
    /// @brief 设置最大重连次数
    void SetMaxReconnectCount(int count);
    /// @brief 设置 Jitter Buffer 间隔（毫秒）
    void SetJitterBufferIntervalMs(int ms);
    /// @brief 设置 Jitter Buffer 配置
    void SetJitterBufferConfig(const AdaptiveJitterBuffer::Config& config);
    /// @brief 设置 Watchdog 间隔（毫秒）
    void SetWatchdogIntervalMs(int ms);

    using PacketCallback = std::function<void(std::shared_ptr<MediaPacket>)>;
    using StreamInfoCallback = std::function<void(const MultiStreamInfo&)>;
    using StateCallback = std::function<void(State)>;

    void SetPacketCallback(PacketCallback cb);
    void SetStreamInfoCallback(StreamInfoCallback cb);
    void SetStateCallback(StateCallback cb);

    State GetState() const;
    Stats GetStats() const;
    static const char* StateName(State state);

private:
    void readLoop();
    bool tryReconnect(int& reconnect_attempts);
    static std::chrono::milliseconds reconnectDelay(const ReconnectPolicy& config, int reconnect_attempt);
    bool waitForReconnectDelay(std::chrono::milliseconds delay);
    void notifyStreamInfo();
    void setState(State state);

    boost::asio::io_context& io_;   ///< 异步 I/O 上下文，用于调度 ReadLoop
    std::unique_ptr<IPuller> puller_; ///< 拉流器，负责从服务器拉取数据
    InputEndpointConfig endpoint_;   ///< 输入端点配置，包含 URL、端口、用户名、密码等
    SessionConfig session_config_;   ///< 会话配置，包含重连间隔、最大重连次数、Jitter Buffer 配置等

    std::atomic<State> state_{State::KIDLE}; ///< 会话当前状态
    std::atomic<bool> running_{false};  ///< 是否正在运行 ReadLoop
    std::thread read_thread_; ///< 读线程，负责从 puller 读取数据
    mutable std::mutex lifecycle_mutex_; ///< 生命周期互斥锁，用于保护 state_ 和 running_

    // 重连退避等待专用的条件变量。Stop() 设置 running_ 为 false 后通知它，
    // 使读线程无需等待完整退避时间即可退出。
    std::mutex reconnect_wait_mutex_;
    std::condition_variable reconnect_wait_cv_;

    mutable std::mutex stats_mutex_; ///< 统计信息互斥锁，用于保护 stats_
    Stats stats_;                    ///< 会话统计信息

    std::mutex cb_mutex_;            ///< 回调互斥锁，用于保护回调函数
    PacketCallback packet_cb_;       ///< 数据包回调函数，用于处理从 puller 接收到的数据包
    StreamInfoCallback streaminfo_cb_; ///< 流信息回调函数，用于处理从 puller 接收到的流信息
    StateCallback state_cb_;         ///< 状态回调函数话状态变化
};
