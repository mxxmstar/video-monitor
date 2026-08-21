#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "media/puller/puller_config.h"

/// @brief Session 重连策略
struct ReconnectPolicy {
    bool enabled{true};   ///< 是否启用重连策略
    int max_attempts{-1};   ///< 最大重连次数，-1 表示无限重连
    std::chrono::milliseconds initial_delay{3000};   ///< 初始重连延迟
    double multiplier{1.0};   ///< 重连延迟倍数
    std::chrono::milliseconds max_delay{3000};   ///< 最大重连延迟
    std::chrono::milliseconds reset_after_stable{30000};   ///< 重置重连策略时间间隔
};

/// @brief Session watchdog 配置
struct WatchdogConfig {
    bool enabled{false};   ///< 是否启用 watchdog
    std::chrono::milliseconds check_interval{1000};   ///< 检查间隔
    std::chrono::milliseconds idle_timeout{10000};   ///< 空闲超时时间
};

/// @brief Session jitter buffer 配置
struct SessionJitterConfig {
    bool enabled{false};   ///< 是否启用 jitter buffer
    std::chrono::milliseconds release_interval{5};   ///< 释放间隔
    std::size_t capacity_packets{512};   ///< 缓冲区容量（单位：包数）
    std::chrono::milliseconds min_delay{20};   ///< 最小延迟
    std::chrono::milliseconds max_delay{200};   ///< 最大延迟
    std::chrono::milliseconds safety_margin{10};   ///< 安全边距
    double alpha{0.9};   ///< 平滑系数
};

/// @brief 媒体流 Session 配置
struct SessionConfig {
    ReconnectPolicy reconnect;   ///< 重连策略
    WatchdogConfig watchdog;     ///< watchdog 配置
    SessionJitterConfig jitter;   ///< jitter buffer 配置
    std::chrono::milliseconds no_data_backoff{1};   ///< 无数据超时时间间隔
};

/// @brief 业务源配置
struct SourceConfig {
    std::string source_id;   ///< 业务源 ID
    std::optional<std::uint64_t> numeric_stream_id;   ///< 数字流 ID（可选）
    bool cache_stream_info{true};   ///< 是否缓存流信息
    std::chrono::seconds stats_log_interval{0};   ///< 统计日志间隔
};

/// @brief 媒体输入聚合配置
struct MediaInputConfig {
    InputEndpointConfig endpoint;   ///< 输入端点配置
    PullerConfig puller;   ///< 拉流器配置
    SessionConfig session;   ///< Session 配置
    SourceConfig source;   ///< 业务源配置
};
