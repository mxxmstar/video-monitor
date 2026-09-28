# FFmpegPusher 与 FFmpegMuxer 设计文档

## 1. 概述

本文档介绍媒体输出链路中的两个核心组件：

- **FFmpegMuxer**：媒体封装层，负责将编码后的媒体包写入目标容器（MP4、FLV、TS 等）
- **FFmpegPusher**：FFmpeg 输出适配层，负责将统一的 `MediaPacket` 接口适配到 FFmpeg Muxer

```text
PusherSession -> FFmpegPusher -> FFmpegMuxer -> FFmpeg -> 输出目标
```

### 1.1 职责划分

| 组件 | 职责 | 不负责的 |
|---|---|---|
| **FFmpegMuxer** | FFmpeg 资源管理、容器写入、时间基转换 | 关键帧策略、重连、packet 校验 |
| **FFmpegPusher** | 配置解析、协议识别、错误映射、packet 校验 | 状态机、时间戳策略、重连 |

### 1.2 设计原则

- **Muxer 只管怎么写**：不关心 packet 从哪里来，是否应该发送
- **Pusher 管适配和校验**：不实现会话级策略（等待关键帧、重连等）
- **协议由配置决定**：新增 FFmpeg 支持的协议时，优先扩展配置而非新增类

---

## 2. FFmpegMuxer：媒体封装层

### 2.1 核心职责

FFmpegMuxer 是 FFmpeg 输出容器的直接管理者，负责：

1. 创建和配置 `AVFormatContext`
2. 创建 `AVStream` 并设置编解码参数
3. 设置中断回调（超时/停止检测）
4. 打开 I/O 输出（文件或网络）
5. 写入容器头部（`avformat_write_header`）
6. 写入媒体包（`av_interleaved_write_frame`）
7. 写入容器尾部（`av_write_trailer`）
8. 释放 FFmpeg 资源

### 2.2 架构设计

```
┌──────────────────────────────────────────────────────────────┐
│                      FFmpegMuxer                             │
│                                                              │
│  ┌────────────────┐    ┌────────────────┐    ┌────────────┐ │
│  │ MuxerOpenOpts  │    │ InterruptCtx   │    │  FFmpeg    │ │
│  │                │    │                │    │  Resources │ │
│  │ - output_url   │    │ - stop_req     │    │            │ │
│  │ - format_name  │    │ - timed_out    │    │ - format_  │ │
│  │ - io_options   │    │ - deadline     │    │   ctx_     │ │
│  │ - muxer_opts   │    └────────────────┘    │ - video_   │ │
│  └────────────────┘            │             │   stream_  │ │
│                                │             └────────────┘ │
│                                ▼                            │
│  ┌────────────────┐    ┌────────────────┐                   │
│  │ stop_generation│    │ beginOperation │                   │
│  │ (代际计数器)   │    │ (设置超时)     │                   │
│  └────────────────┘    └────────────────┘                   │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

### 2.3 核心接口

#### 2.3.1 Open()

```cpp
MuxerResult Open(const MuxerOpenOptions& options, 
                 const MediaTrackConfig& config,
                 std::optional<std::uint64_t> expected_stop_generation = std::nullopt);
```

**功能：**
1. 关闭旧的输出（如果已打开）
2. 检查代际是否匹配（防止在停止后继续打开）
3. 分配 `AVFormatContext`
4. 创建视频流并设置编解码参数
5. 设置中断回调
6. 打开 I/O（`avio_open2`）
7. 写入头部（`avformat_write_header`）

**代际检查：**
```cpp
const auto stop_generation = expected_stop_generation.value_or(stop_generation_.load());
// ...
if (stop_generation_.load() != stop_generation) {
    return failure(MuxerOperation::OpenIo, MuxerErrorCategory::Cancelled, AVERROR_EXIT);
}
```

#### 2.3.2 Write()

```cpp
MuxerResult Write(const MediaPacket& packet);
```

**功能：**
1. 检查是否已打开
2. 检查是否已请求停止
3. 将 `MediaPacket` 转换为 `AVPacket`
4. 转换时间基（输入 time_base → 输出流 time_base）
5. 写入数据包（`av_interleaved_write_frame`）

**注意：** 这是**消费式写入**，写入后 `AVPacket` 被 FFmpeg 消费，不能重用。

#### 2.3.3 Close()

```cpp
MuxerResult Close();
```

**功能：**
1. 写入尾部（如果头部已写入）
2. 关闭 I/O
3. 释放 `AVFormatContext`
4. 重置所有状态

**幂等性：** 可以安全地多次调用。

#### 2.3.4 RequestStop()

```cpp
void RequestStop();
```

**功能：**
1. 递增 `stop_generation_` 代际计数器
2. 设置 `interrupt_ctx_.stop_requested = true`

**效果：** FFmpeg 的中断回调会检测到停止请求，当前阻塞的写入操作会立即返回。

### 2.4 中断机制

#### 2.4.1 InterruptContext

```cpp
struct InterruptContext {
    std::atomic<bool> stop_requested{false};     ///< 是否请求停止
    std::atomic<bool> timed_out{false};          ///< 是否超时
    std::chrono::steady_clock::time_point deadline{...};  ///< 超时截止时间
};
```

#### 2.4.2 FFmpeg 中断回调

```cpp
format_ctx_->interrupt_callback.callback = [](void* opaque) -> int {
    auto* context = static_cast<InterruptContext*>(opaque);
    if (context->stop_requested.load()) {
        return 1;  // 强制停止
    }
    if (std::chrono::steady_clock::now() > context->deadline) {
        context->timed_out.store(true);
        return 1;  // 超时停止
    }
    return 0;  // 继续
};
format_ctx_->interrupt_callback.opaque = &interrupt_ctx_;
```

**工作原理：**
- FFmpeg 在网络写入时会周期性调用此回调
- 返回 1 表示中断，FFmpeg 会立即返回错误
- 返回 0 表示继续

#### 2.4.3 beginOperation()

每次写入操作前调用，设置超时截止时间：

```cpp
void beginOperation(std::chrono::milliseconds timeout) {
    interrupt_ctx_.timed_out.store(false);
    const auto now = std::chrono::steady_clock::now();
    interrupt_ctx_.deadline = now + timeout;
}
```

### 2.5 代际机制（stop_generation_）

`stop_generation_` 是一个原子递增计数器，用于区分不同的推流生命周期。

**递增时机：**
```cpp
void RequestStop() {
    stop_generation_.fetch_add(1);  // 每次请求停止时递增
    interrupt_ctx_.stop_requested.store(true);
}
```

**检查时机：**
```cpp
MuxerResult Open(..., std::optional<std::uint64_t> expected_stop_generation) {
    const auto stop_generation = expected_stop_generation.value_or(stop_generation_.load());
    // ... 执行打开操作 ...
    if (stop_generation_.load() != stop_generation) {
        return failure(..., MuxerErrorCategory::Cancelled, ...);
    }
}
```

**场景示例：**
```
时间线 ─────────────────────────────────────────────────────▶

Open(expected_gen=0)
     │
     │ 执行中...
     │
     │         RequestStop()
     │         stop_generation_ = 1
     │
     │ 检查: 1 != 0 → 返回 Cancelled!
     ▼
```

### 2.6 错误分类

```cpp
enum class MuxerErrorCategory {
    OpenFailed,      ///< 打开失败（分配上下文、创建流、打开 I/O 等）
    WriteFailed,     ///< 写入失败（网络错误、磁盘满等）
    Timeout,         ///< 超时
    Internal,        ///< 内部错误（内存分配失败等）
    Cancelled,       ///< 被请求停止
    CloseFailed,     ///< 关闭失败
};
```

### 2.7 配置选项

#### 2.7.1 MuxerOpenOptions

```cpp
struct MuxerOpenOptions {
    std::string output_url;                          ///< 输出 URL
    std::string format_name;                         ///< 输出格式（mp4, flv, mpegts 等）
    MuxerIoOptions io;                               ///< I/O 超时选项
    std::map<std::string, std::string> io_options;   ///< avio_open2 选项（网络传输）
    std::map<std::string, std::string> muxer_options; ///< avformat_write_header 选项（容器）
};
```

#### 2.7.2 MuxerIoOptions

```cpp
struct MuxerIoOptions {
    std::chrono::milliseconds open_timeout{5000};   ///< 打开超时
    std::chrono::milliseconds write_timeout{10000}; ///< 写入超时
};
```

### 2.8 成员变量

| 成员 | 类型 | 用途 |
|---|---|---|
| `format_ctx_` | `AVFormatContext*` | FFmpeg 输出上下文 |
| `video_stream_` | `AVStream*` | 视频流 |
| `output_url_` | `std::string` | 输出 URL |
| `header_written_` | `bool` | 是否已写入头部 |
| `io_` | `MuxerIoOptions` | I/O 超时配置 |
| `interrupt_ctx_` | `InterruptContext` | 中断回调上下文 |
| `stop_generation_` | `std::atomic<uint64_t>` | 停止代际计数器 |

---

## 3. FFmpegPusher：FFmpeg 输出适配层

### 3.1 核心职责

FFmpegPusher 是 `IPusher` 接口的 FFmpeg 实现，负责：

1. 解析 `PusherConfig`，转换为 `MuxerOpenOptions`
2. 识别输出类型（本地文件 vs 网络输出）
3. 校验输入 packet 的合法性
4. 调用 `FFmpegMuxer` 执行实际操作
5. 将 `MuxerError` 映射为 `PusherError`
6. 提供异步事件回调接口

### 3.2 架构设计

```
┌──────────────────────────────────────────────────────────────┐
│                     FFmpegPusher                             │
│                                                              │
│  ┌────────────────┐    ┌────────────────┐    ┌────────────┐ │
│  │ PusherConfig   │    │ FFmpegMuxer    │    │  Error     │ │
│  │                │    │                │    │  Mapping   │ │
│  │ - output_url   │───►│ - Open()       │───►│            │ │
│  │ - video_track  │    │ - Write()      │    │ MuxerError │ │
│  │ - ffmpeg_opts  │    │ - Close()      │    │   →        │ │
│  └────────────────┘    └────────────────┘    │ PusherError│ │
│                                              └────────────┘ │
│                                                              │
│  ┌────────────────┐                                         │
│  │ Protocol       │                                         │
│  │ Detection      │                                         │
│  │                │                                         │
│  │ - UrlScheme()  │                                         │
│  │ - IsNetwork()  │                                         │
│  │ - IsRtsp()     │                                         │
│  │ - IsRtmp()     │                                         │
│  └────────────────┘                                         │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

### 3.3 核心接口

#### 3.3.1 Open()

```cpp
PusherResult Open(const PusherConfig& config) override;
```

**功能：**
1. 如果已打开，先关闭旧连接
2. 解析配置，识别协议类型
3. 构建 `MuxerOpenOptions`
4. 调用 `muxer_.Open()`
5. 设置 `opened_ = true`

**协议识别：**
```cpp
bool IsNetworkOutput(const std::string& output_url) {
    const char* protocol = avio_find_protocol_name(output_url.c_str());
    // 检查是否为网络协议：tcp, udp, http, rtmp, srt, rtsp 等
}
```

#### 3.3.2 Push()

```cpp
PusherResult Push(const MediaPacket& packet) override;
```

**功能：**
1. 检查是否已打开
2. 校验 packet 类型（必须为 H.264 视频）
3. 校验 packet 数据有效性
4. 调用 `muxer_.Write(packet)`
5. 如果失败，映射错误并触发事件回调

**Packet 校验：**
```cpp
if (packet.type != MediaType::VIDEO) {
    return MakeFailure(PusherErrorCategory::UnsupportedMedia, ...);
}
if (packet.codec != CodecType::H264) {
    return MakeFailure(PusherErrorCategory::UnsupportedMedia, ...);
}
if (!packet.av_packet || !packet.av_packet->buf) {
    return MakeFailure(PusherErrorCategory::InvalidPacket, ...);
}
```

#### 3.3.3 Close()

```cpp
PusherResult Close() override;
```

**功能：**
1. 调用 `muxer_.Close()`
2. 设置 `opened_ = false`
3. 停止异步回调

**幂等性：** 可以安全地多次调用。

#### 3.3.4 RequestStop()

```cpp
PusherResult RequestStop() override;
```

**功能：**
1. 调用 `muxer_.RequestStop()`
2. 中断底层 FFmpeg 的阻塞操作

**特性：** 线程安全、非阻塞、可重复调用。

### 3.4 错误映射

FFmpegPusher 负责将 `MuxerError` 转换为 `PusherError`：

```cpp
PusherResult MapMuxerError(const MuxerError& error, bool network_output) {
    switch (error.category) {
        case MuxerErrorCategory::OpenFailed:
            return PusherResult::Failed(PusherError{
                PusherErrorCategory::OpenFailed,
                error.message,
                false  // 打开失败通常不可重试
            });
        
        case MuxerErrorCategory::WriteFailed:
            return PusherResult::Failed(PusherError{
                PusherErrorCategory::WriteFailed,
                error.message,
                network_output  // 网络错误可能可重试
            });
        
        case MuxerErrorCategory::Timeout:
            return PusherResult::Failed(PusherError{
                PusherErrorCategory::Timeout,
                "Write timeout: " + error.message,
                network_output  // 网络超时可能可重试
            });
        
        case MuxerErrorCategory::Cancelled:
            return PusherResult::Failed(PusherError{
                PusherErrorCategory::Cancelled,
                "Operation cancelled",
                false  // 用户取消不可重试
            });
        
        // ...
    }
}
```

### 3.5 配置解析

#### 3.5.1 URL 协议识别

```cpp
std::optional<std::string> UrlScheme(const std::string& url) {
    // 解析 URL 的 scheme 部分
    // "rtmp://host/live" → "rtmp"
    // "output.mp4" → nullopt
    // "C:\output.mp4" → nullopt (Windows 路径)
}
```

#### 3.5.2 协议专用配置

```cpp
ResolvedMuxerOutput ResolveMuxerOutput(const PusherConfig& config, PusherResult& error) {
    const auto scheme = UrlScheme(config.output_url);
    
    // RTSP 配置
    if (IsRtspScheme(scheme)) {
        options.format_name = "rtsp";
        // 添加 RTSP 专用选项
        if (config.ffmpeg.rtsp.has_value()) {
            AddOption(options.io_options, "rtsp_transport", "tcp", error);
        }
    }
    
    // RTMP 配置
    if (IsRtmpScheme(scheme)) {
        options.format_name = "flv";
        // 添加 RTMP 专用选项
    }
    
    // 本地文件
    if (!scheme.has_value()) {
        // 根据扩展名推断格式
    }
    
    return ResolvedMuxerOutput{options, network_output};
}
```

### 3.6 事件回调机制

```cpp
void SetEventCallback(EventCallback cb) override {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    event_cb_ = std::move(cb);
}
```

**用途：** 当底层发生异步错误（如网络断开）时，通过回调通知 PusherSession。

**当前状态：** 回调接口已定义，但 FFmpegPusher 尚未实际触发回调（为未来扩展预留）。

### 3.7 成员变量

| 成员 | 类型 | 用途 |
|---|---|---|
| `config_` | `PusherConfig` | 推流配置 |
| `muxer_` | `FFmpegMuxer` | 底层 FFmpeg Muxer |
| `opened_` | `bool` | 是否已打开 |
| `network_output_` | `bool` | 是否为网络输出 |
| `event_cb_` | `EventCallback` | 异步事件回调 |
| `callback_mutex_` | `std::mutex` | 保护回调的互斥锁 |
| `operation_mutex_` | `std::recursive_mutex` | 串行化操作的互斥锁 |

---

## 4. 错误处理链

### 4.1 错误传播路径

```
FFmpeg 返回错误码 (如 AVERROR(EPIPE))
        │
        ▼
┌───────────────────┐
│   FFmpegMuxer     │
│                   │
│ failure()         │
│   → MuxerError    │
│   → category      │
│   → native_code   │
│   → message       │
│   → operation     │
└─────────┬─────────┘
          │
          ▼
┌───────────────────┐
│   FFmpegPusher    │
│                   │
│ MapMuxerError()   │
│   → PusherError   │
│   → category      │
│   → message       │
│   → retryable     │
└─────────┬─────────┘
          │
          ▼
┌───────────────────┐
│  PusherSession    │
│                   │
│ 更新 state_       │
│ 更新 last_event_  │
│ 触发重连（未来）  │
└───────────────────┘
```

### 4.2 错误分类对比

| MuxerErrorCategory | PusherErrorCategory | retryable |
|---|---|---|
| OpenFailed | OpenFailed | false |
| WriteFailed | WriteFailed | network_output |
| Timeout | Timeout | network_output |
| Cancelled | Cancelled | false |
| Internal | Internal | false |
| CloseFailed | CloseFailed | false |

### 4.3 FFmpeg 错误码示例

| FFmpeg 错误码 | 含义 | 转换后 |
|---|---|---|
| `AVERROR(EPIPE)` | 连接断开 | WriteFailed |
| `AVERROR(ETIMEDOUT)` | 超时 | Timeout |
| `AVERROR(ENOMEM)` | 内存不足 | Internal |
| `AVERROR_EXIT` | 被中断 | Cancelled |
| `AVERROR(EINVAL)` | 参数错误 | InvalidPacket |

---

## 5. 使用示例

### 5.1 直接调用（不推荐，应通过 PusherSession）

```cpp
// 创建 Muxer
FFmpegMuxer muxer;

// 配置
MuxerOpenOptions muxer_opts;
muxer_opts.output_url = "output.mp4";
muxer_opts.format_name = "mp4";
muxer_opts.io.open_timeout = std::chrono::milliseconds(5000);
muxer_opts.io.write_timeout = std::chrono::milliseconds(10000);

MediaTrackConfig track_config;
track_config.video().width = 1920;
track_config.video().height = 1080;
track_config.time_base_num = 1;
track_config.time_base_den = 25;
track_config.extra_data = sps_pps_data;

// 打开
auto open_result = muxer.Open(muxer_opts, track_config);
if (!open_result.Succeed()) {
    LOG_ERROR("Open failed: {}", open_result.error->message);
    return;
}

// 写入
while (has_packets()) {
    MediaPacket packet = get_next_packet();
    auto write_result = muxer.Write(packet);
    if (!write_result.Succeed()) {
        LOG_ERROR("Write failed: {}", write_result.error->message);
        break;
    }
}

// 关闭
muxer.Close();
```

### 5.2 通过 FFmpegPusher 调用

```cpp
// 创建 Pusher
FFmpegPusher pusher;

// 配置
PusherConfig config;
config.output_url = "rtmp://localhost/live/stream";
config.video_track.codec = CodecType::H264;
config.video_track.width = 1920;
config.video_track.height = 1080;
config.video_track.time_base_num = 1;
config.video_track.time_base_den = 25;
config.video_track.extra_data = sps_pps_data;

// 打开
auto open_result = pusher.Open(config);
if (!open_result.Succeed()) {
    LOG_ERROR("Open failed: {}", open_result.error->message);
    return;
}

// 推送
while (has_packets()) {
    MediaPacket packet = get_next_packet();
    auto push_result = pusher.Push(packet);
    if (!push_result.Succeed()) {
        LOG_ERROR("Push failed: {}", push_result.error->message);
        break;
    }
}

// 关闭
pusher.Close();
```

### 5.3 通过 PusherSession 调用（推荐）

```cpp
// 创建 Session
PusherSession session;

// 配置
PusherSessionConfig config;
config.pusher.output_url = "output.mp4";
config.timestamp_policy.mode = PusherTimestampMode::StartAtZero;

// 打开
auto open_result = session.Open(config);
if (!open_result.Succeed()) {
    LOG_ERROR("Open failed");
    return;
}

// 发布
while (has_packets()) {
    MediaPacket packet = get_next_packet();
    auto publish_result = session.Publish(packet);
    
    if (publish_result.WasPublished()) {
        LOG_INFO("Published");
    } else if (publish_result.status == PusherPublishStatus::DroppedAwaitingKeyframe) {
        LOG_DEBUG("Waiting for keyframe");
    } else {
        LOG_ERROR("Publish failed");
        break;
    }
}

// 关闭
session.Close();
```

---

## 6. 支持的输出类型

### 6.1 本地文件

| 扩展名 | 格式 | 说明 |
|---|---|---|
| `.mp4` | MP4 | MPEG-4 容器 |
| `.flv` | FLV | Flash Video |
| `.ts` | MPEG-TS | 传输流 |
| `.mkv` | Matroska | MKV 容器 |

### 6.2 网络协议

| 协议 | URL 示例 | 格式 | 说明 |
|---|---|---|---|
| RTMP | `rtmp://host/live/stream` | FLV | 实时消息协议 |
| RTSP | `rtsp://host/live/stream` | RTSP | 实时流协议（客户端） |
| SRT | `srt://host:port` | MPEG-TS | 安全可靠传输 |
| HTTP | `http://host/upload` | 多种 | HTTP 上传 |
| TCP | `tcp://host:port` | 原始 | TCP 连接 |
| UDP | `udp://host:port` | 原始 | UDP 连接 |

---

## 7. 线程安全

### 7.1 FFmpegMuxer

| 操作 | 线程安全 | 说明 |
|---|---|---|
| Open() | 否 | 调用方需保证串行调用 |
| Write() | 否 | 调用方需保证串行调用 |
| Close() | 否 | 调用方需保证串行调用 |
| RequestStop() | 是 | 原子操作，可随时调用 |
| StopGeneration() | 是 | 原子读取 |
| IsTimeOut() | 是 | 原子读取 |

### 7.2 FFmpegPusher

| 操作 | 线程安全 | 说明 |
|---|---|---|
| Open() | 是 | 内部使用 operation_mutex_ |
| Push() | 是 | 内部使用 operation_mutex_ |
| Close() | 是 | 内部使用 operation_mutex_ |
| RequestStop() | 是 | 内部使用 operation_mutex_ |
| SetEventCallback() | 是 | 内部使用 callback_mutex_ |

---

## 8. 当前限制

### 8.1 FFmpegMuxer

- 仅支持单路 H.264 视频
- 不支持音频
- 不支持多轨
- 消费式写入（写入后 packet 不可重试）

### 8.2 FFmpegPusher

- 仅支持 H.264 视频
- 不实现自动重连
- 不等待关键帧
- 不缓存 packet
- 异步事件回调尚未实际使用

---

## 9. 未来扩展

### 9.1 FFmpegMuxer

- 支持音频轨道
- 支持多视频轨道
- 非消费式写入（`av_packet_ref`）
- 更精细的超时控制

### 9.2 FFmpegPusher

- 支持 AAC 音频
- 支持其他视频编码（H.265、VP9）
- 实现异步事件回调
- 支持多路输出

---

## 10. 与相关模块的对比

### 10.1 FFmpegMuxer vs FFmpegPusher

| 特性 | FFmpegMuxer | FFmpegPusher |
|---|---|---|
| **层级** | 底层 FFmpeg 封装 | 中间适配层 |
| **FFmpeg 依赖** | 直接调用 | 间接调用（通过 Muxer） |
| **错误类型** | MuxerError | PusherError |
| **配置** | MuxerOpenOptions | PusherConfig |
| **协议识别** | 无 | 有（URL 解析） |
| **Packet 校验** | 无 | 有（类型、codec） |
| **线程安全** | 部分 | 完全 |

### 10.2 与输入端对比

| 输出端 | 输入端 | 说明 |
|---|---|---|
| FFmpegMuxer | FFmpegPuller | 容器写入 vs 容器读取 |
| FFmpegPusher | FFmpegDecoder | 适配层 vs 解码层 |
| PusherSession | MediaStreamSession | 输出策略 vs 输入策略 |

---

## 11. 常见问题

### Q1: 为什么 Muxer 使用消费式写入？

**A:** 消费式写入（直接传递 `AVPacket` 所有权）可以减少一次 `av_packet_ref()` 拷贝，提高性能。但这也意味着写入失败后不能重试同一个 packet。未来如果需要重试，必须改为非消费式写入。

### Q2: 为什么 Pusher 不实现重连？

**A:** 重连是会话级策略，应该由 PusherSession 统一实现。如果每个 Pusher 都实现自己的重连逻辑，会导致代码重复和策略不一致。

### Q3: 如何判断是本地文件还是网络输出？

**A:** 通过 `avio_find_protocol_name()` 检查 URL 的协议类型。网络协议包括：tcp、udp、http、rtmp、srt、rtsp 等。

### Q4: RequestStop() 能立即停止 FFmpeg 吗？

**A:** 不能立即停止。FFmpeg 只在网络写入时周期性检查中断回调。如果 FFmpeg 正在执行非网络操作（如本地文件写入），可能需要等待操作完成。

### Q5: 为什么 operation_mutex_ 是 recursive_mutex？

**A:** 因为某些操作可能需要递归调用，例如 Open() 内部可能调用 Close()，Close() 可能再次调用 Close()（幂等）。

---

## 12. 总结

### 12.1 设计亮点

| 设计 | 说明 |
|---|---|
| **职责分离** | Muxer 只管 FFmpeg，Pusher 管适配和校验 |
| **错误映射** | 将 FFmpeg 错误码转换为结构化错误 |
| **中断机制** | 通过回调和代际机制实现优雅停止 |
| **协议无关** | 通过配置支持多种协议，不硬编码 |
| **线程安全** | Pusher 完全线程安全，Muxer 部分线程安全 |

### 12.2 核心原则

```text
FFmpeg 可以完成的协议 → 配置扩展
输出策略发生变化 → PusherSession 扩展
不再通过 FFmpeg 写出 → 新增 IPusher 实现
```

这种设计使得新增协议时只需修改配置，而不需要新增类；同时保持了清晰的职责边界，便于维护和测试。