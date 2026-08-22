# 媒体模块问题驱动学习路线

本文用于指导当前工程的学习顺序。

前一份设计文档从目标架构出发，内容比较完整，但不适合当前阶段逐步学习。当前改用问题驱动方式：每增加一个模块或功能，先说明它解决了什么实际问题，再实现它。

## 1. 当前目标

当前最重要的目标不是先完成全部架构，而是先跑通下面这条最小链路：

```text
RTSP 输入
    -> FFmpegPuller
    -> MediaPacket
    -> FFmpeg Decoder
    -> AVFrame
```

成功标准：

- 能打开固定 RTSP 地址；
- 能读取视频编码包；
- 能把编码包送入 FFmpeg 解码器；
- 能收到 `AVFrame`；
- 能打印帧的宽度、高度、像素格式和时间戳；
- 能主动停止并释放 FFmpeg 资源。

在这条链路没有跑通之前，不继续扩展 AVTP、MediaStreamSource、MediaFlow、jitter buffer 和复杂状态机。

## 2. 为什么需要 Session

### 2.1 先说结论

解码功能第一次跑通时，不一定需要完整的 `MediaStreamSession`。

可以先直接写一个测试程序：

```text
FFmpegPuller.Open()
    -> FFmpegPuller.ReadPacket()
    -> avcodec_send_packet()
    -> avcodec_receive_frame()
```

这样做的好处是学习目标单一：先确认“输入包确实能够被解码成帧”。

`MediaStreamSession` 的价值不在于解码。它解决的是下面这些运行时问题：

- 谁负责打开和关闭 Puller；
- 谁负责持续读取；
- 谁负责响应 Stop；
- 谁负责区分 EOS、暂时无数据、网络错误和致命错误；
- 谁负责重连和重连等待；
- 谁负责把包和状态通知给上层；
- 谁负责统计和观察连接是否正常。

因此，Session 是“媒体输入连接的生命周期管理器”，不是“解码器”。

### 2.2 没有 Session 时会发生什么

最初的测试程序可以把所有逻辑写在 `main()` 中：

```cpp
puller.Open(endpoint);

while (running) {
    PullReadResult result = puller.ReadPacket();
    if (result.status == PullReadStatus::Packet) {
        decoder.Send(result.packet);
        decoder.ReceiveFrames();
    }
}

puller.Close();
```

这个程序适合验证解码，但很快会遇到问题：

1. `main()` 同时处理输入、错误、停止和解码，职责开始混在一起；
2. `ReadPacket()` 可能阻塞，主线程无法及时响应停止；
3. 不同 Puller 的错误形式不同，上层必须理解协议细节；
4. 发生网络断开后，重连逻辑会和解码逻辑交织；
5. 以后增加第二个媒体源时，需要复制一套读取和重连代码；
6. 上层无法清楚知道当前是“正常结束”“主动停止”还是“连接失败”。

Session 就是为了解决这些重复出现的生命周期问题。

### 2.3 Session 与 Puller、Decoder 的边界

```text
Puller
    负责：如何从某种输入协议取得编码包

Session
    负责：这一路输入连接如何启动、读取、停止、重连和报告状态

Decoder
    负责：如何把编码包转换为解码帧
```

三者的依赖方向应该是：

```text
Session -> IPuller -> FFmpegPuller
上层    -> Session -> MediaPacket
Decoder 接收 MediaPacket，不反向控制 Puller
```

Decoder 不应该自己调用 `FFmpegPuller::Open()` 或自己实现重连。否则同一条媒体输入会出现两套生命周期控制。

## 3. Session 需要做什么

下面的“必要性”不是说所有功能都要现在实现，而是说明这个功能出现的原因和合适的学习阶段。

| Session 功能 | 它解决的实际问题 | 为什么由 Session 负责 | 当前阶段 |
|---|---|---|---|
| `Open` | 输入资源没有统一启动入口 | Session 管理一次逻辑输入会话的开始 | 最小功能 |
| `Close` | FFmpeg context、线程和网络资源可能泄漏 | Session 知道读取是否已经结束，可以安排安全关闭 | 最小功能 |
| 读循环 | 上层不应该手写无限读取循环 | Session 是唯一的编码包生产者 | 最小功能 |
| Stop | 阻塞读取时如何退出 | Session 统一设置停止状态并通知 Puller | 最小功能 |
| 读取结果分类 | 上层需要区分 Packet、EOS、NoData 和错误 | Puller 报告结果，Session 决定会话行为 | 最小功能 |
| StreamInfo 回调 | Decoder 需要知道编码、宽高和时间基 | Session 在连接成功后把输入元信息转交上层 | 最小功能 |
| Packet 回调 | Decoder 不应该依赖 Puller 的具体类型 | Session 将编码包交给解码器或其他消费者 | 最小功能 |
| 重连 | 网络输入断开后需要重新建立连接 | 重连是连接生命周期问题，不是协议 Puller 的业务策略 | 第二阶段 |
| 退避和重连预算 | 连接失败时不能高速循环重连 | Session 统一控制等待时间和尝试次数 | 第二阶段 |
| 稳定后重置预算 | 长时间稳定后，旧的失败次数不应永久影响新故障 | Session 能观察连接持续时间 | 第二阶段 |
| 统计 | 需要知道收到了多少包、多少字节、重连几次 | 这些是会话级统计，不属于单个协议读取动作 | 第二阶段 |
| Watchdog | 连接没有报错但长时间没有数据 | Session 能同时观察读取活动和会话状态 | 后续阶段 |
| generation | 异步重连后旧连接的迟到结果不能污染新连接 | Session 是连接代次的唯一拥有者 | 后续阶段 |
| Jitter Buffer | 网络抖动时需要按时间戳重新排序或延迟释放 | 这是会话级时序处理，不是 Puller 或 Decoder 的职责 | 后续阶段 |

## 4. 每项功能为什么不能放到别处

### 4.1 为什么 Open 和 Close 放在 Session

`FFmpegPuller` 当然会调用 `avformat_open_input()` 和 `avformat_close_input()`，但它只知道底层资源如何打开和释放。

Session 还需要知道：

- 什么时候允许启动读线程；
- 什么时候可以通知上层“已连接”；
- 关闭前是否还有读取线程；
- 关闭后是否还允许分发包；
- 重连时如何复用同一个逻辑会话。

这些信息超出了 Puller 的职责，所以由 Session 编排。

### 4.2 为什么读循环放在 Session

`ReadPacket()` 是一次读取动作，Session 的读循环是持续运行策略。

Puller 只回答：

```text
这一次读取拿到了什么？
```

Session 决定：

```text
下一步继续读、等待、发送 EOS、重连，还是结束会话？
```

如果每个上层模块自己写读循环，就会出现多个版本的 EOS、重连和 Stop 行为。

### 4.3 为什么 Stop 不能只由 Decoder 处理

Decoder 只能释放解码器资源，无法可靠地停止底层网络读取。

停止一条 RTSP 输入通常需要：

```text
停止继续读取
    -> 中断可能阻塞的 Puller
    -> 等待读取线程退出
    -> 关闭 FFmpeg 输入
    -> 停止向 Decoder 发送新包
```

这条顺序跨越 Puller、线程和 Decoder，所以应该由 Session 统一安排。

### 4.4 为什么错误分类放在 Session 处理

Puller 负责把 FFmpeg 原生错误转换成结构化结果，例如：

```cpp
PullReadStatus::EOS
PullReadStatus::RetryableError
PullReadStatus::FatalError
PullReadStatus::Stopped
```

Session 根据结果决定会话动作：

| 结果 | Session 行为 |
|---|---|
| `Packet` | 转交 Decoder 或上层回调 |
| `NoData` | 稍后继续读取，避免忙循环 |
| `EOS` | 正常结束，不默认重连 |
| `RetryableError` | 按策略尝试重连 |
| `FatalError` | 进入错误状态，停止读取 |
| `Stopped` | 结束读取，不作为输入故障 |

Decoder 不需要知道 `AVERROR_EOF`、socket 超时或 FFmpeg 网络错误码的含义。

### 4.5 为什么重连不能放在 FFmpegPuller

`FFmpegPuller` 可以重新调用 `Open()`，但“什么时候重连、重连几次、等待多久、是否已经稳定过”属于业务策略。

同一个 Session 未来可能使用：

- FFmpegPuller；
- AvtpPuller；
- 测试 Puller；
- 其他输入协议。

如果每种 Puller 自己实现重连，错误策略会按协议分裂。Session 负责通用的连接生命周期，Puller 只负责一次底层连接。

### 4.6 为什么 Decoder 不属于 Session

Session 只应保证编码包的生命周期和顺序。Decoder 还需要处理：

- `AVCodecContext`；
- 解码器选择；
- `send_packet/receive_frame`；
- B 帧导致的延迟输出；
- flush；
- 像素格式和帧内存；
- 硬件解码。

这些是解码领域的问题。把 Decoder 放进 Session 会让 Session 同时承担输入协议、连接状态和编解码，后续难以替换解码器或输出多个消费者。

## 5. 当前工程中的最小 Session

当前代码已经有一个基础版 `MediaStreamSession`，它包含：

- `Start()` 调用 Puller 的 `Open()`；
- 独立读取线程；
- `PacketCallback`；
- `StreamInfoCallback`；
- `StateCallback`；
- `Stop()` 和 `Close()`；
- `PullReadResult` 状态处理；
- 基础重连、退避和稳定重置；
- 包数、字节数和重连次数统计。

但当前工程还没有真正的 Decoder 模块。因此，下一步不是继续增加 Session 功能，而是验证编码包能否被解码。

## 6. 问题驱动的实现顺序

### 阶段 1：不用完整 Session，直接跑通解码

新增一个固定参数的测试程序，例如：

```text
test_media_ffmpeg_decode.cpp
```

程序只做这些事：

1. 创建 `FFmpegPuller`；
2. 使用固定 RTSP 地址调用 `Open()`；
3. 从 `GetStreamInfo()` 找到视频流；
4. 根据视频编码创建 `AVCodecContext`；
5. 循环调用 `ReadPacket()`；
6. 对 `Packet` 调用 `avcodec_send_packet()`；
7. 循环调用 `avcodec_receive_frame()`；
8. 打印收到的 `AVFrame` 信息；
9. 读到指定帧数后释放 Decoder 和 Puller。

这一阶段要回答的问题只有一个：

```text
FFmpegPuller 输出的 MediaPacket 是否能够正确进入 FFmpeg Decoder 并产生 AVFrame？
```

### 阶段 2：观察不用 Session 的问题

解码跑通后，再主动验证：

- Stop 时 `ReadPacket()` 是否阻塞；
- RTSP 断开后读取结果是什么；
- Decoder 收到 EOS 时如何 flush；
- Packet 回调和解码线程是否需要解耦；
- 如果同时运行两路输入，代码是否开始重复。

这些问题出现后，再把“持续读取、状态处理和停止”移动到 Session。

### 阶段 3：让 Session 只负责输入编排

此时结构变为：

```text
FFmpegPuller -> MediaStreamSession -> Decoder
```

Session 负责：

- Open；
- 读循环；
- Stop；
- 读取结果分类；
- Packet/StreamInfo 回调。

Decoder 负责：

- 创建和销毁 `AVCodecContext`；
- 接收 `MediaPacket`；
- 输出 `AVFrame`；
- 处理 flush 和解码错误。

### 阶段 4：只在问题出现后加入重连

当真实 RTSP 流出现断开时，再验证：

```text
RetryableError
    -> KRECONNECTING
    -> 等待
    -> Open()
    -> KCONNECTED
```

这时再学习重连预算、退避、稳定重置和 StreamInfo 重新发布。

### 阶段 5：再考虑高级能力

只有在基本输入和解码稳定后，才进入：

- watchdog；
- generation；
- jitter buffer；
- MediaStreamSource；
- MediaFlow；
- AVTP；
- 音频和多轨同步。

## 7. 当前不需要 Session 做的事情

为了避免职责继续膨胀，当前 Session 不做：

- 视频解码；
- 音频解码；
- 帧格式转换；
- 编码；
- 渲染；
- Decoder 的线程安全管理；
- MediaFlow 消息构造；
- 多订阅者管理；
- AVTP 协议解析；
- 业务级源 ID 管理。

## 8. 每次实现前先问三个问题

以后新增 Session 功能前，先回答：

1. 现在遇到的具体问题是什么？
2. 这个问题为什么属于 Session，而不是 Puller 或 Decoder？
3. 有没有一个测试能在实现前失败、实现后通过？

例如：

| 问题 | 应该修改谁 |
|---|---|
| `avcodec_receive_frame()` 收不到帧 | Decoder 或输入包格式 |
| RTSP URL 打不开 | FFmpegPuller 或配置 |
| Stop 后线程无法退出 | Session 与 Puller 的停止契约 |
| 网络断开后需要重试 | Session |
| H.264 帧需要转 BGR | Decoder 后的转换模块 |
| 多个订阅者收到同一个包 | Source adapter |

## 9. 当前下一步

当前下一步明确为：

```text
编写固定参数的 FFmpeg 解码测试
    -> FFmpegPuller.Open()
    -> ReadPacket()
    -> avcodec_send_packet()
    -> avcodec_receive_frame()
    -> 打印 AVFrame
```

暂时不继续修改 Session。等解码测试暴露出明确的生命周期或线程问题后，再回到 Session 针对问题增加功能。
