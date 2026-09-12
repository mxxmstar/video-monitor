# Encoder 模块学习说明

## 1. 为什么需要 encoder

decoder 输出的是已经解码的音视频帧，encoder 负责将这些帧重新编码为压缩的编码数据包。

encoder 模块的职责是：

```text
输入 MediaFrame -> 编码 -> 输出 MediaPacket 列表
```

它不关心帧从哪里来（decoder、converter 或其他来源），只负责：

- 选择合适的编码器（libx264、libx265、AAC 等）。
- 配置编码器参数（分辨率、帧率、码率、preset 等）。
- 将 MediaFrame 送入编码器，接收编码后的 MediaPacket。
- 处理编码器的 flush 操作，确保所有缓存帧输出。

## 2. 当前公共接口

当前对外使用统一的 `MediaFrame -> MediaPacket 列表` 接口：

```cpp
FFmpegEncoder encoder;

EncoderConfig config;
config.media_type = MediaType::VIDEO;
config.codec_type = CodecType::H264;
config.bitrate = 2'000'000;
config.video().width = 1920;
config.video().height = 1080;
config.video().fps_num = 25;
config.video().fps_den = 1;
config.video().pixel_format = PixelFormat::kI420;

if (!encoder.Open(config)) {
    // 打开失败
}

std::vector<PacketPtr> packets;
if (!encoder.Encode(frame, packets)) {
    // 编码失败
}
```

`Open()` 决定编码器参数和使用的后端，`Encode()` 接收一帧并输出零个或多个 packet，`Flush()` 清空编码器缓存，`Close()` 释放编码器上下文。

当前只实现了：

```text
FFmpegEncoder（基于 libavcodec）
```

## 3. IEncoder 抽象接口

`IEncoder` 是编码器的抽象基类，定义了所有编码器必须实现的接口：

```cpp
class IEncoder {
public:
    virtual ~IEncoder() = default;
    virtual bool Open(const EncoderConfig& cfg) = 0;
    virtual bool Encode(FramePtr frame, std::vector<PacketPtr>& packets) = 0;
    virtual bool Flush(std::vector<PacketPtr>& packets) = 0;
    virtual EncodedTrackInfo GetOutputInfo() const = 0;
    virtual void Close() = 0;
};
```

这种设计允许后续增加其他编码器实现（如硬件编码器、第三方编码器），而不改变调用方的代码。

## 4. MediaFrame 到编码器的数据流

编码器接收的是已经解码的 `MediaFrame`，需要将其转换为 FFmpeg 的 `AVFrame` 才能送入编码器。

```text
MediaFrame
    -> MediaFrameToAVFrame()
    -> AVFrame
    -> avcodec_send_frame()
    -> 编码器处理
    -> avcodec_receive_packet()
    -> MediaPacket
```

`MediaFrameToAVFrame()` 由 `MediaFrameConverter` 提供，负责：

1. 从 `MediaFrame.meta` 读取格式参数。
2. 设置 AVFrame 的格式、宽高、采样率等。
3. 从 `MediaFrame.buffer` 复制数据到 AVFrame 的数据平面。
4. 设置时间戳。

## 5. 编码器配置

### 5.1 EncoderConfig 结构

`EncoderConfig` 是编码器的统一配置结构，包含：

- `media_type`：媒体类型（视频/音频）。
- `codec_type`：编码格式（H264/H265/AAC/OPUS）。
- `bitrate`：目标码率。
- `specific`：视频或音频的独有配置（使用 `std::variant`）。
- `encoder_name`：指定的 FFmpeg 编码器名称（如 "libx264"），为空则自动选择。
- `global_header`：是否在 extradata 中存储全局头信息。
- `thread_count`：编码线程数。

### 5.2 VideoEncoderConfig

视频编码的独有配置：

```text
width / height        分辨率
fps_num / fps_den     帧率
pixel_format          输入像素格式
gop_size              GOP 大小（关键帧间隔）
max_b_frames          最大 B 帧数
preset                编码器 preset（如 ultrafast、medium、slow）
tune                  编码器 tune 参数（如 zerolatency）
crf                   CRF 质量控制值
```

### 5.3 AudioEncoderConfig

音频编码的独有配置：

```text
sample_rate           采样率
channels              通道数
channel_layout        通道布局
sample_format         输入采样格式
```

## 6. FFmpegEncoder 内部实现

### 6.1 Open() 流程

```text
1. 调用 Close() 清理旧状态
2. 校验 EncoderConfig 有效性
3. 根据 codec_type 查找 FFmpeg 编码器
4. 检查编码器是否支持输入的像素/采样格式
5. 创建 AVCodecContext
6. 配置编码器参数（码率、GOP、B 帧、preset 等）
7. 调用 avcodec_open2() 打开编码器
8. 创建并打开 MediaFrameConverter 用于格式转换
```

### 6.2 Encode() 流程

```text
1. 将 MediaFrame 转换为 AVFrame（通过 MediaFrameConverter）
2. 设置 AVFrame 的时间戳
3. 调用 avcodec_send_frame() 发送帧到编码器
4. 循环调用 avcodec_receive_packet() 接收所有已编码 packet
5. 将 AVPacket 封装为 MediaPacket 并输出
```

### 6.3 receivePackets() 做什么

编码器内部可能缓存多帧（尤其是 B 帧），因此一次 `Encode()` 可能输出零个或多个 packet。

`receivePackets()` 循环调用 `avcodec_receive_packet()`，直到返回 `AVERROR(EAGAIN)` 或 `AVERROR_EOF`。

每个接收到的 AVPacket 会被封装为 MediaPacket：

```text
MediaPacket.buffer      持有编码数据的 SimpleBuffer
MediaPacket.meta        编码数据的元数据（codec、pts、dts、size 等）
MediaPacket.is_keyframe 是否关键帧
```

### 6.4 Flush() 做什么

`Flush()` 向编码器发送 `nullptr`，触发编码器输出所有缓存帧：

```cpp
bool Flush(std::vector<PacketPtr>& packets) {
    return Encode(nullptr, packets);
}
```

这是必要的，因为编码器内部可能还有未输出的帧（尤其是 B 帧）。

## 7. 时间戳处理

编码器使用自己的 `time_base`，而 `MediaFrame.time` 使用微秒单位。

当前实现：

```text
MediaFrame.time.pts_us -> AVFrame.pts（直接赋值）
编码器根据 time_base 换算 packet 的 pts/dts
```

如果帧的 pts 为 0，编码器会自动分配递增的 pts：

```cpp
int64_t resolveFramePts(const MediaFrame& frame);
```

### 7.1 已知问题：编码输出时间戳非单调递增导致 muxer 失败

**现象**：在 `test_ffmpeg_puller_decoder_converter_encoder` 中，编码若干帧后 muxer 报错并停止写出：

```text
[mp4] Application provided invalid, non monotonically increasing dts to muxer in stream 0: 1024 >= 1024
[ffmpeg_muxer.cpp] av_interleaved_write_frame failed: Invalid argument
```

**根因**：`resolveFramePts()` 本应保证输出时间戳严格递增，但旧实现返回的是 `av_rescale_q` 取整后的原始 `pts`，而不是被 `next_pts_` 计数器修正后的值：

```cpp
// 旧实现（有 bug）
if (IsValidTimestamp(frame.time.pts_us)) {
    const int64_t pts = av_rescale_q(frame.time.pts_us, kMicrosecondTimeBase,
        codec_ctx_->time_base);
    next_pts_ = std::max(next_pts_, pts + 1);
    return pts;   // 错误地返回了会碰撞的原始值，next_pts_ 成了死代码
}
return next_pts_++;
```

当编码器 `time_base` 为帧率制（如 `1/25`）时，间隔较近的输入帧微秒时间戳会在取整后碰撞。例如解码器输出 `pts_us = 0, 66045, 99200`：

- `66045 * 25 / 1e6 ≈ 1.65 → 2`
- `99200 * 25 / 1e6 ≈ 2.48 → 2`

第 2、3 帧都得到 `pts=2`。由于 `max_b_frames=0` 时 `dts == pts`，两个包的 `dts` 也都为 `2`。muxer 将其重标定到 MP4 流时间基（`1/12800`，乘子 512）后得到 `1024`，两个包相同 → muxer 判定 DTS 非单调而报错。

**修复**：`resolveFramePts()` 真正返回单调递增的值——源时间戳有效且领先于已分配计数器则采用它，否则用 `next_pts_` 兜底递增：

```cpp
int64_t FFmpegEncoder::resolveFramePts(const MediaFrame& frame) {
    int64_t raw_pts = kNoTimestamp;
    if (IsValidTimestamp(frame.time.pts_us)) {
        raw_pts = av_rescale_q(frame.time.pts_us, kMicrosecondTimeBase,
            codec_ctx_ ? codec_ctx_->time_base : AVRational{1, 1'000'000});
    }
    int64_t pts = raw_pts;
    if (!IsValidTimestamp(pts) || pts < next_pts_) {
        pts = next_pts_;
    }
    next_pts_ = pts + 1;
    return pts;
}
```

修复后上述样例输出时间戳变为 `0, 2, 3` → 重标定后 `0, 1024, 1536`，严格递增，muxer 可正常写完。

**注意**：该修复保证编码器层 PTS/DTS 单调递增；若未来引入 B 帧（`max_b_frames > 0`），需额外确认 DTS/PTS 重排后 muxer 仍能接受（编码器通常也会自行保证 DTS 单调）。

## 8. 编码器选择

`FFmpegEncoder` 会根据 `codec_type` 和输入格式自动选择合适的编码器：

```text
H264 -> libx264（默认）
H265 -> libx265（默认）
AAC  -> aac（FFmpeg 内置）
OPUS -> libopus（需要外部库）
```

如果指定了 `encoder_name`，则使用指定的编码器。

编码器选择时会检查支持的像素/采样格式，选择第一个可用的格式。

## 9. 当前限制

当前实现用于学习和跑通最小链路，限制包括：

1. 只实现 FFmpeg 软编码器，尚未实现硬件编码（NVENC、QSV 等）。
2. 编码前必须经过 MediaFrameConverter 转换格式，暂时不是零拷贝。
3. 视频和音频编码器不能同时打开，需要分别创建编码器实例。
4. 码率控制当前只支持固定码率，尚未实现 VBR、CQP 等模式。
5. 时间戳换算由编码器内部处理，调用方不需要关心 time_base。
6. 当前支持的编码格式以 `ffmpeg_format.h` 中的映射函数为准。

## 10. 后续扩展顺序

建议按以下顺序继续学习和扩展：

1. 在单元测试中检查编码后的实际 packet 内容，而不仅是元数据。
2. 增加硬件编码器支持（NVENC、QSV、VideoToolbox）。
3. 将 encoder 接入 muxer，验证"解码 -> 转换 -> 编码 -> 封装"的完整链路。
4. 根据性能需求，再评估 AVFrame 零拷贝和 buffer 池化。
5. 增加更多码率控制模式（VBR、CQP、CRF）。