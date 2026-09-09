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

bool IsValidFrame(const AVFrame* frame) {
    const int plane_count = GetPlaneCount(frame);
    if (!frame || plane_count <= 0 || plane_count > 8) {
        return false;
    }

    if (frame->width > 0 && frame->height > 0) {
        if (frame->format < 0) {
            return false;
        }
        for (int plane = 0; plane < plane_count; ++plane) {
            const uint8_t* data = frame->extended_data
                ? frame->extended_data[plane]
                : (plane < AV_NUM_DATA_POINTERS ? frame->data[plane] : nullptr);
            if (!data) {
                return false;
            }
        }
        return true;
    }

    if (frame->nb_samples > 0 && frame->ch_layout.nb_channels > 0) {
        if (frame->format < 0 || !frame->extended_data) {
            return false;
        }
        for (int plane = 0; plane < plane_count; ++plane) {
            if (!frame->extended_data[plane]) {
                return false;
            }
        }
        return true;
    }

    return false;
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

bool FFmpegRawFrameBuffer::IsValid() const {
    return IsValidFrame(frame_);
}

int FFmpegRawFrameBuffer::PlaneStride(int plane) const {
    if (!frame_ || plane < 0 || plane >= GetPlaneCount(frame_) ||
        plane >= AV_NUM_DATA_POINTERS) {
        return 0;
    }
    return frame_->linesize[plane];
}

size_t FFmpegRawFrameBuffer::PlaneSize(int plane) const {
    if (!frame_ || plane < 0 || plane >= GetPlaneCount(frame_)) {
        return 0;
    }

    if (frame_->width > 0 && frame_->height > 0) {
        size_t plane_sizes[4]{};
        ptrdiff_t linesizes[4]{};
        for (int index = 0; index < 4; ++index) {
            linesizes[index] = frame_->linesize[index];
        }
        if (plane >= 4 || av_image_fill_plane_sizes(
                plane_sizes,
                static_cast<AVPixelFormat>(frame_->format),
                frame_->height,
                linesizes) < 0) {
            return 0;
        }
        return plane_sizes[plane];
    }

    if (frame_->nb_samples > 0 && frame_->ch_layout.nb_channels > 0) {
        const auto sample_format = static_cast<AVSampleFormat>(frame_->format);
        const int bytes_per_sample = av_get_bytes_per_sample(sample_format);
        if (bytes_per_sample <= 0) {
            return 0;
        }
        const size_t one_plane_size = static_cast<size_t>(frame_->nb_samples) *
            static_cast<size_t>(bytes_per_sample);
        return av_sample_fmt_is_planar(sample_format)
            ? one_plane_size
            : one_plane_size * static_cast<size_t>(frame_->ch_layout.nb_channels);
    }

    return 0;
}
