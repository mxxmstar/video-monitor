# MediaFrameConverter 学习说明

## 1. 为什么需要 converter

解码器输出的是已经解码的音视频帧，编码器也需要接收已经解码的音视频帧。但是，解码器输出格式和编码器要求的输入格式经常不同，例如：

- 视频分辨率不同：解码得到 `1280x720`，编码器要求 `1920x1080`。
- 视频像素格式不同：解码得到 `YUV420P`，编码器要求 `NV12`。
- 音频采样率不同：解码得到 `8000 Hz`，编码器要求 `16000 Hz`。
- 音频通道数不同：解码得到单声道，编码器要求双声道。
- 音频采样格式不同：解码得到 `S16P`，编码器要求 packed 的 `S16`。

这些工作属于“媒体数据格式转换”，不属于拉流、解码或编码本身。把它单独放到 converter 模块，可以让各个模块职责更清楚：

```text
Puller      读取编码数据包
Decoder     编码数据包 -> 解码帧
Converter   解码帧 -> 编码器需要的解码帧
Encoder     解码帧 -> 编码数据包
```

后续如果需要使用 SIMD 优化，只需要增加另一种转换后端，不需要把 SIMD 代码塞进 decoder 或 encoder。

## 2. 当前公共接口

当前对外使用统一的 `MediaFrame -> MediaFrame` 接口：

```cpp
MediaFrameConverter converter;

MediaFrameConverterConfig config;
config.video.width = 1920;
config.video.height = 1080;
config.video.pixel_format = PixelFormat::kNV12;

if (!converter.Open(config)) {
    // converter.LastError()
}

std::shared_ptr<MediaFrame> output;
if (!converter.Convert(*input, output)) {
    // converter.LastError()
}
```

`Open()` 决定目标格式和使用的后端，`Convert()` 根据输入帧的 `MediaType` 自动选择视频或音频转换，`Close()` 释放 `SwsContext`、`SwrContext` 以及相关对象。

当前只实现了：

```text
ConvertBackend::FFmpeg
```

`OpenCV` 和 `SIMD` 目前只是预留枚举值，尚未实现。

## 3. MediaFrame 和 AVFrame 的区别

`AVFrame` 是 FFmpeg 内部使用的帧对象。它包含 FFmpeg 的数据指针、linesize、格式、分辨率、采样率、声道布局和时间戳等信息。

`MediaFrame` 是工程公共层的数据结构。它使用：

- `meta` 描述媒体数据格式。
- `buffer` 持有实际字节数据。
- `time` 保存工程层时间戳。
- `backend` 记录数据是否仍然关联某个后端对象。

两者的主要差异是所有权表达方式：

```text
AVFrame
    data[] / extended_data[] 指向 FFmpeg 管理的内存
    引用计数由 AVBufferRef 管理

普通 MediaFrame
    buffer 持有 SimpleBuffer
    SimpleBuffer 持有连续字节数组

FFmpeg 后端 MediaFrame
    buffer 持有 FFmpegRawFrameBuffer
    FFmpegRawFrameBuffer 持有 AVFrame
    backend.ptr 指向同一个 AVFrame（非拥有）
```

因此 converter 需要负责两个方向的适配：

```text
mediaFrameToAVFrame()
    MediaFrame 的元数据和 buffer -> 一个可供 sws/swr 使用的 AVFrame

avFrameToMediaFrame()
    转换后的 AVFrame -> 复制到 SimpleBuffer 的 MediaFrame

AdoptAVFrame()
    拥有所有权的 AVFrame -> 由 FFmpegRawFrameBuffer 直接接管
```

## 4. mediaFrameToAVFrame 做什么

### 4.1 FFmpeg 后端快速路径

当输入满足：

```cpp
input.backend.type == BackendHandle::FFMPEG
input.backend.ptr != nullptr
```

converter 把 `backend.ptr` 当作 `AVFrame*`，调用 `av_frame_ref()` 引用它的数据。

这里的 `backend.ptr` 只是后端对象指针，不是独立的所有权。输入的 `buffer` 必须继续存活，因为 `FFmpegFrameBuffer` 或 `FFmpegRawFrameBuffer` 负责释放真正的 `AVFrame`。

`av_frame_ref()` 会增加 FFmpeg 数据缓冲区的引用计数，所以 converter 释放自己的临时 AVFrame 时，不会释放输入帧仍在使用的数据。

### 4.2 普通 MediaFrame

当输入没有 FFmpeg 后端句柄时，converter 根据 `meta` 构造新的 AVFrame：

1. 从 `VideoFrameMeta` 或 `AudioFrameMeta` 读取格式参数。
2. 设置 AVFrame 的格式、宽高、采样率、样本数和声道布局。
3. 调用 `av_frame_get_buffer()` 分配 FFmpeg 自己管理的目标内存。
4. 按 `PlaneInfo.offset`、`PlaneInfo.stride` 和 `PlaneInfo.size` 从 `MediaFrame.buffer` 复制数据。
5. 设置时间戳。

这种路径会发生一次数据拷贝，但它不依赖 `MediaFrame.buffer` 的具体实现，适合 `SimpleBuffer` 以及将来的其他公共层 buffer。

## 5. 视频平面数据

视频数据通常由一个或多个平面组成。平面数量和每个平面的大小由像素格式决定。

以 `I420` 为例，宽度为 `W`、高度为 `H` 时：

```text
Y 平面：W       x H       字节
U 平面：ceil(W/2) x ceil(H/2) 字节
V 平面：ceil(W/2) x ceil(H/2) 字节
```

`NV12` 则是两个平面：

```text
Y 平面：W x H 字节
UV平面：W x ceil(H/2) 字节，U/V 交错存放
```

每个平面在 `MediaFrame.buffer` 中用 `PlaneInfo` 描述：

```text
offset  平面开始位置，相对于 buffer 起始地址
stride  一行数据占用的字节数
size    该平面可读取的总字节数
```

视频输入到 AVFrame 时，converter 使用 `av_image_copy()`，它会按照像素格式理解平面高度，并正确处理源和目标的不同 stride。

AVFrame 输出到 MediaFrame 时，converter 使用 `av_image_copy_to_buffer()` 把数据整理成连续 buffer，再重新计算每个平面的 offset、stride 和 size。

## 6. 音频 packed 和 planar

音频的核心区别是多个声道如何排列。

### 6.1 planar

以双声道 `S16P` 为例：

```text
plane[0] = L0, L1, L2, ...
plane[1] = R0, R1, R2, ...
```

每个平面的字节数是：

```text
nb_samples * bytes_per_sample
```

总字节数是：

```text
nb_samples * bytes_per_sample * channels
```

### 6.2 packed

以双声道 `S16` 为例，所有声道交错存储在一个平面：

```text
L0, R0, L1, R1, L2, R2, ...
```

唯一的平面字节数也是总字节数：

```text
nb_samples * bytes_per_sample * channels
```

因此，converter 在输出 packed 音频时必须按通道数分配 buffer。只按 `nb_samples * bytes_per_sample` 分配会造成容量不足。

`AudioFrameMeta::planes` 的约定是：

- planar：每个声道一个 `PlaneInfo`。
- packed：只有 `planes[0]` 有效，它描述整个交错数据平面。

## 7. FFmpeg 后端内部实现

### 7.1 FFmpegVideoConverter

视频转换使用 `libswscale`：

```text
输入 AVFrame
    -> sws_getContext() 创建或复用 SwsContext
    -> sws_scale()
    -> 输出 AVFrame
```

`SwsContext` 会缓存输入帧的宽度、高度和像素格式。当这些参数变化时，当前实现释放旧上下文并重新创建。

目标宽度、目标高度和目标像素格式来自 `VideoConvertConfig`。当前默认使用 `SWS_BILINEAR`。

### 7.2 FFmpegAudioConverter

音频转换使用 `libswresample`：

```text
输入 AVFrame
    -> swr_alloc_set_opts2() 创建或复用 SwrContext
    -> swr_convert()
    -> 输出 AVFrame
```

当输入采样率、采样格式或声道布局变化时，当前实现重新创建 `SwrContext`。

采样率转换可能在 `SwrContext` 内部产生延迟，因此输出容量会根据：

```text
已有 delay + 当前输入样本数
```

计算，而不是简单地只按照当前输入样本数分配。

## 8. BackendHandle 和所有权

`BackendHandle` 不是 `MediaFrame.buffer` 的替代品，也不是一个通用智能指针。它只表达：

```cpp
BackendHandle::NONE
    MediaFrame 的 buffer 保存公共层数据，MediaFrame 不依赖 FFmpeg 对象

BackendHandle::FFMPEG
    MediaFrame 还关联一个 FFmpeg 对象，backend.ptr 指向该对象
```

当前 decoder 和 converter 都输出不复制的 raw 形式：

```text
MediaFrame.buffer
    -> FFmpegRawFrameBuffer
        -> 拥有 AVFrame

MediaFrame.backend.ptr
    -> 指向同一个 AVFrame
    -> 非拥有指针
```

converter 的内部输出路径使用 `AdoptAVFrame()`，由
`FFmpegRawFrameBuffer` 接管 AVFrame，不再复制到新的 `SimpleBuffer`：

```cpp
media_frame->backend.type = BackendHandle::FFMPEG;
media_frame->backend.ptr = raw_buffer->GetFrame();
```

`backend.ptr` 本身不负责释放。真正的所有权在
`MediaFrame.buffer -> FFmpegRawFrameBuffer`，因此只要 `MediaFrame` 还活着，
编码器就可以通过 `av_frame_ref()` 引用这个 AVFrame 的数据。

`FFmpegRawFrameBuffer::Data()` 返回空指针、`Size()` 返回 0，这是有意的：
多平面 AVFrame 没有一个通用的连续字节区。需要访问像素或样本时，应通过
`GetFrame()` 或 `PlaneData()`；只接受连续 `IMediaBuffer` 的模块仍应使用
`FFmpegFrameBuffer` 或 `SimpleBuffer`。

`FFmpegFrameBuffer` 仍保留给必须读取连续字节区的兼容场景，但 decoder、
converter 不再使用它。当前拉流、解码、转换、编码链路使用 AVFrame 引用传递；
只有 `sws_scale()` 或 `swr_convert()` 在确实改变格式、尺寸、采样率时才会生成
新的目标数据。

## 9. 时间戳约定

当前 `MediaFrame::time` 字段名称使用 `_us`，因此 converter 暂按微秒保存时间戳。

```text
MediaFrame.time.pts_us       微秒
MediaFrame.time.dts_us       微秒
MediaFrame.time.duration_us  微秒
```

当前实现内部构造 AVFrame 时也把这些值写入 AVFrame 的时间戳字段。由于 AVFrame 本身不携带通用 `time_base`，真正送入编码器前，编码器必须根据自己的 `codec time_base` 做换算。

无时间戳使用工程统一的：

```cpp
kNoTimestamp
```

进入 FFmpeg 时转换为 `AV_NOPTS_VALUE`，从 FFmpeg 返回时再转换回 `kNoTimestamp`。

## 10. 当前限制

当前实现用于学习和跑通最小链路，限制包括：

1. 只实现 FFmpeg 后端，尚未实现 OpenCV 和 SIMD。
2. decoder 和 converter 已避免 `AVFrame -> SimpleBuffer -> AVFrame` 的额外
   拷贝；`FFmpegRawFrameBuffer` 不能提供连续 `Data()/Size()` 接口。
3. 视频和音频配置在 `Open()` 时固定；输入媒体类型必须对应已打开的转换器。
4. 音频声道布局接口当前使用 `uint64_t` native channel mask，不能完整表达自定义声道布局。
5. converter 当前不负责编码器 time_base 的时间戳换算。
6. 当前支持的像素格式和采样格式以 `media_frame_converter.cpp` 中的映射函数为准。

## 11. 后续扩展顺序

建议按以下顺序继续学习和扩展：

1. 在单元测试中检查转换后的实际视频平面内容，而不仅是元数据。
2. 增加 packed 音频和多声道 planar 音频的输入输出测试。
3. 将 converter 接入 encoder，验证“解码 -> 转换 -> 编码”的完整链路。
4. 在保持 `MediaFrame -> MediaFrame` 公共接口不变的前提下，增加 SIMD 后端。
5. 根据性能需求，再评估 AVFrame 池化和跨线程复用。

## 12. 问题记录

### 12.1 编码帧率与输入帧率不匹配导致 MP4 DTS 非单调

#### 现象

运行下面的完整链路时，`RAW_FRAME_BUFFER=0`（非零拷贝）和
`RAW_FRAME_BUFFER=1`（零拷贝）两种配置都会出现：

```text
FFmpegPuller -> FFmpegDecoder -> MediaFrameConverter -> FFmpegEncoder -> FFmpegMuxer
```

会周期性出现：

```text
Application provided invalid, non monotonically increasing dts to muxer
av_interleaved_write_frame failed: Invalid argument
```

问题在大约每 5 到 6 帧出现一次。当前测试仍然会继续运行并最终打印 `test passed`，因为 muxer 写包失败只被记录，没有让测试失败。

#### 实测数据

输入流时间基为 `1/90000`，解码帧的时间戳间隔约为 `33.3 ms`，实际接近 30 FPS。例如：

```text
decoded pts_us=2100611 -> encoded pts=53
decoded pts_us=2133878 -> encoded pts=53
```

测试中的编码器固定配置为 25 FPS，编码器时间基为 `1/25`，每个时间 tick 为 `40000 us`。因此相邻输入时间戳经过 `av_rescale_q()` 后可能落到同一个编码时间 tick，产生重复 PTS；由于配置了 `max_b_frames=0`，编码包的 DTS 通常也相同。

例如首包时间戳为 50，MP4 输出时间基为 `1/12800` 时，重复的编码时间戳 53 会被 muxer 转换为：

```text
(53 - 50) * 512 = 1536
```

所以 FFmpeg 报告 `1536 >= 1536`。这证明重复 DTS 在进入 muxer 前已经产生。

#### 原因

问题与视频数据拷贝方式或 `FFmpegRawFrameBuffer` 的内存所有权无关，而是输入时间戳与编码器时间基不匹配：

1. 输入帧实际约为 30 FPS，但测试编码器固定为 25 FPS。
2. `FFmpegEncoder::resolveFramePts()` 将微秒时间戳量化到 `1/25`。
3. `next_pts_` 只被更新，没有约束当前返回的 PTS 必须大于上一帧，因此重复 tick 会直接传给编码器。
4. MP4 muxer 要求同一视频流的 DTS 严格递增，重复 DTS 会被 `av_interleaved_write_frame()` 拒绝。

#### 验证结果

将测试中的：

```cpp
video_enc_cfg.fps_num = 25;
```

修改为：

```cpp
video_enc_cfg.fps_num = 30;
```

后，输入流与编码器帧率匹配，当前 DTS 错误不再出现。

#### 修复方向

- 如果目标是固定 25 FPS，应在编码前执行明确的 CFR 重采样策略，例如按输出时间轴丢帧或补帧，并保证输出 PTS 严格递增。
- 如果目标是保留输入帧率，应根据实际输入帧时间戳配置编码器，不要固定使用 25 FPS；同时应使用更细的编码时间基，例如 `1/90000` 或 `1/1000000`。
- `resolveFramePts()` 应在存在有效输入 PTS 时同时保证返回值单调递增，而不是只更新 `next_pts_`。
- 完善测试断言：统计 `muxer.Write()` 失败次数，任何 packet 写入失败都应使测试失败；日志也应同时打印 packet 的 `pts`、`dts`、`duration` 和 `time_base`。

### 12.2 零拷贝对 converter 性能的影响

在相同测试链路下，零拷贝路径的 converter 耗时明显低于非零拷贝路径。当前实测统计如下：

| 路径 | max | min | avg |
| --- | ---: | ---: | ---: |
| 零拷贝 | 2.068 ms | 0.463 ms | 0.675 ms |
| 非零拷贝 | 6.634 ms | 1.322 ms | 1.798 ms |

零拷贝路径的平均转换耗时约为非零拷贝路径的 37.5%。主要原因是非零拷贝路径需要将 AVFrame 平面数据复制到连续 buffer，后续编码前还需要再次构造或复制 AVFrame；零拷贝路径则通过 `FFmpegRawFrameBuffer` 保留 AVFrame，并使用 `av_frame_ref()` 传递底层数据。

以上数据是当前机器和当前测试流下的实测结果，实际数值会受到分辨率、像素格式、缩放参数、CPU 和输入帧率影响。
