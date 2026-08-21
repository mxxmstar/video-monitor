#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>

/// @brief 拉流器类型
enum class PullerKind {
    Auto,
    FFmpeg,
    Avtp,
};

/// @brief 输入端点配置
struct InputEndpointConfig {
    std::string uri;
    PullerKind puller_kind{PullerKind::Auto};
};

/// @brief FFmpeg 延迟模式
enum class LatencyMode {
    Auto,
    Normal,
    Low,
};

/// @brief FFmpeg 通用 I/O 配置
struct FFmpegIoConfig {
    std::chrono::milliseconds connect_timeout{5000};     ///< 连接超时时间 , 默认 5s
    std::chrono::milliseconds read_timeout{10000};       ///< 读取超时时间 , 默认 10s
};

/// @brief FFmpeg 探测配置
struct FFmpegProbeConfig {
    std::optional<std::int64_t> probe_size_bytes;   ///< 探测大小
    std::optional<std::chrono::microseconds> analyze_duration;   ///< 分析时长
    int max_video_probe_packets{120};   ///< 最大视频探测包数
    std::chrono::milliseconds video_probe_timeout{3000};   ///< 视频探测超时时间 , 默认 3s
};

/// @brief RTSP 输入配置
struct RtspInputOptions {
    std::optional<std::string> transport;   ///< 传输协议
    std::optional<bool> prefer_tcp;   ///< 是否优先 TCP 连接
    std::optional<std::chrono::microseconds> socket_timeout;   ///< 套接字超时时间
    std::optional<int> reorder_queue_size;   ///< 重排序队列大小
    std::optional<std::int64_t> receive_buffer_bytes;   ///< 接收缓冲区大小
    std::optional<int> min_port;   ///< 最小端口
    std::optional<int> max_port;   ///< 最大端口
    std::optional<std::string> user_agent;   ///< 用户代理
    std::optional<std::string> ca_file;   ///< CA 证书文件
    std::optional<bool> tls_verify;   ///< 是否验证 TLS 证书
    std::optional<std::string> username;   ///< 用户名
    std::optional<std::string> password;   ///< 密码
};


/// @brief HTTP 输入配置
struct HttpInputOptions {
    std::map<std::string, std::string> headers;
    std::optional<std::string> user_agent;
    std::optional<std::string> referer;
    std::optional<std::string> cookies;
    std::optional<std::string> proxy;
    std::optional<bool> multiple_requests;
    std::optional<bool> reconnect;
    std::optional<bool> reconnect_on_network_error;
    std::optional<std::string> reconnect_on_http_error;
    std::optional<bool> reconnect_streamed;
    std::optional<int> reconnect_max_retries;
    std::optional<std::chrono::milliseconds> reconnect_delay_max;
    std::optional<bool> tls_verify;
};

/// @brief HLS demuxer 配置
struct HlsInputOptions {
    std::optional<int> live_start_index;
    std::optional<bool> prefer_x_start;
    std::optional<int> max_reload;
    std::optional<int> m3u8_hold_counters;
    std::optional<bool> http_persistent;
    std::optional<bool> http_multiple;
    std::optional<int> seg_max_retry;
};

/// @brief RTMP 输入配置
struct RtmpInputOptions {
    std::optional<std::string> live_mode;
    std::optional<int> buffer_ms;
    std::optional<std::string> app;
    std::optional<std::string> playpath;
    std::optional<std::string> subscribe;
    std::optional<std::string> flash_version;
    std::optional<std::string> page_url;
    std::optional<std::string> swf_url;
    std::optional<std::string> swf_hash;
    std::optional<int> swf_size;
    std::optional<bool> tcp_nodelay;
};

/// @brief SRT 输入配置
struct SrtInputOptions {
    std::optional<std::string> mode;
    std::optional<std::chrono::milliseconds> connect_timeout;
    std::optional<std::chrono::microseconds> io_timeout;
    std::optional<std::chrono::microseconds> latency;
    std::optional<std::chrono::microseconds> receive_latency;
    std::optional<std::chrono::microseconds> peer_latency;
    std::optional<std::string> stream_id;
    std::optional<std::string> passphrase;
    std::optional<int> key_length;
    std::optional<std::string> transmission_type;
    std::optional<std::int64_t> receive_buffer_size;
    std::optional<std::int64_t> send_buffer_size;
    std::optional<int> payload_size;
};

/// @brief Puller 诊断配置
struct PullerDiagnosticsConfig {
    bool dump_packets{false};
};

/// @brief FFmpeg Puller 配置
struct FFmpegPullerConfig {
    FFmpegIoConfig io;
    FFmpegProbeConfig probe;
    LatencyMode latency{LatencyMode::Auto};
    /// @brief 输入格式，“rtsp”、“v4l2”、“h264”等，为空时根据 URI 自动检测
    std::optional<std::string> input_format;

    std::optional<RtspInputOptions> rtsp;
    std::optional<HttpInputOptions> http;
    std::optional<HlsInputOptions> hls;
    std::optional<RtmpInputOptions> rtmp;
    std::optional<SrtInputOptions> srt;

    std::map<std::string, std::string> extra_av_options;
    PullerDiagnosticsConfig diagnostics;
};

/// @brief AVTP Puller 配置
struct AvtpPullerConfig {
    std::optional<std::string> device;
    std::optional<std::string> source_mac;
    std::optional<std::uint64_t> stream_id;
    std::optional<std::string> payload_format;
    std::optional<std::size_t> pcap_queue_size;
    std::optional<std::chrono::milliseconds> capture_read_timeout;
    std::optional<bool> promiscuous;

    std::optional<int> width;
    std::optional<int> height;
    std::optional<double> fps;
    std::optional<bool> audio_enabled;
    std::optional<std::string> audio_codec;
    std::optional<int> audio_sample_rate;
    std::optional<int> audio_channels;

    std::optional<std::string> timestamp_mode;
    std::optional<bool> probe_on_open;
    std::optional<std::chrono::milliseconds> probe_timeout;
    std::optional<std::size_t> probe_packet_limit;
};

/// @brief Puller 具体配置
using PullerSpecificConfig = std::variant<
    FFmpegPullerConfig,
    AvtpPullerConfig>;

/// @brief Puller 聚合配置
struct PullerConfig {
    PullerSpecificConfig specific;
};
