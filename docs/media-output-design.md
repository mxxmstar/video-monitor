# Media 输出链路推进计划

## 1. 目标

当前工程已经完成了 `FFmpegPuller -> FFmpegDecoder -> MediaFrameConverter -> FFmpegEncoder -> FFmpegMuxer` 的基本实验链路。

当前正在实现的是原工程的 `PushClient` 路线：程序主动连接一个输出目标，
将已经编码好的媒体包交给 FFmpeg 封装并发送。这个路线中的协议差异，优先由
FFmpeg 的输出 URL、format 和协议选项解决；不需要为 RTMP、RTSP Client、SRT
或 MPEG-TS 等协议分别创建一个 Pusher。

后续不直接照搬原工程 `Publisher -> ProtocolAdapter -> Protocol -> MuxerCore` 的职责划分，而是建立一套更容易理解和维护的输出结构：

```text
Muxer -> Pusher -> PusherSession -> Publisher
```

第一条目标链路只支持：

```text
编码视频包 -> PusherSession -> FFmpegPusher -> FFmpegMuxer -> MP4 文件
```

完成这条链路后，可以先通过修改 FFmpeg 输出配置验证网络 PushClient；
再单独增加 RTSP Server/PullServer 路线。后者是监听端口并向客户端分发媒体，
职责与 PushClient 不同。

## 2. 四层职责

### 2.1 Muxer：媒体封装层

代表类：`FFmpegMuxer`

Muxer 只关心如何把已经编码好的媒体包写入目标容器或输出格式：

- 创建和配置 `AVFormatContext`；
- 创建 `AVStream`；
- 设置 codec parameters 和 extradata；
- 将输入 packet 的时间基转换为输出流时间基；
- 调用 `av_interleaved_write_frame()`；
- 写 trailer 并释放 FFmpeg 资源。

Muxer 不负责：

- 从哪里获取 packet；
- 是否等待关键帧；
- 是否重连；
- 是否丢包；
- 是否把多个输出目标组织成一个发布任务。

当前阶段的 `FFmpegMuxer` 只实现单路 H.264 视频，先不扩展音频和多轨。

### 2.2 Pusher：FFmpeg 输出适配层

代表类：`FFmpegPusher`

在当前 PushClient 路线中，Pusher 持有 FFmpeg Muxer，并把统一的
`MediaPacket` 输出接口适配到 FFmpeg：

- 接收 `MediaPacket`；
- 校验 packet 是否符合当前输出配置；
- 调用 `FFmpegMuxer::Open/Write/Close`；
- 将底层错误转换为 Pusher 层结果；
- 保存当前输出目标和基本统计。

`FFmpegPusher` 不是某一种协议的实现类。只要目标协议和目标封装格式由
FFmpeg 支持，就继续使用同一个 `FFmpegPusher -> FFmpegMuxer`：

```text
output_url = "output.mp4"              -> FFmpeg muxer -> MP4
output_url = "rtmp://host/live/stream" -> FFmpeg muxer -> FLV/RTMP
output_url = "rtsp://host/live/stream" -> FFmpeg muxer -> RTSP Client
output_url = "srt://host:port"          -> FFmpeg muxer -> SRT
```

因此，新增一种 FFmpeg 已支持的 PushClient 协议时，优先扩展配置，而不是
增加 `RtmpPusher`、`RtspPusher` 或 `SrtPusher`。未来配置可以逐步补充：

```cpp
struct FFmpegOutputConfig {
    std::string output_url;  // 由 URL 推断协议和默认封装格式
    std::string format_name; // 可选，例如 flv、rtsp、mpegts
    std::map<std::string, std::string> options;
};
```

`FFmpegPusher` 仍然只负责生命周期、packet 校验和错误转换；
`FFmpegMuxer` 负责把 format、协议参数和时间基传给 FFmpeg。Publisher 和
PusherSession 不应根据 `rtmp`、`rtsp` 等字符串编写协议分支。

第一版 `FFmpegPusher` 只做同步转发，不实现：

- 自动重连；
- 等待关键帧；
- DTS 交织队列；
- 后台线程；
- 多路输出。

Pusher 是“怎么写到这个输出”的实现，不是“什么时候允许写”的策略层。

### 2.3 PusherSession：发布会话编排层

代表类：`PusherSession`

PusherSession 负责一次输出会话的生命周期和策略：

- 管理 `Closed/Opening/WaitingForKeyframe/Running/Stopping/Failed` 状态；
- 创建、打开和关闭 Pusher；
- 判断当前 packet 是否允许发送；
- 首次启动时等待视频关键帧；
- 记录输出会话的首个时间戳，并按策略进行时间轴归零；
- 处理 Pusher 错误；
- 后续负责重连、退避以及重连后的关键帧恢复。

PusherSession 不直接调用 FFmpeg API，也不直接操作 `AVFormatContext`。

这里的 Session 是“输出端生产会话”，与输入端的 `MediaStreamSession` 不同：

- 输入 `MediaStreamSession` 管理 Puller、读循环和输入重连；
- 输出 `PusherSession` 管理 Pusher、写入策略和输出重连。

### 2.4 Publisher：对外发布门面层

代表类：`Publisher`

Publisher 是应用或上层 Pipeline 使用的统一入口：

- 接收发布配置；
- 根据发布类型创建对应的 PusherSession；
- 对外提供统一的 `Open/Publish/Close` 接口；
- 隐藏 Pusher 和 PusherSession 的具体类型；
- 汇总状态、错误和统计信息。

Publisher 不实现 FFmpeg 封装细节，也不实现 RTSP 控制协议细节。

## 3. 与原工程的对应关系

原工程的模块可以大致映射到新结构，但不是一一复制：

| 原工程模块 | 新结构中的位置 | 说明 |
|---|---|---|
| `DefaultPublisher` | `Publisher` | 对外门面和创建具体实现 |
| `FfmpegProtocolAdapter` | `FFmpegPusher` 的部分职责 | MediaPacket 路由和模型转换 |
| `FfmpegMuxProtocol` | `FFmpegPusher` + `PusherSession` 的拆分职责 | 原工程同时包含协议、重连和关键帧策略 |
| `FfmpegMuxerCore` | `FFmpegMuxer` | FFmpeg stream、时间基和写包 |
| `PublisherSinkNode` | `PusherSession` 的部分职责 | 原工程还混合了 MediaFlow 节点、队列和多轨逻辑 |
| `RtspServerProtocol` | 后续 PullServer 的 Protocol 层 | 监听、建轨、RTP/RTCP 和客户端分发 |
| `RtspClientSession` | 后续 PullServer 内部对象 | 管理单个播放客户端，不是通用 PusherSession |

原工程中没有 `pusher` 目录，是因为它把“输出协议适配”和“发布会话策略”主要放进了 `Protocol`、`ProtocolAdapter` 和 `PublisherSinkNode`。本版本主动拆开这些职责，目的是学习和维护，而不是保持原工程的类名一致。

### 3.1 PushClient 与 PullServer 的边界

两条路线虽然都属于媒体输出，但工作模型不同：

```text
PushClient：主动连接一个目标
编码包 -> PusherSession -> FFmpegPusher -> FFmpegMuxer -> 远端服务

PullServer：监听并服务多个客户端
编码包 -> 发布轨道/Server -> Protocol -> RtspClientSession(s) -> RTP/RTCP 客户端
```

PushClient 主要需要解决：

- FFmpeg 输出格式和 URL；
- 编码包到输出流的映射；
- 时间基转换；
- 连接失败和会话重连。

PullServer 还需要解决：

- 监听端口和接受连接；
- OPTIONS、DESCRIBE、SETUP、PLAY、TEARDOWN；
- 多个客户端共享同一媒体轨道；
- RTP 打包、RTCP 和传输模式；
- 慢客户端、客户端断开和鉴权。

所以，原工程的 `Protocol` 主要在 PullServer 路线中变得必要。不能因为
两条路线都使用 RTSP，就把 RTSP Server 的客户端控制和 RTP 逻辑塞进
`FFmpegPusher`。

### 3.2 新增输出能力时的判断规则

按下面的顺序决定扩展位置：

| 变化 | 扩展位置 |
|---|---|
| 只是更换 FFmpeg 支持的 URL 协议或封装格式 | `PusherConfig`、`FFmpegOutputConfig`、`FFmpegMuxer` |
| FFmpeg 支持该协议，但需要专用连接参数 | FFmpeg 输出 options，不新增 Pusher 子类 |
| 不再通过 FFmpeg 写出，使用独立网络库发送单个目标 | 新增 `IPusher` 实现 |
| 需要监听端口、管理多个客户端和 RTP/RTCP | 新增 PullServer/Protocol/ClientSession 路线 |
| 同时发布到多个目标 | 后续新增 `MultiPublisher` 或统一多目标编排层 |
| 主备切换或统一健康检查 | 后续新增 Publisher 装饰器或管理层 |

核心原则是：

```text
FFmpeg 可以完成的 PushClient 协议 -> 配置扩展
输出策略发生变化 -> Publisher/PusherSession 扩展
服务端协议和多客户端模型 -> Protocol/ServerSession 扩展
```

## 4. 推进阶段

### 阶段 0：固定当前 Muxer 基线

目标：确保 Muxer 自己可以被独立验证。

任务：

- 固定单路 H.264 MP4 输出；
- 验证编码器时间基到流时间基的转换；
- 验证输出时间轴从预期位置开始；
- 验证关闭顺序为 `Flush -> Write remaining packets -> Close`；
- 统计所有 `Write()` 失败，测试失败时不能只打印日志。

完成条件：

- 输入固定数量的视频编码包；
- 输出 MP4 能被 `ffprobe` 正常识别；
- 视频时长、帧数、帧率与输入一致；
- `Close()` 后文件可以正常读取。

暂不做：音频、多轨、RTSP、重连。

### 阶段 1：整理 Muxer 的所有权契约

目标：明确 `Write()` 是否消费输入 packet。

当前 `FFmpegMuxer::Write(const MediaPacket&)` 会直接操作其内部 `AVPacket`。因此必须明确以下规则之一：

方案 A：非消费式写入。

- Muxer 内部通过 `av_packet_ref()` 创建自己的 packet；
- `Write()` 返回后调用方仍然可以使用或重试原 packet；
- 适合将来支持重连和异步队列。

方案 B：消费式写入。

- Muxer 直接取得上游 `AVPacket`；
- 调用 `av_interleaved_write_frame()` 后 packet 不得再次使用；
- 接口和注释必须明确所有权转移，最好不要继续使用带 `const` 的参数表达这一语义。

当前学习版可以先选择方案 B，以减少一次 packet 引用；但在进入重连和异步队列前，必须重新评估方案 A。

完成条件：

- 用单元测试验证成功写入后 packet 的状态；
- 用失败路径测试验证不会 double free；
- 明确 Pusher 和 Session 是否允许重试同一个 packet。

### 阶段 2：实现 FFmpegPusher

目标：把 `FFmpegMuxer` 包装成一个清晰的输出协议实现。

建议接口：

```cpp
class IPusher {
public:
    virtual ~IPusher() = default;

    virtual PusherResult Open(const PusherConfig& config) = 0;
    virtual PusherResult Push(const MediaPacket& packet) = 0;
    virtual PusherResult Close() = 0;
};

class FFmpegPusher final : public IPusher {
public:
    PusherResult Open(const PusherConfig& config) override;
    PusherResult Push(const MediaPacket& packet) override;
    PusherResult Close() override;
};
```

第一版内部只持有：

```text
PusherConfig
FFmpegMuxer
opened 状态
最近一次错误
```

`FFmpegPusher::Push()` 只做：

1. 检查 Pusher 是否已打开；
2. 检查 packet 类型、codec 和 buffer；
3. 调用 `FFmpegMuxer::Write()`；
4. 将错误转换成 `PusherResult`。

完成条件：

- 原测试中的 `muxer.Write(*packet)` 可以替换为 `pusher.Push(*packet)`；
- 输出 MP4 结果不发生变化；
- Pusher 不包含关键帧等待和重连代码。

### 阶段 3：实现 PusherSession

目标：把输出会话策略从具体 Pusher 中分离出来。

建议接口：

```cpp
class PusherSession {
public:
    PusherResult Open(const PusherSessionConfig& config);
    PusherResult Publish(const std::shared_ptr<MediaPacket>& packet);
    PusherResult Close();

    PusherSessionState State() const;
};
```

第一版只实现以下策略：

- `Open()` 成功后进入 `WaitingForKeyframe`；
- 非关键视频包被丢弃，并返回明确的 `AwaitingKeyframe` 结果；
- 第一个关键帧写入成功后进入 `Running`；
- `Close()` 幂等；
- Pusher 写入失败后进入 `Failed`，暂时不自动重连。

时间戳归零也放在这一层，因为它是输出会话策略，而不是 FFmpeg 封装规则：

```text
session_pts = packet_pts - first_packet_pts
```

不要修改输入 Puller 或 Decoder 的原始时间轴。

完成条件：

- 非关键帧不会成为 MP4 的第一个输出包；
- 第一关键帧写入后状态变为 `Running`；
- 写入失败后不会继续向失效 Pusher 写包；
- Session 测试可以使用脚本 Pusher，不依赖真实 RTSP。

暂不做：自动重连、DTS 交织、多轨等待。

### 阶段 4：实现 Publisher 门面

目标：让上层不依赖 `FFmpegPusher` 和 `PusherSession` 的具体类型。

Publisher 负责：

- 校验 `PublisherConfig`；
- 根据 `PublisherKind` 选择 Pusher；
- 创建并持有一个 `PusherSession`；
- 转发 `Publish()`；
- 暴露状态、错误和统计信息。

第一版只注册：

```text
PublisherKind::Client
```

`Client` 与 `Server` 表示不同的交互模型，而不是 FFmpeg、MP4、RTSP 或
ZLMediaKit 的名称。第一版 Client 路线使用 `FFmpegPusher`，它既可以输出
本地文件，也可以主动推送到 FFmpeg 支持的网络目标。未来增加非 FFmpeg 的
Client 后端时，新增 `IPusher` 实现；未来实现监听和多客户端分发时，使用
`PublisherKind::Server` 的独立 Server/Protocol/ClientSession 路线。

不要在这一阶段加入 RTSP Server、WebRTC 或 RTP UDP。RTSP Server 属于后续
PullServer 路线，不是当前 FFmpeg PushClient 的下一种 URL 配置。

### 阶段 5：补充输出重连

目标：在会话层实现可解释的输出故障恢复。

重连策略放在 `PusherSession`：

```text
Push 失败
    -> 关闭旧 Pusher
    -> 按配置等待
    -> 创建并打开新 Pusher
    -> 进入 WaitingForKeyframe
    -> 等待新的关键帧
    -> 恢复 Running
```

注意：如果 Muxer 使用消费式写入，失败后的原 packet 不能重试。因此恢复时默认丢弃失败包，等待下一关键帧。若业务要求重试当前包，必须改用非消费式 packet 契约或建立可重放队列。

完成条件：

- 重连次数有上限；
- 退避等待可测试；
- 重连期间非关键视频包不会写入新会话；
- 新关键帧到达后恢复写入；
- 停止操作可以中断重连等待。

### 阶段 6：接入完整学习测试

目标：验证真实媒体链路和各层边界。

测试链路：

```text
RTSP
  -> FFmpegPuller
  -> FFmpegDecoder
  -> MediaFrameConverter
  -> FFmpegEncoder
  -> Publisher
  -> PusherSession
  -> FFmpegPusher
  -> FFmpegMuxer
  -> test.mp4
```

测试至少验证：

- 编码器输出 packet 的时间基；
- PusherSession 是否正确等待关键帧；
- Muxer 是否正确转换输出流时间基；
- 输出文件是否从预期时间开始；
- 输出帧数和时长是否合理；
- Flush 和 Close 是否完整写出尾部数据。

测试分为两类：

1. 离线合成测试：固定帧和固定时间戳，保证稳定回归；
2. RTSP 实流测试：验证真实网络、输入 PTS、帧率变化和连接失败。

RTSP 测试不能替代离线测试，因为网络流的首个时间戳、帧率和连接状态不是确定的。

## 5. 后续 PullServer / RTSP Server 路线

RTSP Server 不是当前 PushClient 的另一个 FFmpeg URL。它是 PullServer 路线：
程序监听端口，接受客户端请求，再把编码媒体分发给一个或多个客户端。
因此不能把 RTSP Server 的客户端控制逻辑塞进 `FFmpegPusher`。

后续结构建议为：

```text
PullServer / Publisher Server
    -> RtspServer
        -> RtspProtocol
            -> RtspClientSession(s)
                -> RTP/RTCP transport
```

这里有两种不同含义的 Session：

- `PusherSession`：面向 PushClient 的媒体生产者，管理一次主动输出任务；
- `RtspClientSession`：面向单个 RTSP 播放客户端，管理 OPTIONS、DESCRIBE、SETUP、PLAY、TEARDOWN 和 RTP/RTCP。

如果后续仍希望由统一的应用层 `Publisher` 暴露入口，可以在 Publisher
下面增加 PullServer 类型；但它内部应使用独立的 Server/Protocol/ClientSession
组件，而不是复用 `FFmpegPusher` 来实现监听和多客户端分发。两者不能混成
一个通用 Session。

RTSP Server 路线的实现顺序：

1. 先实现独立于 FFmpegPusher 的单路 H.264、单客户端、TCP interleaved；
2. 再支持多个客户端共享同一发布轨道；
3. 再加入 UDP RTP/RTCP；
4. 再加入鉴权、慢客户端隔离和连接数限制；
5. 最后处理多轨、时间戳映射和完整统计。

第一版 RTSP Server 不同时实现 UDP、鉴权、多轨和限流。

## 6. 当前明确不做的事情

- 不复制原工程全部 Publisher/Protocol 类名；
- 不在 `FFmpegMuxer` 中实现重连和关键帧策略；
- 不在 `FFmpegPusher` 中实现会话级丢包策略；
- 不在第一版 Publisher 中支持所有协议；
- 不在 MP4 单视频验证完成前引入音频多轨；
- 不在真实 RTSP 推流之前引入异步队列和线程池；
- 不把输入 `MediaStreamSession` 和输出 `PusherSession` 合并成一个通用类。

## 7. 建议的下一步

当前直接进入阶段 1 和阶段 2：

1. 先确认 `FFmpegMuxer::Write()` 的 packet 所有权语义；
2. 给 Muxer 增加独立的单视频写入测试；
3. 定义最小 `PusherResult` 和 `PusherConfig`；
4. 实现 `IPusher` 和 `FFmpegPusher`；
5. 把现有 Puller/Decoder/Converter/Encoder 测试接到 `FFmpegPusher`；
6. 验证输出结果与当前直接调用 Muxer 时一致。

完成 `FFmpegPusher` 后再实现 `PusherSession`，这样每一层的引入原因都可以通过测试直接观察到。

## 8. 推流配置扩展计划（2026-09-17 追加）

本节是下一轮实现计划，更新第 7 节的推进顺序；结构体和示例均为拟议接口，
不表示当前代码已经支持这些配置或完成 ZLMediaKit 联调。
下一步先补齐输出配置，再验证网络推流，之后实现 Session 自动重连。

### 8.1 对齐 Puller 的组织方式

参考 `src-cpp/include/media/puller/puller_config.h`，采用通用参数、协议专用
参数和 FFmpeg 扩展参数分组。复用设计方式，不直接复用输入端结构体：

| Puller 配置 | 输出端对应设计 |
|---|---|
| 输入 URI | 保留 `PusherConfig::output_url`，地址只保存一份 |
| connect/read timeout | `PusherConfig::io` 中的 connect/write timeout |
| `FFmpegPullerConfig::input_format` | `FFmpegPusherConfig::output_format` |
| `RtspInputOptions` 等 | 独立的 `RtspOutputOptions`、`RtmpOutputOptions` |
| `extra_av_options` | 按 AVIO 和 muxer 两个接收入口分组 |
| probe、分析时长、接收重排队列 | 输出端不引入；轨道参数由上游提供 |

当前只有 FFmpeg 输出实现，先增加 `PusherConfig::ffmpeg`，不提前引入
只有一个成员的 variant 或重复的 PusherKind。将来增加非 FFmpeg 后端时，
再将具体后端配置收敛为类似 `PullerSpecificConfig` 的 variant。

### 8.2 配置归属

| 层 | 配置与职责 |
|---|---|
| Publisher | 选择输出实现、提供应用入口，不重复保存 URL、协议参数 |
| PusherSession | 关键帧等待、重连次数、退避、恢复策略 |
| Pusher | 输出地址、轨道、IO 超时、格式与协议选项；校验、转换和错误映射 |
| Muxer | 接收转换后的执行参数；管理 FFmpeg context、字典、deadline 和中断回调 |

IO 的用户配置归 Pusher，执行状态归 Muxer。Muxer 不读取
`PusherConfig`，不返回 `PusherError`，也不决定是否重连。
轨道描述应放到共享媒体配置头文件，避免 Muxer 为使用轨道类型而包含
Pusher 配置头文件。

### 8.3 第一批配置定义

第一批覆盖本地文件、RTSP Client 和 RTMP Client，保留当前单路 H.264 限制。
以下示意使用 `std::optional` 区分“未指定”和“显式设置”：

```cpp
enum class RtspOutputTransport { Tcp, Udp };

struct RtspOutputOptions {
    RtspOutputTransport transport{RtspOutputTransport::Tcp};
};

struct RtmpOutputOptions {
    std::optional<std::string> app;
    std::optional<std::string> playpath;
    std::optional<bool> tcp_nodelay;
};

struct FFmpegPusherConfig {
    std::optional<std::string> output_format;
    std::optional<RtspOutputOptions> rtsp;
    std::optional<RtmpOutputOptions> rtmp;
    std::map<std::string, std::string> extra_io_options;
    std::map<std::string, std::string> extra_muxer_options;
};

struct PusherConfig {
    std::string output_url;
    MediaTrackConfig video_track;
    PusherIoConfig io;
    FFmpegPusherConfig ffmpeg;
};
```

`PusherIoConfig` 保留 `connect_timeout{5000ms}` 和
`write_timeout{10000ms}`；0 表示禁用对应 deadline，负数无效。
`connect_timeout` 覆盖整个打开过程，包括 AVIO 连接和 write_header 中的
协议协商；`write_timeout` 用于写包及关闭阶段的阻塞操作。
这些是依赖 FFmpeg 中断回调的超时约束，不承诺能强制中断所有系统调用。

第一版认证参数通过目标 URL 提供，不增加 `zlmediakit` 专用配置。
URL 中的用户名、密码、查询令牌必须在日志和诊断信息中脱敏。
后续若增加独立认证字段，应明确与 URL 内凭据冲突时的处理规则。

SRT 的 mode、stream_id、latency、passphrase 和 TLS 证书选项延后加入。
输入端的 RTMP live_mode、subscribe、HTTP reconnect 等选项不直接复制，
每个输出字段都必须确认当前 FFmpeg 构建确实支持其发送端语义。

### 8.4 输出格式解析

Pusher 在打开任何输出资源之前解析格式，不能只依赖 FFmpeg 根据 URL 扩展名猜测：

| 目标 | 默认输出格式 | 规则 |
|---|---|---|
| 本地文件、file URL | 根据扩展名推断 | 无扩展名或无法推断时要求显式配置 |
| rtsp URL | `rtsp` | 使用 RTSP muxer，由 write_header 建立发布会话 |
| rtmp、rtmps URL | `flv` | 使用 FLV muxer，经 AVIO 连接服务器 |
| srt URL | 后续阶段默认 `mpegts` | 需先验证 FFmpeg 构建支持 SRT |
| 其他网络协议 | 要求显式配置 | 不猜测容器，不声称已支持 |

显式 `output_format` 优先于默认推断，但与第一批协议约束冲突时返回
`InvalidConfiguration`，例如 RTMP + MP4、RTSP + FLV。
显式空字符串视为无效，未设置才表示自动解析。
协议名解析需要处理大小写，并识别 Windows 盘符，不能把 `C:\\...` 当作网络协议。
格式、协议是否可用还需要查询当前 FFmpeg 构建能力；配置存在不等于库已启用对应模块。

### 8.5 转换为 Muxer 执行参数

由 Pusher 生成独立参数，Muxer 只执行：

```cpp
struct MuxerOpenOptions {
    std::string output_url;
    std::string format_name;
    MuxerIoOptions io;
    std::map<std::string, std::string> io_options;
    std::map<std::string, std::string> muxer_options;
    bool normalize_timestamps;
};

// Shared MediaTrackConfig is independent of PusherConfig.
MuxerResult Open(const MuxerOpenOptions& options,
                 const MediaTrackConfig& track);
```

映射规则：

| 来源 | FFmpeg 选项或调用入口 |
|---|---|
| 解析后的 format_name | `avformat_alloc_output_context2` 的 format_name |
| RTSP transport | `rtsp_transport=tcp/udp`，传入 write_header 字典 |
| RTMP app、playpath、tcp_nodelay | `rtmp_app`、`rtmp_playpath`、`tcp_nodelay`，传入 AVIO 字典 |
| extra_io_options | `avio_open2(..., &io_dict)` |
| extra_muxer_options | `avformat_write_header(..., &muxer_dict)` |
| connect/write timeout | Muxer deadline 和 interrupt callback |

RTSP 属于 `AVFMT_NOFILE`，不调用 `avio_open2`；第一版拒绝为该目标配置
非空 extra_io_options，避免静默忽略。以后需要 RTSP 内部 socket 参数时，
应验证其在 RTSP muxer 的选项入口，再增加对应字段。

结构化字段与同入口扩展字典出现同名选项时返回配置错误，避免隐含覆盖优先级。
连接与写入超时以结构化 IO 字段为唯一来源；扩展字典中的超时或自动重连选项
需拒绝或在后续建立明确语义后开放，防止与 deadline、Session 策略冲突。

Muxer 用独立 AVDictionary 执行调用，所有成功、失败路径都释放字典。
成功调用后检查未消费的选项：返回包含选项名和阶段的结构化错误，由 Pusher
映射为不可重试的配置错误，不能静默成功。该检查可能发生在资源已经打开之后，
失败时必须完成清理；诊断不输出可能包含凭据的选项值。

### 8.6 参数校验与错误边界

Pusher 负责以下检查：

- URL、轨道类型、H.264 编码能力、尺寸、帧率、时间基和 extradata 长度合法；
- 超时非负；RTSP/RTMP 专用字段与目标协议一致，禁止同时启用不相关的协议组；
- 格式与协议组合有效，选项无冲突，枚举值受支持；
- Push 的媒体类型、后端句柄、载荷、时间基和 duration 符合约定。

H.264 推流需要可用的 SPS/PPS。第一批 ZLMediaKit 联调要求上游在 Open 前提供
有效 extradata，并在开始发送时提供可解码关键帧；仅检查 extra_data 非空并不能
证明其合法。需要验证编码器输出的 Annex B/AVCC 与所选 muxer 的兼容性，
不能靠改格式名称假定转换正确。后续再考虑从首包提取参数或增加 bitstream filter。

Muxer 保留 FFmpeg 原始错误码、错误文本和操作阶段；对于未消费选项等自定义错误，
提供明确的本地原因，不伪造 FFmpeg 返回码。Pusher 映射为统一错误和 retryable。
Session 只消费映射结果，不解析错误文本或 AVERROR。

网络超时、连接中断、可识别的服务端暂时错误可以标记可重连；配置错误、
协议不支持、认证失败、主动取消和未知错误默认不可重连。本地文件和关闭阶段
不自动重连，避免重开文件截断已有内容或重新创建已结束的输出任务。

### 8.7 ZLMediaKit 配置示例与验证

RTSP 推荐先验证 TCP：

```cpp
PusherConfig config;
config.output_url = "rtsp://127.0.0.1:554/live/camera";
config.video_track = encoded_track; // H.264, time base, dimensions and SPS/PPS
config.ffmpeg.rtsp = RtspOutputOptions{}; // TCP
config.io.connect_timeout = std::chrono::milliseconds{5000};
config.io.write_timeout = std::chrono::milliseconds{10000};
```

RTMP 使用另一份配置，不能携带上一份配置的 rtsp 选项：

```cpp
PusherConfig config;
config.output_url = "rtmp://127.0.0.1:1935/live/camera";
config.video_track = encoded_track;
config.ffmpeg.rtmp = RtmpOutputOptions{};
config.ffmpeg.rtmp->tcp_nodelay = true;
// output_format omitted: Pusher resolves RTMP to FLV.
```

地址和端口以实际 ZLMediaKit 部署为准，服务器需要开放对应端口并允许发布。
ZLMediaKit 是远端服务，不作为 Pusher 的新后端类型。
`Open()` 成功只证明初始化成功；验收还应检查服务端媒体注册、播放端解码、
首关键帧、连续时间戳和断开后的资源释放。

### 8.8 实施顺序与验收

1. 配置模型：增加 FFmpeg 分组、RTSP/RTMP 专用配置；迁移共享轨道类型；
   保留 output_url、video_track、io 的现有入口，默认 MP4 用法不变。
2. 解析与校验：实现可独立测试的格式解析、协议匹配、选项转换和冲突检查；
   覆盖 Windows 路径、大小写协议、无扩展名、显式格式、负超时及错误选项。
3. Muxer 接入：显式指定格式，分别传入 AVIO/muxer 字典；验证失败清理、
   未消费选项、超时和取消仍保留原始阶段。
4. Publisher 路线：保持 `Client/Server` 两种交互模型；Client 内按后端创建
   Pusher，不按 RTSP/RTMP/ZLMediaKit 增加 Publisher 分支。
5. 回归与联调：先通过本地 MP4、时间戳和 Session 测试，再分别验证 ZLMediaKit
   RTSP/TCP、RTMP 的单路 H.264 发布与播放；加入连接拒绝、认证拒绝、
   服务端断开以及可控本地测试服务制造的超时测试。
6. 后续扩展：网络推流基线通过后，再增加 Session 重连配置与退避状态机，
   然后扩展 SRT、TLS 专用参数、音频及多轨。

重连配置只加入 PusherSessionConfig，建议后续提供 enabled、
max_reconnect_attempts、initial_delay、max_delay 和 backoff_multiplier。
消费式写包失败后不重发原包；重连成功后等待新关键帧。
重连机制不能替代本阶段的格式、选项和媒体参数校验。

## 9. 边界收敛项（2026-09-19 追加）

本节固化网络推流基线之后的接口约束。以下工作不改变 PushClient 与
PullServer 的分界：`PublisherKind::Client` 继续表示主动向单一目标输出，
`PublisherKind::Server` 留给后续监听端口和服务多个播放客户端的独立路线。
ZLMediaKit 是 Client 的远端目标，不是新的 Publisher 类型。

### 9.1 时间轴策略归属 PusherSession

时间轴的起点和恢复策略属于一次输出会话，必须由 `PusherSession` 决定：

- Session 配置显式选择 `Preserve` 或 `StartAtZero`；
- Session 在首个被接纳的媒体包建立 epoch，并在重连成功后按配置重建；
- Session 不改写 Puller、Decoder 或 Encoder 的原始时间轴；
- Pusher 只将 Session 给出的 packet 时间戳交给输出后端；Muxer 只负责按
  输出流 time base 转换并写入。

现阶段本地文件的时间戳归零仍是过渡行为，已由 `FFmpegPusher` 根据输出类型
转为传给 Muxer 的显式执行参数。迁移时让 Session 生成该参数，再删除 Pusher
基于 URL 的本地文件判断。网络输出默认保留连续时间轴，避免重连或多个输出
目标各自猜测偏移量。

### 9.2 停止、事件与并发契约

在引入重连或异步队列前，先固定两类操作：

| 操作 | 契约 | 责任路径 |
|---|---|---|
| `RequestStop()` | 线程安全、非阻塞、可重复；只请求中断阻塞 I/O 或退避等待 | `Publisher -> PusherSession -> IPusher -> Muxer` |
| `Close()` | 在停止后等待当前 I/O 退出，写 trailer、释放资源并返回最终结果 | `Publisher -> PusherSession -> IPusher -> Muxer` |

`IPusher` 应将停止作为虚接口，而不是仅由 `FFmpegPusher` 暴露。Pusher 的异步
事件也必须使用结构化事件（至少包含错误、发生阶段和是否可重试），由
`PusherSession` 串行处理状态迁移和重连。不能让 Pusher 回调直接改 Publisher
状态，也不能用字符串回调让 Session 解析 FFmpeg 错误文本。

当前同步 `Publish()` 明确会把网络写入背压传给调用线程。以后若增加队列，队列
归 Session 所有，需在配置中定义容量、满队列策略、停止时丢弃规则和统计口径；
Publisher 只暴露结果和控制入口。

### 9.3 Packet 所有权与重试

当前 FFmpeg Muxer 使用消费式写入：一旦进入
`av_interleaved_write_frame()`，底层 `AVPacket` 不可由调用方重试或复用。
这个约束必须体现在接口中，不能只依赖 `const MediaPacket&` 注释表达。

- 保持消费式模型时，`Push/Write` 应接收可移动或可变的 packet，并在结果中
  明确标记是否已消费；
- 改为非消费式模型时，Pusher 或 Session 必须通过 `av_packet_ref()` 创建自己
  的引用，原 packet 才能用于重试或异步队列；
- 首版重连默认丢弃失败包，关闭旧 Pusher 后等待新的关键帧；不得隐式重发；
- 引入音频或多轨前，需单独定义关键帧门控期间音频的缓存/丢弃策略和各轨
  所有权，不能沿用单视频规则。

### 9.4 统计、健康状态与联调验收

统计按拥有信息的层采集，避免 Publisher 从日志或 FFmpeg 错误文本反推：

| 层 | 必须提供的统计/状态 |
|---|---|
| Muxer | 实际写入包数/字节数、写入与关闭失败的 native code 和操作阶段 |
| Pusher | 已映射错误分类、目标可达性、协议/format 及后端打开状态 |
| PusherSession | 等待关键帧丢弃数、实际发布数、重连次数/退避状态、当前会话状态 |
| Publisher | 对外汇总状态、最后错误、每个输出目标的 Session 统计 |

`RunFfmpegPullerDecoderConverterEncoderPushTest` 的成功条件还应包括：

1. `publisher.Close()` 成功，不能忽略 trailer 或连接关闭失败；
2. ZLMediaKit 已注册预期媒体流，并由独立播放端拉流并完成解码；
3. 首帧为可解码关键帧，随后 PTS/DTS 连续且单调；
4. 主动断开 ZLMediaKit、认证拒绝、连接超时和停止阻塞写入都得到预期状态；
5. 测试结束后确认服务端流和本地 FFmpeg 资源均已释放。

离线单测继续覆盖格式解析、option 映射、消费语义、状态机和重连退避；真实
ZLMediaKit 联调只验证网络兼容性，不能替代这些可重复的回归测试。
