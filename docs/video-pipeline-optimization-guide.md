# 拉流-解码-转换-编码视频链路优化指导

本文针对当前 `src-cpp` 中的 FFmpeg 视频链路：

```text
FFmpegPuller
  -> MediaPacket / FFmpegPacketBuffer
  -> FFmpegDecoder
  -> MediaFrame
  -> MediaFrameConverter（可选缩放、像素格式转换）
  -> FFmpegEncoder
  -> MediaPacket / FFmpegPacketBuffer
  -> FFmpegMuxer
```

目标不是立刻堆叠硬件编解码和线程池，而是在不破坏时间戳、媒体质量、停止语义和断线重连的前提下，降低端到端时延、CPU 占用和内存带宽。

本文依据当前仓库代码编写。它描述的是“代码实际生效的行为”，不是设计草案中的目标状态。

## 1. 结论和优先级

当前首要问题按“媒体模块是否需要现在修改”排列如下。测试程序中的问题单独列为测试改进项，不作为当前 `media` 模块的 P0。

| 优先级 | 问题 | 当前证据 | 风险 | 建议动作 |
| --- | --- | --- | --- | --- |
| 测试项 | 编码时间基和输入实际帧率可能不匹配，PTS/DTS 可重复 | 当前完整链路样例固定使用 `25 fps`，而真实 RTSP 输入可能是约 `30 fps` | 样例 mux 失败或输出时间轴异常 | 当前仅作为测试程序改进项；媒体模块暂不因该样例参数修改 |
| 基线项 | `RAW_FRAME_BUFFER=0` 路径包含额外帧复制 | 该宏当前用于和 `RAW_FRAME_BUFFER=1` 对比零拷贝收益 | 只影响基线性能，不代表当前媒体模块功能错误 | 保留 `0` 做对照；切换 `1` 前完成 raw AVFrame 契约和回归测试 |
| P1 | 拉流线程同步执行下游回调，没有背压边界 | Session 读线程直接调用 `packet_callback`；测试回调内完成转换、编码、复用 | 慢编码或慢网络会阻塞拉流，时延无限累积 | 按阶段增加有界队列和明确的实时丢帧策略 |
| P1 | 编码器实际像素格式与配置像素格式混用 | 编码器可回退到 NV12，但转换目标仍使用配置格式 | 特定软件/硬件编码器下 `avcodec_send_frame` 失败或隐性颜色异常 | 区分输入格式、规范化目标格式和实际编码格式 |
| P1 | 码率控制配置不完整，低时延目标无法验证 | `max_bitrate`、`rc_buffer_size` 已定义但未写入 `AVCodecContext` | 实际码率、VBV 缓冲和延迟不受配置约束 | 增加显式 rate-control 模式和生效配置检查 |
| P1 | 可观测性不足，现有集成测试不能可靠作为性能基线 | RTSP 地址、时长和编码参数硬编码；mux 写失败仅打印日志 | 问题无法定位到具体阶段，回归会被掩盖 | 分离本地回归、网络集成和性能基准；补齐阶段指标 |
| P2 | decoder 固定单线程，尚未评估软硬件编解码选择 | `codec_ctx_->thread_count = 1` | 高分辨率多路场景吞吐不足 | 先测量，再按部署平台增加线程配置和硬件路径 |

当前媒体模块的推荐实施顺序是：先完成 raw AVFrame 契约和 packed/raw 回归，再消除不必要的像素复制，然后实施队列背压与独立 mux 线程，最后评估硬件编解码。测试程序的时间戳参数另行整理，不把测试专用配置当成模块缺陷。

## 2. 当前实际数据路径

### 2.1 拉流和压缩包

`FFmpegPuller::ReadPacket()` 读取 `AVPacket`，用 `av_packet_move_ref()` 将有效载荷移交到 `FFmpegPacketBuffer`。这一段不复制压缩码流，是当前链路中正确的零拷贝边界。

`MediaPacket::pts/dts/duration` 的单位是 `MediaPacket::time_base` 的 tick，不能假定为微秒。输入流时间基来自 `AVStream::time_base`。这套约定在 `media_packet.h` 中已经写明，应继续保持。

注意两个小问题：

1. 每个交付包仍会新建一个 `AVPacket` 描述符、`FFmpegPacketBuffer` 和 `MediaPacket`。这是元数据分配，不是码流 payload 复制，优先级低于像素帧复制。
2. `LatencyMode::Low` 会注入 `fflags=nobuffer`。该选项应只通过经过验证的实时 profile 启用；它会降低缓存时延，也会降低抗抖动和乱序容忍度。

### 2.2 解码、转换、编码之间的像素复制

当前 `ffmpeg_raw_frame_buffer.h` 中 `RAW_FRAME_BUFFER` 为 `0`。这是为了建立“有额外复制”和“raw AVFrame”两条基线，不是功能错误。此时编译的是 packed 分支，实际路径为：

```text
decoder receive AVFrame
  -> FFmpegFrameBuffer: AVFrame 平面数据打包到 malloc 连续内存     [复制 A]
  -> MediaFrame 仍保留原始 AVFrame 后端句柄
  -> MediaFrameConverter: av_frame_ref 输入，sws_scale 产生目标帧    [必要转换]
  -> AvFrameToMediaFrame: 目标 AVFrame 打包到 SimpleBuffer           [复制 B]
  -> Encoder: MediaFrameToAVFrame 分配新帧并 av_image_copy           [复制 C]
  -> avcodec_send_frame
```

对于确实发生缩放或像素格式转换的帧，`sws_scale` 的读写不可避免；但 A、B、C 都是额外的整帧内存操作。以 1920x1080 I420 为例，单帧约 3 MiB，60 fps 下仅一次额外读写就会快速占满内存带宽。

仓库已经具备 `FFmpegRawFrameBuffer`、`AdoptAVFrame()` 和 `av_frame_ref()` 快速路径，但默认宏没有启用它们。切换到 `RAW_FRAME_BUFFER=1` 时，目标是消除 A、B、C 三类额外复制；如果实际发生 `sws_scale`，转换本身仍然会产生一次必要的目标帧写入。现有 `docs/converter.md` 中关于“默认使用 raw 零拷贝”的表述应同步改成“由宏选择的实验路径”，不能把它当作当前默认性能结论。

### 2.3 对两个问题的范围修正

1. 完整链路测试中固定 `25 fps` 的问题属于测试程序配置。当前流程只是验证组件能否串起来，因此不要求现在修改 `FFmpegEncoder` 的时间轴策略；只有测试要代表真实输入，或媒体模块要支持生产级 CFR/VFR 时，才单独实施时间轴改造。
2. `RAW_FRAME_BUFFER=0` 属于有意保留的性能对照组。媒体模块当前要做的是保证 `RAW_FRAME_BUFFER=1` 切换后行为正确、所有权清晰、所有消费者不再误用连续 buffer，而不是现在删除 `0` 分支。

### 2.4 线程模型

`MediaStreamSession::Start()` 创建一个 `read_thread_`。`readLoop()` 每读到一个包后，在同一线程直接执行 `packet_callback`。如果回调内同步做解码、转换、编码和 mux，那么最慢阶段决定读取速度：

```text
read thread
  -> av_read_frame
  -> packet callback
      -> decode callback
          -> convert
          -> encode
          -> mux/write
  -> 下一次 av_read_frame
```

这在单路教学链路中便于理解，但实时系统没有隔离点：下游慢时会停止读取，上游协议缓存和相机端缓存继续增长，最终显示的是更旧的画面。

`MediaStreamSession` 的头文件仍描述“由 `io_context` 调度、watchdog、jitter buffer”，但当前实现实际持有 `std::thread`，也没有使用 `io_`、watchdog 或 jitter 配置。不要基于这些尚未落地的设计字段估算时延或容错能力。

## 3. 媒体模块 P0：先固定 raw AVFrame 契约

这里的“P0”是指切换 `RAW_FRAME_BUFFER=1` 之前必须具备的模块契约，不表示当前 `RAW_FRAME_BUFFER=0` 路径已经存在功能故障。

### 3.1 raw AVFrame 契约是什么

`FFmpegRawFrameBuffer` 不是一个“也可以调用 `Data()/Size()` 的普通 buffer”，而是一个由媒体模块管理生命周期、向 FFmpeg 消费者提供多平面 `AVFrame` 的帧存储对象。建议把下面的规则作为正式契约：

#### 所有权规则

```text
MediaFrame::buffer
  -> 唯一拥有 FFmpegRawFrameBuffer
      -> 唯一拥有 AVFrame
          -> AVFrame::data[] / extended_data[] 的 AVBufferRef

MediaFrame::backend.ptr
  -> 当前实现中的借用指针，仅在 buffer 存活期间有效
  -> 不负责释放，不得单独 av_frame_free()
```

- `AVFrame` 的释放只能由 `FFmpegRawFrameBuffer` 完成。
- decoder 将 `avcodec_receive_frame()` 得到的帧交给 raw buffer 后，不得再次使用或复用该帧描述符。
- 下游如果要保留帧，保留 `shared_ptr<MediaFrame>`；如果只需要 FFmpeg 帧引用，创建自己的 `AVFrame` 并调用 `av_frame_ref()`。
- 不允许缓存 `backend.ptr`、`AVFrame::data[]` 或 `PlaneData()` 返回的指针而脱离 `MediaFrame::buffer` 生命周期。

#### 数据访问规则

- raw 视频帧的真实数据只能通过 `GetFrame()`、`PlaneData(plane)` 和 `VideoFrameMeta::plane_info` 访问。
- `FFmpegRawFrameBuffer::Data()` 返回 `nullptr`、`Size()` 返回 `0` 是合法语义，不表示空帧。
- 任何需要连续数据的消费者必须显式执行一次 pack/copy，或拒绝 raw buffer；不能默认调用 `Data() + offset`。
- 当前 raw 契约只覆盖 CPU 可访问的普通视频像素格式。硬件帧、GPU handle 和 `AV_PIX_FMT_*` hardware format 需要单独的 `FFmpegHwFrameBuffer` 契约。

#### 元数据一致性规则

以下字段必须对应同一个 `AVFrame`，不能由调用方分别拼出：

```text
MediaFrame.type                 == VIDEO
VideoFrameMeta.width/height     == AVFrame.width/height
VideoFrameMeta.pixel_format     == FromAVPixelFormat(AVFrame.format)
VideoFrameMeta.plane_count      == av_pix_fmt_count_planes(AVFrame.format)
plane_info[i].stride            == AVFrame.linesize[i]
plane_info[i].size              == 该平面可读的有效字节数
backend.ptr                     == FFmpegRawFrameBuffer::GetFrame()
```

raw 模式下 `plane_info[i].offset` 没有连续 buffer 语义，建议固定为 `0`，并在注释中明确“不得用于地址计算”。平面地址只能来自 `AVFrame` 或 `PlaneData()`。

#### 并发规则

- `AVCodecContext`、`SwsContext` 和同一个 `AVFrame` 描述符由单一线程操作。
- raw 帧发布到队列后视为只读；不要在一个线程调用 `av_frame_make_writable()` 或修改 `data/linesize/format`，另一个线程同时读取。
- 跨线程传递时传 `shared_ptr<MediaFrame>`；需要独立 FFmpeg 生命周期时，在生产者线程或消费者线程创建独立 `AVFrame` 并 `av_frame_ref()`。
- `av_frame_ref()` 共享的是底层引用计数数据，不等于允许并发修改同一个 `AVFrame` 结构。

### 3.2 如何在现有 media 模块中落地

按最小改动顺序实施：

1. **先固化 `FFmpegRawFrameBuffer` 接口**：保留 `GetFrame()`、`PlaneData()`、`PlaneCount()`，补充 `IsValid()` 或构造时校验；明确 `Data()/Size()` 的 raw 语义。构造失败时必须保证传入 `AVFrame` 不泄漏，建议使用带 `av_frame_free` deleter 的 RAII 临时所有者完成转移。
2. **在 decoder 建立唯一转移点**：`receiveFrames()` 从 `avcodec_receive_frame()` 获得帧后，立即交给 raw buffer；成功转移后只通过 buffer 访问。下一次 `avcodec_receive_frame()` 使用新的描述符，不能覆盖已发布帧。
3. **在 converter 统一引用入口**：`MediaFrameToAVFrame()` 对 FFmpeg raw frame 只做 `av_frame_ref()`，不访问 `Data()/Size()`；目标帧在 `sws_scale` 后用 `AdoptAVFrame()` 交给新的 raw buffer。输入输出的时间戳和元数据在引用/接管时一并复制。
4. **让 encoder 消费实际 AVFrame**：encoder 不能根据 `MediaFrame.buffer->Size()` 判断 raw 帧有效性；应验证 `VideoMeta` 和 AVFrame 一致，然后引用到自己的输入 `AVFrame`。编码器只允许读取/消费自己的引用，不修改上游 raw 帧。
5. **隔离连续 buffer 消费者**：对 muxer 之外的模块逐个搜索 `Data()`、`Size()` 和 `PlaneOffset()`。需要连续内存的模块显式调用 pack helper；只支持 FFmpeg 帧的模块使用 typed raw 访问。不能让 `IMediaBuffer` 的旧连续内存假设渗透到 raw 路径。
6. **最后再把宏切为 `1`**：先以 feature flag 或独立构建配置验证 raw 路径，确认所有模块都通过后再把 `1` 作为默认值。长期建议从全局宏演进为类型化的 storage capability，而不是让同一个 `MediaFrame` 接口在不同编译产物中隐式改变语义。

一个适合当前代码的最小辅助原则是：`backend.ptr` 只能作为兼容字段，FFmpeg 相关模块优先从 `buffer` 的具体类型取得 `AVFrame`，并校验两者指向同一对象。这样可以避免 `void*` 被错误地解释为另一种 FFmpeg 对象，也能让所有权来源回到 `MediaFrame::buffer`。

### 3.3 切换 RAW=1 前必须有的测试

- **两路径像素一致性**：同一个固定输入，分别以 `RAW_FRAME_BUFFER=0/1` 运行，比较每个平面的 checksum、stride、宽高、像素格式和时间戳。
- **生命周期测试**：解码输出帧后继续解码多帧，第一帧仍能读取；释放 `MediaFrame` 后没有 double free、use-after-free 或泄漏。
- **引用测试**：raw `MediaFrame` 经过 converter/encoder 后，释放原始帧，编码输入引用仍然有效。
- **连续 buffer 兼容性测试**：所有必须连续访问的模块在 raw 模式下要么显式 pack 成功，要么返回清晰错误；不能出现空指针解引用。
- **异常路径测试**：`av_frame_alloc`、`av_frame_ref`、`make_shared` 失败时，所有权仍只有一个且资源全部释放。
- **格式覆盖测试**：至少覆盖 I420、NV12、带 stride 的帧；后续再加入负 stride、10 bit 和硬件帧的明确“不支持”断言。

### 3.4 如何比较 RAW=0 和 RAW=1

两条路径的比较必须固定输入、输出和编码参数。建议分别测量：decoder 输出、converter、encoder、端到端的 p50/p95/p99，另加进程 CPU、RSS、分配次数或分配总字节数、内存带宽、输出帧数和输出码率。

比较时要单独区分：

1. `source format == target format` 的直通场景：RAW=1 主要验证能否通过 `av_frame_ref()` 避免像素复制。
2. 发生缩放/像素格式转换的场景：RAW=1 只能消除转换前后的 pack/unpack，不能消除 `sws_scale` 本身的读写。
3. 是否启用 B 帧、线程数、日志等级和 mux 输出必须完全一致，否则测到的不是 raw buffer 差异。

## 4. 时间戳问题的范围说明

### 4.1 当前不修改 media 模块的原因

当前完整链路只是简单测试，测试代码固定 `25 fps` 并不等于 `FFmpegEncoder` 在所有输入上都错误。这里应先把测试输入帧率、编码目标和验证目的说明清楚；如果只是验证组件调用顺序，保持现状即可。

只有下面情况出现时，才需要把时间轴问题提升为媒体模块改造：

- 生产链路要求支持实际 VFR 或输入帧率变化；
- 同一个 encoder 需要稳定输出 CFR；
- 重连、分段或多轨 mux 要求 generation 间时间轴连续；
- 测试开始作为自动回归并要求 mux 失败立即失败。

### 4.2 明确时间轴策略

`MediaFrame::time` 以微秒表示，而 `MediaPacket` 以各自 `time_base` tick 表示。编码器的 `resolveFramePts()` 会把输入微秒 PTS 换算到编码器时间基。这是正确方向，但当前实现只更新 `next_pts_`，没有保证当前返回 PTS 比上一帧大。

当输入大约 30 fps、编码器却固定 `1/25` 时，33.3 ms 的相邻 PTS 会量化到相同的 40 ms tick。无 B 帧时，这通常也会形成相同 DTS；muxer 会拒绝“非严格递增”的 DTS。

必须在产品层选择以下其中一种策略，而不是让编码器隐式决定。

| 场景 | 时间轴策略 | 编码器配置建议 |
| --- | --- | --- |
| 保留源帧率或 VFR | 保留每帧源 PTS，缺失时才生成 | 使用足够细的时间基，例如源 `1/90000` 或 `1/1000000`；`framerate` 仅作标称信息 |
| 输出固定帧率（CFR） | 在进入 encoder 前按目标时钟主动丢帧/补帧 | `time_base = fps_den/fps_num`；每个输出帧恰好一个或多个固定 tick |
| 实时预览且允许降帧 | 队列过载时丢弃旧 raw 帧，但输出 PTS 仍按目标时钟生成 | 固定 fps，记录丢帧原因和实际输出 fps |

修复要求：

1. 为 `FFmpegEncoder` 保存 `last_submitted_pts`，并在送入 `avcodec_send_frame()` 前检测 `pts <= last_submitted_pts`。
2. 对 CFR，重复或过密帧按明确规则丢弃或分配下一 tick；不能静默把相同 PTS 送进编码器。
3. 对 VFR，使用比最小帧间隔更精细的时间基，并保留实际 `duration`；不要把 VFR 强行量化成 25 fps。
4. 若重连导致源 PTS 回跳，生成新的 `generation` 并重置/分段输出，不能把新旧代次放入同一单调 mux 时间轴。
5. 每次写包前后记录 `pts`、`dts`、`duration`、源/目标 `time_base` 和 stream generation；任何 mux 写失败都必须使回归测试失败。

### 4.3 保留颜色和画面属性

当前 `PixelFormat` 只表达内存布局，不表达 `color_range`、`colorspace`、`color_primaries`、`color_trc`、sample aspect ratio 或隔行信息。`YUVJ420P` 被映射为 `I420` 后，full-range 信息也丢失。`sws_scale` 未显式设置颜色空间参数。

当链路只做同格式编码时该问题不一定显现；一旦 YUV/RGB 转换、缩放或不同摄像机接入，就可能出现发灰、偏色、过曝或拉伸。

改造建议：

1. 在 `VideoFrameMeta` 或独立的 `VideoColorInfo` 中增加 range、colorspace、primaries、transfer、SAR、interlaced/top-field-first。
2. decoder 从 `AVFrame` 复制这些字段；converter 输出时继承或按目标规则设置。
3. RGB/YUV 或 range 改变时，使用 `sws_setColorspaceDetails()` 显式配置；同格式直通不得随意改写。
4. 用 limited/full range 色卡和 8/10 bit 样本做像素级回归，不只检查宽高和像素格式。

### 4.4 生命周期和重配置屏障

文件结束的正确顺序是：停止输入 -> `decoder.Flush()` -> 消费所有帧 -> `encoder.Flush()` -> 写完所有 packet -> `muxer.Close()`。当前组件接口已经支持显式 flush，必须把这个顺序固化为 pipeline 的 EOS 控制消息。

RTSP 重连、SPS/PPS 变化、分辨率变化或像素格式变化不能只继续使用旧 decoder/encoder：

```text
输入 generation N 结束
  -> 丢弃或 drain N 的队列（按业务策略）
  -> flush decoder / encoder
  -> 关闭或完成当前分段 mux
  -> 重新读取 StreamInfo
  -> 用 generation N+1 打开 decoder、converter、encoder、mux
```

生产链路需要把 `Data`、`EOS`、`Stop`、`Reconnect`、`Reconfigure` 作为队列中的控制事件，而不是只用空指针表示所有情况。

## 5. P1：减少复制和分配

### 5.1 以 raw AVFrame 作为阶段间视频载体

目标是让 `MediaFrame` 在 FFmpeg 视频链路中持有 `FFmpegRawFrameBuffer`，并由 `MediaFrame::buffer` 持有唯一所有权、`backend.ptr` 提供非拥有访问。这样 decoder、converter 和 encoder 可以通过 `av_frame_ref()` 共享底层 `AVBufferRef`，不复制像素 payload。

目标路径：

```text
decoder AVFrame
  -> FFmpegRawFrameBuffer                         [无像素复制]
  -> 若尺寸、格式、颜色属性都已满足 encoder：av_frame_ref -> encoder
  -> 否则：sws_scale/swr_convert 一次生成目标 AVFrame
  -> FFmpegRawFrameBuffer                         [无二次打包]
  -> av_frame_ref -> encoder
```

不要只把宏从 `0` 改为 `1` 后就结束。`FFmpegRawFrameBuffer::Data()` 返回空、`Size()` 返回 0；任何仍然假定 `IMediaBuffer` 必然连续的消费者都会失效。建议按下面顺序实施。

1. 为 `MediaFrame` 增加明确的平面访问能力，或让消费者先检查 `backend.type` 后使用 AVFrame/raw buffer；禁止通过 `Data() + offset` 访问 raw frame。
2. 增加 packed/raw 两条路径的像素一致性测试、生命周期测试和 AddressSanitizer/Windows PageHeap 检查。
3. 用运行时或构建时 feature flag 在单路本地文件上启用 raw 路径，采集每阶段 p50/p95、CPU 和内存。
4. 只有所有消费者都遵守 raw 契约后，删除 `RAW_FRAME_BUFFER` 的全局宏分支，改用类型化的 buffer 能力判断。全局宏会让同一公共接口在不同二进制中改变语义，不适合作为长期架构。

### 5.2 跳过无变换 converter

当输入和目标的宽、高、像素格式、颜色属性完全一致，且没有 OSD/AI 等需要写入像素的处理时，不应调用 `sws_scale`。直接 `av_frame_ref()` 后送 encoder 即可。

是否跳过必须由一个完整的 `FrameTransformSpec` 判断，不能只比较宽高和像素格式。至少包括：尺寸、像素格式、颜色范围/空间、旋转/SAR、裁剪、是否需要 OSD、是否需要 CPU 可写内存。

### 5.3 池化时先池化描述符和输出帧

当前 decoder、video converter、encoder receive path 都频繁调用 `av_frame_alloc()` 或 `av_packet_alloc()`。完成零拷贝后，优先池化的对象是：

- 由 decoder 输出、但已不再被下游引用的 `AVFrame` 描述符；
- converter 的目标 `AVFrame` 和其 `AVBufferRef`；
- encoder 输出 `AVPacket` 描述符；
- `MediaFrame` / `MediaPacket` 的小对象包装器（仅在 profile 表明确有热点时）。

池必须受队列上限约束，并通过 `shared_ptr` 自定义 deleter 在最后一个消费者释放后归还。不能复用仍被 encoder、muxer 或异步消费者引用的 AVFrame/AVPacket。

## 6. P1：建立背压、丢帧和线程边界

### 6.1 建议的最小线程拓扑

单路实时转码的第一版不需要通用线程池。采用三个单一所有者阶段即可：

```text
T0 demux/read
  -- bounded Packet queue -->
T1 decode + transform + encode
  -- bounded EncodedPacket queue -->
T2 mux / network write
```

T1 内先把 decode、转换、编码放在同一线程，减少一条 raw frame 队列和跨线程引用；只有 profile 表明转换/推理是瓶颈时，再将 transform 拆到独立线程。每个 `AVCodecContext`、`SwsContext` 和 muxer 都必须只由一个线程调用。

仓库已有 `BoundedSpscQueue`，适合上述一生产者一消费者边界。它只能用于严格 SPSC；多个消费者要显式 fan-out，不要把同一个队列误作广播机制。

### 6.2 队列容量按时延预算设计

容量不能只写一个“512 包”的常数。先定义允许的阶段排队时延，再换算帧数：

```text
frame_capacity = ceil(stage_latency_budget_ms / frame_interval_ms)
packet_capacity = frame_capacity * packets_per_frame_upper_bound
```

例如 30 fps、允许解码前最多积压 100 ms 时，raw frame 队列通常不应超过 3 到 4 帧。不同协议、GOP、分片方式的压缩包数量不同，因此 packet 队列要按实测上限设置并记录峰值。

实时预览的默认原则是“保持最新”，不是“绝不丢帧”：

| 队列位置 | 满时策略 | 不应做的事 |
| --- | --- | --- |
| 压缩 packet -> decoder | 尽量阻塞上游或在关键帧处重同步；记录恢复次数 | 任意丢中间压缩包，可能破坏解码参考链 |
| decoded/raw frame -> encoder | 丢弃最旧的待编码帧，保持最近帧；计数并打点 | 无限排队，导致视频越来越旧 |
| encoded packet -> mux | 短暂有界等待；持续满时重连/分段并等待关键帧 | 丢弃任意 H.264 packet 后继续写，可能造成下游花屏 |

录像或证据留存属于不同策略：应限制输入速率、增加磁盘/网络能力，或让上游受控阻塞，而不是沿用预览的丢帧策略。

### 6.3 muxer 写失败的处理

`FFmpegMuxer::Write()` 使用消费式语义，`av_interleaved_write_frame()` 调用后同一个 `AVPacket` 不可原样重试。网络输出失败时应：

1. 记录失败类型、队列时长、最后一个成功 keyframe 的 PTS；
2. 停止向旧 muxer 继续写包；
3. 重建输出连接/分段；
4. 从下一关键帧开始输出，必要时请求上游 IDR；
5. 如果确实需要重试，在调用写入前以 `av_packet_ref()` 准备独立重试副本，不要复用已消费 packet。

## 7. P1：编码器配置应反映实际能力

### 7.1 分离三种像素格式

当前 `findVideoEncoder()` 可能在 I420 不被支持时选择 NV12，但 `codec_ctx_->pix_fmt` 使用实际选择值，内部 converter 目标和输入帧校验仍使用 `cfg.video().pixel_format`。这使“回退到 NV12”的代码路径不完整。

应把下列概念拆开：

```text
source_frame_format       decoder/上游实际提供的格式
normalize_target_format   transform 后保证提供给 encoder 的格式
encoder_accepted_format   本次 avcodec_open2 实际选择的 AVPixelFormat
```

`encoder_accepted_format` 应保存在 encoder 会话状态中，并成为规范化目标；`Encode()` 不应因为原始输入格式不同立刻失败，而应要求上游 transform 已经生成实际目标格式。打开成功日志也应打印真实像素格式，而不是当前的 `todo` 占位值。

### 7.2 码率控制和低时延配置

`EncoderConfig` 已声明 `max_bitrate`、`rc_buffer_size`，但当前 `Open()` 只设置了 `bit_rate`、GOP、B 帧、preset、tune 和 CRF。应先补齐配置语义，而不是认为字段已经生效。

建议定义互斥的 `RateControlMode`：

| 模式 | 必填字段 | 典型用途 |
| --- | --- | --- |
| CBR/VBV | target bitrate、max bitrate、VBV buffer | 受控带宽、低延迟直播 |
| ABR | target bitrate，可选 max bitrate/VBV | 质量和带宽折中 |
| CRF/CQP | quality 参数，可选 max rate 保护 | 本地录制或质量优先 |

实现时要注意：FFmpeg 的 `AVCodecContext::rc_buffer_size` 单位是 bit，不应沿用当前注释中的“字节”含义。配置命名应带单位，例如 `vbv_buffer_bits`，并在 `avcodec_open2()` 后检查未被消费的私有 option。`bitrate + crf` 同时设置的优先级也必须显式规定，不能依赖不同编码器的默认行为。

低时延 H.264 的常见起点是 `max_b_frames=0`、`tune=zerolatency`、GOP 约 1 到 2 秒、有限 VBV buffer；最终值必须由目标协议、码率和可接受首帧等待时间决定。`ultrafast` 只是在 CPU 压力下的取舍，不能替代吞吐和画质评测。

### 7.3 解码和编码线程

decoder 现在固定 `thread_count = 1`，encoder 默认也为单线程。这有利于可重复测试和较低的帧重排序，但高分辨率多路转码可能不足。

建议增加每路可配置的 decode/encode thread policy，并在压测后选择：

- 低时延单路：从 1 线程起测，确认 frame-threading 不引入额外缓存；
- 离线转码或吞吐优先：评估 slice/frame threads 和 CPU 核数配额；
- 多路场景：每路限额，避免每个 encoder 自动抢占所有 CPU；
- 任一配置变更都要比较端到端 p95 时延，而不只比较 fps。

## 8. 拉流层的优化边界

当前 Puller 已具备连接/读取超时、中断回调、RTSP TCP/UDP 选项、探测和结构化错误分类。这些是可靠性基础，优化时应保持。

建议针对每个摄像机 profile 明确以下项目：

1. RTSP TCP 与 UDP 分开测试。TCP 通常降低丢包引起的花屏，UDP 在受控网络下可能更低时延。
2. `fflags=nobuffer`、`reorder_queue_size`、socket buffer 和探测参数只对实时输入 profile 生效；录像回放和文件输入不应无差别启用。
3. 启动阶段单独记录 DNS/握手、`avformat_open_input`、`avformat_find_stream_info`、首 packet、首 decoded frame、首 keyframe 和首 muxed packet 的耗时。
4. 调整 `probesize`、`analyzeduration` 前先记录 SPS/PPS 是否稳定到达。过小探测会使流信息不完整，过大探测会拉高首帧时延。
5. URL、密码、Authorization header 和完整 FFmpeg option 字典不得写入普通日志。

不要试图用更大的缓冲区解决处理过慢的问题。它只能把卡顿变成更高时延；真正的处理能力问题应由降分辨率、降帧率、丢帧策略、编码参数或硬件路径解决。

## 9. 硬件编解码的进入条件

只有在完成零拷贝、背压和指标后，才值得加入 D3D11VA、QSV、NVENC 或 AMF。原因是“硬解 -> CPU download -> swscale/OSD -> CPU upload -> 硬编”经常比纯软件链路更慢，并会增加显存/系统内存同步。

硬件路径的验收条件：

```text
decode hw frame
  -> 保持 AVHWFramesContext
  -> GPU scale/format/OSD（如需要）
  -> hardware encoder
```

若 AI/OSD 必须在 CPU 执行，应把一次 `hwdownload` 的成本纳入基线；只有总时延和 CPU 占用优于软件路径时才保留。硬件实现也必须提供软件回退、设备丢失恢复、编解码器能力探测和按路资源限额。

## 10. 指标、日志和性能基线

每个阶段至少记录下面的指标，按 stream id、generation、codec、分辨率和 profile 分组。

| 类别 | 必要指标 |
| --- | --- |
| 输入 | open 时间、首包时间、包数/字节、读取错误、重连、输入 PTS 间隔 |
| 队列 | 当前/最大深度、排队时长、push 失败、丢帧原因、控制事件数 |
| 解码 | 包数、帧数、解码耗时 p50/p95/p99、输出像素格式、frame reorder 数 |
| 转换 | bypass 数、sws 次数、缩放次数、耗时分位、颜色转换次数、输出格式 |
| 编码 | 输入/输出 fps、编码耗时分位、关键帧间隔、输出字节、实际码率、PTS/DTS 违规 |
| 输出 | header/首包时间、mux 写失败、网络阻塞时长、重连、尾帧 flush 数 |
| 端到端 | ingest 到 encode/mux 的处理时延；有可信采集时钟时再计算 capture-to-display 时延 |

阶段耗时应使用 `steady_clock`；媒体时钟应保留 PTS/time base。两者不能混用。日志默认只按周期输出聚合值，出现错误或阈值越界时才输出单帧 PTS/DTS 诊断，避免日志 I/O 本身成为瓶颈。

基线报告至少包含：机器型号、GPU/驱动、FFmpeg 版本、编解码器名称、输入协议、分辨率/fps/码率、网络条件、队列配置、运行时长、CPU/GPU/内存、上述分位指标。没有这些上下文的“平均耗时”不能用于优化决策。

## 11. 测试和验收矩阵

现有 `test_ffmpeg_puller_decoder_converter_encoder.cpp` 将私有 RTSP 地址、60 秒时长、25 fps 编码配置和文件输出放在同一个可执行程序中。它适合作为人工联调样例，不适合作为默认 CTest 回归：它会受网络、设备状态和源帧率影响；而且 mux 写失败仅记录，最终仍可能返回成功。

建议拆为下列测试。

| 测试 | 输入 | 应断言的结果 | 标签 |
| --- | --- | --- | --- |
| packet/时间基单测 | 人工构造 packet | 重标定结果、缺失时间戳和流索引 | unit |
| decoder->encoder 时间轴 | 短本地 H.264，25/30/VFR 三组 | 输出 PTS/DTS 单调、时长、flush 帧数 | unit |
| packed/raw 一致性 | 固定色卡帧 | 平面内容、stride、颜色属性、生命周期 | unit |
| transform bypass | 同格式和变换两组帧 | 同格式不调用 sws；变换后像素/尺寸正确 | unit |
| 背压 | ScriptedPuller + 慢 encoder/mux fake | 队列上界、丢帧策略、停止时间、generation 隔离 | unit |
| 本地完整闭环 | 本地文件 -> mp4 | `ffprobe` 可读、帧数/时长/码率合理、无 mux 错误 | integration-local |
| RTSP 端到端 | 从环境变量读取 URL | 首帧、重连、断流恢复、时延指标 | integration-network |
| 长稳压测 | 真实相机或可重复生成器 | 数小时无泄漏、无队列无界增长、无 DTS 错误 | performance |

网络测试必须由显式的 `VIDEO_TEST_RTSP_URL` 或 CMake 选项启用，不能将私网 IP 编译为默认测试。CTest 应加 `unit`、`integration`、`network`、`performance` 标签，CI 默认只跑 unit 与 local integration。

当前已可直接运行的本地时间戳测试 `test_media_test_ffmpeg_timestamp_handling.exe` 能验证一条合成 25 fps 时间线在 MP4 中归零。它不能覆盖 RTSP 真实帧率、转换路径、队列压力或重连，因此不能替代上述矩阵。

## 12. 分阶段实施清单

### 第一阶段：让媒体模块和 raw 路径可切换

- [ ] 明确 `FFmpegRawFrameBuffer` 的所有权、平面访问、只读和线程契约。
- [ ] 修复 raw buffer 异常路径的 AVFrame 所有权转移和释放保证。
- [ ] 检查 decoder、converter、encoder 对 `Data()/Size()` 的使用，隔离连续 buffer 假设。
- [ ] 增加 packed/raw 像素、生命周期、引用和异常路径测试。
- [ ] 在相同输入和参数下完成 `RAW_FRAME_BUFFER=0/1` 性能基线。

### 第二阶段：降低单路成本

- [ ] 为 MediaFrame 增加颜色属性和 raw frame 访问契约。
- [ ] 在独立构建配置下启用 raw AVFrame 路径，完成 packed/raw 回归。
- [ ] 实现 transform bypass，比较开启前后的 CPU、p95 和内存带宽。
- [ ] 池化 converter 输出帧和 encoder packet 描述符；只在 profile 表确认分配热点后池化包装对象。
- [ ] 修正 actual encoder pixel format 与 converter target 的契约。

### 第三阶段：控制实时延迟

- [ ] 拆分 demux、处理、mux 三个单所有者阶段并接入有界 SPSC 队列。
- [ ] 定义 preview 与 recording 两套背压策略，并为每种丢弃记录原因。
- [ ] 为 EOS、Stop、Reconnect、Reconfigure 引入控制事件和 generation。
- [ ] 将 mux 网络阻塞从拉流线程隔离，建立失败后的关键帧恢复策略。

### 第四阶段：扩大吞吐

- [ ] 补齐 rate-control 模式、VBV 单位和 option 生效检查。
- [ ] 根据实测调整 decoder/encoder 线程数和分辨率/fps profile。
- [ ] 在软件路径达标后，再对 D3D11VA/QSV/NVENC/AMF 做同口径对比。
- [ ] 增加多路压测、设备丢失恢复和资源配额。

## 13. 本次检查依据

关键代码位置如下，后续修改应从这些边界开始审查：

- `src-cpp/src/media/puller/ffmpeg_puller.cpp`：RTSP option、超时中断、`av_read_frame` 和 `av_packet_move_ref`。
- `src-cpp/include/media/ffmpeg_raw_frame_buffer.h`：当前 `RAW_FRAME_BUFFER = 0`。
- `src-cpp/src/media/decoder/ffmpeg_decoder.cpp`：软件 decoder 固定单线程、packed/raw 分支、回调输出。
- `src-cpp/src/media/converter/media_frame_converter.cpp`：raw fast path、packed copy path、AVFrame 所有权和微秒时间戳约定。
- `src-cpp/src/media/converter/ffmpeg_video_converter.cpp`：SwsContext 缓存、每帧输出分配和 `sws_scale`。
- `src-cpp/src/media/encoder/ffmpeg_encoder.cpp`：编码格式选择、时间戳量化、`max_bitrate`/`rc_buffer_size` 未应用、输出 packet 所有权。
- `src-cpp/src/media/pusher/ffmpeg_muxer.cpp`：时间基重标定、文件时间戳归零和消费式写包。
- `src-cpp/src/media/stream/stream_session.cpp`：读线程直接回调、重连和当前未落地的 watchdog/jitter 语义。
- `src-cpp/test/media/test_ffmpeg_puller_decoder_converter_encoder.cpp`：当前人工 RTSP 样例及其测试隔离问题。

`MediaStreamSource` 仍是未完成骨架，且被 `src-cpp/CMakeLists.txt` 显式排除编译。生产链路不要通过它新增调度逻辑，先在已编译的 Puller/Session/Decoder/Encoder/Muxer 边界完成上述改造。
