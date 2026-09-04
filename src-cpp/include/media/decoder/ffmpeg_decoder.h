#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "media/decoder/i_decoder.h"

#include <mutex>


/// @brief FFmpeg 软件解码器
class FFmpegDecoder : public IDecoder {
public:
    FFmpegDecoder() = default;
    ~FFmpegDecoder() override;

    // 禁用复制构造函数和赋值运算符
    FFmpegDecoder(const FFmpegDecoder&) = delete;
    FFmpegDecoder& operator=(const FFmpegDecoder&) = delete;


    bool Open(const MediaStreamInfo& info) override;
    bool Flush() override;
    void Close() override;
    bool Decode(std::shared_ptr<MediaPacket> packet) override;
    void SetFrameCallback(FrameCallback cb) override;

private:
    /// @brief 接收所有已解码帧并回调
    bool receiveFrames();

    AVCodecContext* codec_ctx_{nullptr};  ///< FFmpeg 解码器上下文
    MediaStreamInfo      stream_info_;          ///< 解码器打开的流信息
    FrameCallback   frame_cb_;             ///< 解码帧回调
    std::mutex      cb_mutex_;             ///< 保护 frame_cb_ setter
};