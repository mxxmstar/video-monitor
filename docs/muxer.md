# FFmpegMuxer 模块学习说明

## 1. 为什么需要 muxer

编码器输出的是已经编码的音视频数据包（MediaPacket），但这些数据包只是内存中的字节序列，无法直接被播放器读取。muxer（复用器）负责将编码后的数据包按照特定容器格式（如 MP4、MKV、FLV 等）组织成完整的媒体文件。

muxer 模块的职责是：

```text
输入 MediaPacket（已编码） -> 按容器格式组织 -> 输出到文件/网络流
```

它不关心数据包从哪里来（编码器或其他来源），只负责：

- 创建输出容器上下文（AVFormatContext）。
- 配置输出流的 codec 参数（codecpar）。
- 将 MediaPacket 的时间戳重标定到容器时间基。
- 写入容器文件头（header）、交错帧（interleaved frames）、文件尾（trailer）。

## 2. 当前公共接口

当前对外使用统一的 `MediaPacket -> 容器文件` 接口：

```cpp
FFmpegMuxer muxer;

MediaTrackConfig config;
config.media_type = MediaType::VIDEO;
config.codec_type = CodecType::H264;
config.time_base_num = 1;
config.time_base_den = 1000000;
config.video().width = 1920;
config.video().height = 1080;
config.video().fps = 25.0f;
config.extra_data = encoder_extradata; // SPS/PPS

if (!muxer.Open("output.mp4", config)) {
    // 打开失败
}

// 写入编码后的 packet
if (!muxer.Write(encoded_packet)) {
    // 写入失败
}

// 完成后关闭，自动写入 trailer
muxer.Close();
```

`Open()` 创建容器上下文并写入 header，`Write()` 将 packet 写入容器，`Close()` 写入 trailer 并释放资源。

当前只实现了：

```text
FFmpegMuxer（基于 libavformat）
```

## 3. MediaPacket 和 AVPacket 的区别

`AVPacket` 是 FFmpeg 内部使用的数据包对象。它包含 FFmpeg 的数据指针、时间戳、流索引、关键帧标志等信息。

`MediaPacket` 是工程公共层的数据结构。它使用：

- `buffer` 持有实际编码数据（通常是 FFmpegPacketBuffer 包装的 AVPacket）。
- `time` 保存工程层时间戳（pts、dts、duration）。
- `backend` 记录数据是否关联某个后端对象（如 AVPacket 指针）。
- `type/codec` 描述媒体类型和编码格式。

两者的主要差异是时间戳表达方式：

```text
AVPacket
    pts/dts 使用 AVStream::time_base 的 tick 单位
    例如 1/90000 表示每个 tick 是 1/90000 秒

MediaPacket
    time.pts_us / time.dts_us 使用微秒单位
    time_base 描述时间戳的精度（如 1/1000000 表示微秒）
```

因此 muxer 需要负责两个方向的适配：

```text
MediaPacket -> AVPacket
    提取 backend.ptr 作为 AVPacket*
    重标定时间戳到 AVStream::time_base
    设置 stream_index 和 keyframe 标志

AVPacket -> 容器写入
    av_interleaved_write_frame() 写入交错帧
    FFmpeg 自动处理容器格式细节
```

## 4. MediaTrackConfig 配置

`MediaTrackConfig` 是 muxer 的统一配置结构，包含：

- `media_type`：媒体类型（视频/音频）。
- `codec_type`：编码格式（H264/H265/AAC/OPUS）。
- `time_base_num/den`：时间基（如 1/1000000 表示微秒）。
- `extra_data`：codec extradata（如 H.264 的 SPS/PPS）。
- `track_config`：视频或音频的独有配置（使用 `std::variant`）。

### 4.1 VideoTrackConfig

视频轨道的独有配置：

```text
width / height        分辨率
fps                   帧率（浮点数）
```

### 4.2 AudioTrackConfig

音频轨道的独有配置：

```text
sample_rate           采样率
channels              通道数
```

## 5. FFmpegMuxer 内部实现

### 5.1 Open() 流程

```text
1. 调用 Close() 清理旧状态（支持重复 Open）
2. 校验 MediaTrackConfig 有效性
3. 分配 AVFormatContext（avformat_alloc_output_context2）
4. 创建视频流（avformat_new_stream）
5. 配置 video_stream_->codecpar（codec_id、宽高、extradata）
6. 设置 video_stream_->time_base
7. 打开输出文件（avio_open）
8. 写入文件头（avformat_write_header）
```

### 5.2 Write() 流程

```text
1. 校验 packet 有效性（buffer、backend、time_base）
2. 提取 AVPacket*（从 packet.backend.ptr）
3. 覆盖 AVPacket 的 pts/dts/duration（使用 MediaPacket 的元数据）
4. 设置 stream_index 和 keyframe 标志
5. 重标定时间戳（av_packet_rescale_ts）
6. 写入交错帧（av_interleaved_write_frame）
```

### 5.3 Close() 做什么

`Close()` 负责清理所有资源：

```cpp
void Close() {
    // 1. 写入文件尾（trailer）
    if (format_ctx_ && header_written_) {
        av_write_trailer(format_ctx_);
    }

    // 2. 关闭 AVIO
    if (format_ctx_->pb && !(oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&format_ctx_->pb);
    }

    // 3. 释放 AVFormatContext
    avformat_free_context(format_ctx_);
}
```

这是必要的，因为容器文件（如 MP4）的 trailer 包含重要的索引信息（moov box），没有它文件无法被播放器正确读取。

## 6. 时间戳重标定

编码器使用自己的 `time_base`（如 1/25），而容器可能使用不同的时间基（如 1/90000）。

Write() 中使用 `av_packet_rescale_ts()` 进行重标定：

```cpp
av_packet_rescale_ts(
    av_packet,
    AVRational{packet.time_base.num, packet.time_base.den},  // 源时间基
    video_stream_->time_base);                               // 目标时间基
```

转换关系是：

```text
目标 tick = 源 tick * 源 time_base / 目标 time_base
```

例如源为 1/25、pts=1，目标为 1/90000，则结果为 3600。

## 7. 消费式写入语义

FFmpegMuxer 采用"消费式"语义：

```text
编码器产生的 MediaPacket 已经通过 FFmpegPacketBuffer 持有 AVPacket
Muxer 不再复制 AVPacket 或其 payload
av_interleaved_write_frame() 会接管/消费这个 AVPacket 的数据引用
返回后原 packet 不可再次写入，也不能拿同一个 packet 重试
```

这意味着：

- 同一个 packet 不能被多次写入。
- 写入失败后不能重试同一个 packet。
- 调用方需要确保 packet 在写入期间保持有效。

## 8. 容器格式选择

FFmpeg 根据输出文件扩展名自动选择容器格式：

```text
.mp4   -> MP4/MOV
.mkv   -> Matroska
.flv   -> FLV
.ts    -> MPEG-TS
.m3u8  -> HLS
```

`avformat_alloc_output_context2()` 的第三个参数 `format_name` 可以强制指定格式，但通常传 `nullptr` 让 FFmpeg 自动推断即可。

## 9. 典型使用场景

### 9.1 编码后写入文件

```text
Puller -> Decoder -> Converter -> Encoder -> Muxer -> output.mp4
```

编码器输出的 packet 直接写入 muxer，muxer 负责按 MP4 格式组织。

### 9.2 网络推流

```text
Encoder -> Muxer -> RTMP/RTSP/HTTP
```

muxer 的 `output_url` 可以是网络地址（如 `rtmp://server/live/stream`），FFmpeg 会自动使用网络协议而不是文件 IO。

### 9.3 内存 Muxer

```text
Encoder -> Muxer -> 内存缓冲区
```

使用 `AVFMT_NOFILE` 标志，FFmpeg 不打开文件，而是将数据写入内存缓冲区。适合需要后续处理的场景。

## 10. 注意事项

### 10.1 Extradata 必须正确设置

H.264 的 SPS/PPS 必须在 `Open()` 时设置到 `video_stream_->codecpar->extradata`，否则播放器无法解析视频流。

### 10.2 时间戳必须连续

容器要求时间戳单调递增。如果 packet 的 pts 为 `AV_NOPTS_VALUE`，muxer 会保留该值，但某些容器（如 MP4）可能拒绝写入。

### 10.3 关键帧标志必须正确

muxer 会根据 `packet.keyframe` 设置 `AV_PKT_FLAG_KEY`。如果关键帧标志错误，播放器可能无法正确 seek。

### 10.4 Close() 必须调用

`Close()` 会写入 trailer，没有 trailer 的容器文件（尤其是 MP4）可能无法被播放器读取。即使写入过程中出错，也应该调用 `Close()` 清理资源。


## 11. 问题修复记录
