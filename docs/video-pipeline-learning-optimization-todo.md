# video-pipeline 学习与优化 TODO

目标工程：`E:\share\project\video-pipeline`

这份 TODO 服务于两个目标：

1. 学会原工程中一帧媒体数据如何经过拉流、解码、推理、编码和推流。
2. 在真实问题和测量结果的基础上优化，而不是先实现一套脱离现状的完整架构。

## 总原则

当前阶段采用“问题驱动、最小闭环、逐步替换”的方式。

- 先让一条可观察、可重复的链路跑通。
- 每次只引入一个新变量，并保留上一步可运行的版本。
- 先使用原工程已有组件，暂不重写 `IPuller`、`MediaStreamSession`、MediaFlow 或配置系统。
- 先解决能证明的问题，再抽象出通用设计。
- 所有优化都要有指标、复现步骤和回归测试。

不要把以下内容放入第一版 Demo：多路输入、主备切换、复杂重连策略、动态配置、完整 MediaFlow 重构、跨协议统一配置、硬件加速和生产级监控。

## 原工程迁移地图

新工程当前适合作为“学习和重建实验场”，原工程适合作为“行为和实现参考”。两者不要一次性合并。每迁移一块，先在新工程中用一个最小例子证明它能工作，再决定是否保留原工程的抽象。

### 代码应该迁移到哪里

| 原工程 | 新工程建议位置 | 迁移时机 | 说明 |
|---|---|---|---|
| `include/media/media_packet.h`、`media_frame.h`、buffer 类 | `src-cpp/include/media/` | 阶段 1 前 | 这是所有组件共享的数据基础，先理解再迁移。重点确认 buffer 所有权、媒体类型和时间戳。 |
| `include/media/puller/`、`src/media/puller/` | `src-cpp/include/media/puller/`、`src-cpp/src/media/puller/` | 阶段 1 | 先只迁移 `FFmpegPuller` 需要的最小能力。AVTP 和复杂协议配置暂缓。 |
| `include/media/decoder/`、`src/media/decoder/` | `src-cpp/include/media/decoder/`、`src-cpp/src/media/decoder/` | 阶段 1 | 先支持视频解码；音频等第二条轨道在视频闭环稳定后再加入。 |
| `include/media/encoder/`、`src/media/encoder/` | `src-cpp/include/media/encoder/`、`src-cpp/src/media/encoder/` | 阶段 2 | 先迁移 `IEncoder`、`FFmpegEncoder` 和必要配置，先输出本地文件。 |
| `include/media/publisher/`、`src/media/publisher/` | `src-cpp/include/media/publisher/`、`src-cpp/src/media/publisher/` | 阶段 5 | 先使用一种已验证的发布方式；不要一开始同时迁移 RTSP Server、RTP、WebRTC。 |
| `include/inference/`、`src/inference/` | `src-cpp/include/inference/`、`src-cpp/src/inference/` | 阶段 3 | 先迁移同步推理所需的 Session、Model 和 TensorFrame；异步调度器后置。 |
| `include/filter/osd/`、`src/filter/osd/` | `src-cpp/include/filter/osd/`、`src-cpp/src/filter/osd/` | 阶段 4 | 只有在推理旁路稳定后，才迁移 OSD，将结果绘制到帧上。 |
| `include/mediaflow/`、`src/mediaflow/` | `src-cpp/include/mediaflow/`、`src-cpp/src/mediaflow/` | 阶段 7 之后 | MediaFlow 是调度和生命周期适配层，不是理解媒体链路的起点。先用手工调用理解组件，再接节点。 |
| `include/media/recorder/`、`src/media/recorder/` | `src-cpp/include/media/recorder/`、`src-cpp/src/media/recorder/` | 后续功能 | 录像依赖稳定的时间戳、编码包和停止契约，不能作为第一条验证链路。 |
| `include/render/`、`src/render/` | `src-cpp/include/render/`、`src-cpp/src/render/` | 后续功能 | 渲染是观察输出的一种方式，不是拉流到推流闭环的必要条件。 |
| `src/c_api/`、Tauri 和 `apps/player/` | 新工程对应应用层目录 | 最后 | 这些属于产品接入层，等底层媒体能力和接口稳定后再迁移。 |

### 架构设计文档应该怎么用

原工程的设计文档不要当作“待实现清单”逐条照抄。先把它们当作问题背景和决策记录，在对应阶段阅读：

| 原工程文档 | 什么时候阅读 | 用来回答什么问题 |
|---|---|---|
| `docs/media-input-puller-session-source-design.md` | 阶段 1 和阶段 7 | Puller、Session、Source 的职责边界；哪些配置属于 I/O，哪些属于重连和会话。 |
| `docs/mediaflow-media-integration-guide.md` | 阶段 1 了解现状，阶段 7 实施适配 | 为什么不能直接把四个组件包装成节点；时间基、轨道身份、停止屏障和背压有哪些风险。 |
| `docs/media-flow-design.md` | 阶段 7 之后 | Graph、Node、Edge 和 Executor 的整体设计，避免在还没理解单线程闭环时过早引入。 |
| `docs/inference-module-design.md` | 阶段 3 | 推理 Session、模型、预处理、后处理和异步调度的边界。 |
| `docs/osd-module-guide.md`、`docs/osd-module-porting-plan.md` | 阶段 4 | 检测结果如何进入帧处理和绘制，不要把 OSD 逻辑塞进 Decoder 或 Encoder。 |
| `docs/publisher-protocol-design.md` | 阶段 5 | 推流协议、RTSP 传输、发布端配置和协议层边界。 |
| `docs/recording-module-evaluation-and-technical-plan.md` | 录像需求出现时 | 录像文件、分片、索引和存储策略。 |
| `docs/avtp-module-integration-evaluation.md` | 需要接入 AVTP 时 | AVTP 采集、组帧、时间戳映射和设备依赖。 |
| `docs/rtsp-zlm-pipeline-troubleshooting.md` | RTSP 联调失败时 | ZLMediaKit、TCP/UDP、SDP 和网络环境排查。 |

### 推荐迁移顺序

```text
媒体数据类型和 buffer
        -> FFmpegPuller
        -> FFmpegDecoder
        -> FFmpegEncoder
        -> 本地输出验证
        -> InferenceSession
        -> OSD
        -> Publisher
        -> RTSP 网络联调
        -> MediaFlow 适配
        -> Recorder / Render / Tauri / Web UI
```

这个顺序的关键点是：`Publisher` 可以在编码闭环之后接入，`Inference` 可以在解码闭环之后接入，但 `MediaFlow` 适配应该放到手工链路已经能解释之后。否则出现问题时，很难判断错误来自媒体组件还是 Graph 调度。

### 每次迁移一个模块时的检查清单

- [ ] 先找到原工程中该模块的最小公共接口。
- [ ] 只复制该接口直接需要的类型和实现，不复制无关目录。
- [ ] 检查原工程实现依赖的第三方库、编译宏和运行时 DLL。
- [ ] 在新工程写一个最小调用或测试，证明模块独立可用。
- [ ] 检查 `Open`、`Flush`、`Close`、停止和重复调用行为。
- [ ] 检查时间戳、轨道 ID、buffer 所有权和线程要求。
- [ ] 将实际迁移中发现的问题记录到“问题模板”中。
- [ ] 只有测试通过后，才把模块接入下一层。

### 目前不应该迁移的内容

- 完整的 `MediaInputConfig`、Factory 和所有协议 option：当前还没有真实配置问题需要解决。
- 完整 MediaFlow Graph：当前还没有证明手工链路的线程和生命周期契约。
- AVTP、SRT、HLS、WebRTC 等非首条路径协议：先用本地文件和 RTSP 建立基线。
- 录像、渲染、Tauri 和 UI：它们会增加观察方式和产品边界，但不会帮助理解第一条媒体闭环。
- 所有“未来可能用到”的抽象接口：等第二个真实调用方出现后再抽象。

## 目标闭环

第一阶段的最终链路如下：

```text
本地文件
  -> FFmpegPuller
  -> MediaPacket
  -> FFmpegDecoder
  -> MediaFrame
  -> InferenceSession
  -> FFmpegEncoder
  -> MediaPacket
  -> 本地文件或 FFmpeg Publisher
```

网络版再替换输入和输出：

```text
RTSP 输入 -> Puller -> Decoder -> Inference -> Encoder -> RTSP 推流
```

推理第一版只需要能够稳定接收 `MediaFrame`、输出检测结果并记录耗时。编码可以先继续编码原始帧；确认推理链路稳定后，再增加 OSD，把检测结果绘制回画面。

## 阶段 0：建立原工程基线

目标：知道原工程当前能构建什么、已有 Demo 能证明什么、哪些能力只是接口存在。

- [ ] 在 `E:\share\project\video-pipeline` 配置一个最小构建 profile。
- [ ] 构建 `video-pipeline` CLI 和与媒体相关的测试。
- [ ] 运行已有的 `test_stream_decode`，确认 RTSP 拉流和解码的现状。
- [ ] 阅读 `src/main.cpp`，确认 CLI 当前只是占位入口还是已经接入业务。
- [ ] 阅读以下接口和实现，不修改代码：
  - `include/media/puller/i_puller.h`
  - `include/media/puller/ffmpeg_puller.h`
  - `src/media/puller/ffmpeg_puller.cpp`
  - `include/media/decoder/i_decoder.h`
  - `include/media/decoder/ffmpeg_decoder.h`
  - `include/media/encoder/i_encoder.h`
  - `include/media/encoder/ffmpeg_encoder.h`
  - `include/media/publisher/i_publisher.h`
  - `include/media/publisher/publisher_config.h`
- [ ] 记录每个组件的输入、输出、生命周期和线程归属。

完成标准：

- 能说明 `MediaPacket` 和 `MediaFrame` 的区别。
- 能说明 `Open/Decode/Encode/Flush/Close` 的调用顺序。
- 能运行至少一个已有媒体测试，并保存命令和结果。

## 阶段 1：先跑通“拉流/读包 -> 解码”

目标：不加入推理、编码和推流，先观察压缩包如何变成解码帧。

输入优先使用仓库中的 `test.mp4`，因为本地文件比 RTSP 更容易重复验证。

- [ ] 先复用已有 `FFmpegPuller`，打开 `test.mp4`。
- [ ] 获取 `MultiStreamInfo`，只选择一个视频轨道。
- [ ] 创建 `FFmpegDecoder`，用选中的 `MediaStreamInfo` 调用 `Open()`。
- [ ] 逐包调用 `ReadPacket()` 或 `ReadPacketResult()`。
- [ ] 将视频包送入 `Decode()`。
- [ ] 在帧回调中记录：帧序号、宽高、像素格式、PTS、耗时。
- [ ] 文件结束时按顺序调用 `Flush()` 和 `Close()`。
- [ ] 明确音频暂时跳过，不要同时调试音视频两条轨道。

建议新增一个非常小的学习程序或测试，而不是立即把所有逻辑塞进 MediaFlow。程序只负责串联已有组件和打印结果。

完成标准：

- 能稳定输出一定数量的视频帧。
- 输出帧的宽高、像素格式和时间戳合理。
- 能区分“读到包”“文件结束”“解码失败”。
- 重复运行两次，帧数和耗时大致可比较。

此阶段遇到的问题优先记录，不要顺手重构。例如：时间基不一致、`Flush()` 行为不清楚、包所有权不清楚，都先形成问题记录。

## 阶段 2：加入编码，先输出本地文件

目标：验证 `MediaFrame -> FFmpegEncoder -> MediaPacket`，暂时绕开网络发布。

- [ ] 根据解码帧的元数据创建 `EncoderConfig`。
- [ ] 先使用简单、稳定的 H.264 软件编码器配置。
- [ ] 明确编码器输入像素格式、宽高、帧率、码率和 time base。
- [ ] 每收到一帧调用 `Encode()`，处理可能产生的一个或多个 packet。
- [ ] 文件结束时调用 `Flush()`，不能只调用 `Close()`。
- [ ] 将编码 packet 写入一个简单的本地输出格式，或接入现有 FFmpeg mux 逻辑。
- [ ] 用 FFmpeg/ffprobe 检查输出文件能够播放，确认轨道、编码格式和时间戳。

完成标准：

- 输入文件可以解码并重新编码。
- 输出文件可播放，时长和帧率基本正确。
- 已记录编码延迟、输出码率和丢帧情况。
- 明确编码器输出 packet 的 `time_base`，不依赖“默认都是微秒”的假设。

## 阶段 3：加入推理，但先不改变媒体输出

目标：验证推理模块能消费 `MediaFrame`，并把推理耗时和结果与媒体时间戳关联起来。

第一版建议采用旁路方式：

```text
MediaFrame -> InferenceSession -> 记录结果
          \-> Encoder
```

这样可以先验证推理，不把“检测结果绘制”和“编码输出异常”混在一起。

- [ ] 阅读 `include/inference/session/session.h`、`session_factory.h`、`model/i_model.h`。
- [ ] 确认模型文件、设备、输入尺寸和标签配置的最小要求。
- [ ] 选择工程已有且容易验证的模型配置。
- [ ] 使用同步 `Infer()` 跑通单帧推理。
- [ ] 记录推理状态、检测数量、预处理耗时、执行耗时和后处理耗时。
- [ ] 使用固定间隔采样，例如每 5 帧推理一次，先观察吞吐和延迟。
- [ ] 再验证每帧推理，确认是否产生积压。
- [ ] 只有旁路推理稳定后，才考虑 `Submit()` 或异步调度。

完成标准：

- 模型加载成功，推理结果可解释。
- 推理失败不会破坏原始解码和编码链路。
- 能回答“推理耗时是否超过帧间隔”以及“是否需要丢帧/限流”。

注意：如果要让推流画面显示框，后续需要增加 OSD 或帧变换步骤。推理结果本身不会自动修改 `MediaFrame`。

## 阶段 4：把推理结果绘制回画面

目标：验证完整的视频处理闭环，而不是只验证推理旁路。

- [ ] 明确 OSD 输入是检测框结果还是通用 `FrameResult`。
- [ ] 选择一个明确支持的像素格式进行绘制，优先使用现有 OSD 模块。
- [ ] 在绘制前后记录帧的时间戳和尺寸，确认没有改变媒体时序。
- [ ] 验证 OSD 不修改共享只读 buffer，必要时创建输出帧。
- [ ] 对比“无 OSD 编码”和“有 OSD 编码”的 CPU、延迟和码率。

完成标准：

- 输出文件或预览画面能看到检测框。
- OSD 开关关闭时，链路行为与阶段 3 一致。
- OSD 失败时有明确处理策略，不让错误静默吞掉。

## 阶段 5：接入 RTSP 输入和 RTSP 推流

目标：把已经验证过的本地处理链路搬到网络环境。

先只替换一端，减少变量：

1. 本地文件输入 -> 编码 -> RTSP 推流。
2. RTSP 输入 -> 解码 -> 编码 -> 本地文件。
3. RTSP 输入 -> 解码 -> 推理/OSD -> 编码 -> RTSP 推流。

- [ ] 明确 RTSP 输入 URL、账号密码、TCP/UDP 传输方式。
- [ ] 明确 RTSP 输出端是 FFmpeg mux push 还是工程内 RTSP server。
- [ ] 先固定 TCP，验证通过后再测试 UDP。
- [ ] 记录连接耗时、首包时间、首帧时间、重连次数和端到端延迟。
- [ ] 测试服务端断开、输入结束和主动停止三种情况。
- [ ] 确认密码不会出现在日志、命令回显或错误字符串中。

完成标准：

- 能使用播放器验证 RTSP 输出。
- 输入断开后行为可解释，停止后不会继续发送旧帧。
- 本地文件版和 RTSP 版共享同一套 Decoder、Inference、Encoder 逻辑。

## 阶段 6：建立最小回归测试

目标：让 Demo 从一次性实验变成可持续修改的学习基线。

- [ ] 为每个阶段保留一个可运行命令。
- [ ] 固定一个短测试文件，控制测试时长和输入帧数。
- [ ] 增加最小计数断言：读包数、解码帧数、编码包数、推理成功数。
- [ ] 增加生命周期测试：正常结束、Decode 失败、Encode 失败、主动停止。
- [ ] 增加时间戳检查：单调性、time base 有效性、duration 是否合理。
- [ ] 将网络 RTSP 测试标记为集成测试，不让它阻塞纯本地单元测试。

完成标准：

- 修改一个组件后，可以快速知道闭环是否退化。
- 测试失败能够指出是在 Puller、Decoder、Inference、Encoder 还是 Publisher。

## 阶段 7：从问题出发做第一次优化

只有前面闭环稳定后，才开始处理架构和性能问题。每个问题按以下模板记录：

```text
问题：
复现条件：
现象：
期望：
相关组件：
测量数据：
最小修复：
回归测试：
是否需要架构调整：
```

建议优先级：

- [ ] P0：数据正确性，例如时间基、PTS、轨道身份、内存所有权。
- [ ] P0：生命周期，例如 Flush、Stop、Close、重复 Start/Stop。
- [ ] P1：可观察性，例如阶段耗时、队列长度、丢帧、重连和错误分类。
- [ ] P1：背压和限流，例如推理速度低于输入帧率时的处理策略。
- [ ] P1：网络可靠性，例如超时、重连、RTSP TCP/UDP 配置作用域。
- [ ] P2：配置整理，例如将实际使用的字段结构化，删除未生效字段。
- [ ] P2：MediaFlow 适配，例如让 Node 复用已有 Session，而不是复制读循环。
- [ ] P3：多路输入、线程池、插件化和更复杂的 Factory。

## 什么时候开始做架构

出现以下信号时，才值得抽象：

- 同一段 Puller/Decoder/Encoder 生命周期代码已经在两个 Demo 中重复。
- 某个问题需要同时修改多个调用方才能修复。
- 已经明确组件之间的输入输出契约，且契约有测试保护。
- 测量显示线程、队列或调度确实是瓶颈。
- 新需求明确要求多路、重连、动态配置或 MediaFlow 接入。

届时再按问题拆分架构任务：

- [ ] 统一 `PullReadResult` 和错误分类。
- [ ] 解耦 Puller 配置与 Session 配置。
- [ ] 统一 `MediaPacket`、`MediaFrame` 的 time base 约定。
- [ ] 给 Decoder、Encoder、Publisher 补齐可测试的 Flush/Stop 契约。
- [ ] 用一个 Session 复用订阅式 Source 和 MediaFlow SourceNode。
- [ ] 最后再引入配置聚合根、Factory 和协议专用 option builder。

## 推荐的第一次实际操作

先不要继续扩展 `puller_config.h`。在原工程中按以下顺序做第一次练习：

1. 构建并运行已有的 `test_stream_decode`。
2. 阅读它如何创建 `FFmpegPuller`、`StreamSourceNode` 和 `FFmpegDecoder`。
3. 写一个仅针对 `test.mp4` 的小程序：Puller 读包，Decoder 输出帧并计数。
4. 给这个小程序加 Encoder，输出一个新的本地文件。
5. 最后再接推理和 RTSP 推流。

每完成一步，就把实际命令、输出和遇到的问题补回本文件。这样这份 TODO 同时也是你的学习日志和回归基线。
