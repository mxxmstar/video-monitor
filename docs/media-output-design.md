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
PublisherKind::FFmpegFile
```

当前枚举名 `FFmpegFile` 只是第一版以 MP4 文件为目标的历史命名。等
FFmpegPusher 验证网络输出后，如果同一个实现需要同时表示文件和网络目标，
可以将其改名为更准确的 `FFmpegOutput`；这只是配置命名调整，不意味着要
增加多个协议 Pusher。

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
