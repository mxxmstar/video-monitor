#include "media/ffmpeg_raw_frame_buffer.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

namespace {

///@brief 获取 FFmpeg 根据格式计算出的平面数量。
int GetPlaneCount(const AVFrame* frame) {
    if (!frame) {
        return 0;
    }

    if (frame->width > 0 && frame->height > 0) {
        const int count = av_pix_fmt_count_planes(static_cast<AVPixelFormat>(frame->format));
        return count > 0 ? count : 0;
    }

    if (frame->nb_samples > 0 && frame->ch_layout.nb_channels > 0) {
        const auto format = static_cast<AVSampleFormat>(frame->format);
        return av_sample_fmt_is_planar(format) ? frame->ch_layout.nb_channels : 1;
    }

    return 0;
}

///@brief 获取指定平面数据。plane 从 0 开始。
const uint8_t* GetPlaneData(const AVFrame* frame, int plane) {
    if (!frame || plane < 0 || plane >= GetPlaneCount(frame)) {
        return nullptr;
    }

    // 音频声道数可能超过 AV_NUM_DATA_POINTERS，此时必须使用
    // extended_data；视频通常使用 data，但 extended_data 也同样有效。
    if (frame->extended_data) {
        return frame->extended_data[plane];
    }
    return plane < AV_NUM_DATA_POINTERS ? frame->data[plane] : nullptr;
}

}  // namespace

FFmpegRawFrameBuffer::FFmpegRawFrameBuffer(AVFrame* frame)
    : frame_(frame) {}

FFmpegRawFrameBuffer::~FFmpegRawFrameBuffer() {
    if (frame_) {
        av_frame_free(&frame_);
    }
}

uint8_t* FFmpegRawFrameBuffer::Data() {
    return nullptr;
}

const uint8_t* FFmpegRawFrameBuffer::Data() const {
    return nullptr;
}

size_t FFmpegRawFrameBuffer::Size() const {
    return 0;
}

uint8_t* FFmpegRawFrameBuffer::PlaneData(int plane) {
    return const_cast<uint8_t*>(
        static_cast<const FFmpegRawFrameBuffer*>(this)->PlaneData(plane));
}

const uint8_t* FFmpegRawFrameBuffer::PlaneData(int plane) const {
    return GetPlaneData(frame_, plane);
}

int FFmpegRawFrameBuffer::PlaneCount() const {
    return GetPlaneCount(frame_);
}
