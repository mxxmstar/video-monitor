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
```

因此 converter 需要负责两个方向的适配：

```text
mediaFrameToAVFrame()
    MediaFrame 的元数据和 buffer -> 一个可供 sws/swr 使用的 AVFrame

avFrameToMediaFrame()
    转换后的 AVFrame -> 独立拥有数据的 MediaFrame
```

## 4. mediaFrameToAVFrame 做什么

### 4.1 FFmpeg 后端快速路径

当输入满足：

```cpp
input.backend.type == BackendHandle::FFMPEG
input.backend.ptr != nullptr
```

converter 把 `backend.ptr` 当作 `AVFrame*`，调用 `av_frame_ref()` 引用它的数据。

这里的 `backend.ptr` 只是后端对象指针，不是独立的所有权。输入的 `buffer` 必须继续存活，因为当前解码器使用的 `FFmpegFrameBuffer` 负责释放真正的 `AVFrame`。

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

当前解码器产生的 FFmpeg 后端 MediaFrame 大致是：

```text
MediaFrame.buffer
    -> FFmpegFrameBuffer
        -> 拥有 AVFrame

MediaFrame.backend.ptr
    -> 指向同一个 AVFrame
        -> 非拥有指针
```

converter 的输出经过 `avFrameToMediaFrame()` 后，会复制到新的 `SimpleBuffer`。此时输出不再依赖 FFmpeg 输出 AVFrame 的生命周期，所以设置：

```cpp
media_frame->backend.type = BackendHandle::NONE;
media_frame->backend.ptr = nullptr;
```

这并不表示输出没有所有权。恰恰相反，输出数据由 `media_frame->buffer` 中的 `SimpleBuffer` 独立拥有，后续编码器可以安全地读取这个 `MediaFrame`。

如果将来希望编码器直接接收 FFmpeg AVFrame 的零拷贝输出，也可以保留 `BackendHandle::FFMPEG`，但必须同时保证 `buffer` 持有对应的 AVFrame，并且所有使用者都遵守这个生命周期约定。当前公共 converter 选择复制到 `SimpleBuffer`，是为了让接口不依赖 FFmpeg 对象。

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
2. `MediaFrameConverter` 输出统一复制到 `SimpleBuffer`，暂时不是零拷贝。
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
5. 根据性能需求，再评估 AVFrame 零拷贝和 buffer 池化。

