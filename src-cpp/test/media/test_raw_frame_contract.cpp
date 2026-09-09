/// @file test_raw_frame_contract.cpp
/// @brief 验证 raw AVFrame 的所有权、平面访问和引用生命周期契约。

#include "media/ffmpeg_raw_frame_buffer.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "raw frame contract: " << message << std::endl;
    }
    return condition;
}

AVFrame* MakeVideoFrame(AVPixelFormat format, int width, int height) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        return nullptr;
    }
    frame->format = format;
    frame->width = width;
    frame->height = height;
    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }
    for (int plane = 0; plane < 4; ++plane) {
        if (frame->data[plane]) {
            frame->data[plane][0] = static_cast<std::uint8_t>(0x20 + plane);
        }
    }
    return frame;
}

AVFrame* MakeAudioFrame(AVSampleFormat format, int channels, int samples) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        return nullptr;
    }
    frame->format = format;
    frame->sample_rate = 48000;
    frame->nb_samples = samples;
    av_channel_layout_default(&frame->ch_layout, channels);
    if (av_frame_get_buffer(frame, 0) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }
    return frame;
}

bool CheckVideo(AVPixelFormat format, int expected_planes) {
    std::shared_ptr<FFmpegRawFrameBuffer> raw(
        new FFmpegRawFrameBuffer(MakeVideoFrame(format, 16, 8)));
    if (!Check(raw->GetFrame() != nullptr, "video frame was allocated") ||
        !Check(raw->IsValid(), "video frame is valid") ||
        !Check(!raw->IsContiguous(), "video raw frame is non-contiguous") ||
        !Check(raw->Data() == nullptr && raw->Size() == 0,
               "raw Data/Size contract") ||
        !Check(raw->PlaneCount() == expected_planes, "video plane count") ) {
        return false;
    }

    for (int plane = 0; plane < expected_planes; ++plane) {
        if (!Check(raw->PlaneData(plane) != nullptr, "video plane address") ||
            !Check(raw->PlaneStride(plane) > 0, "video plane stride") ||
            !Check(raw->PlaneSize(plane) > 0, "video plane size")) {
            return false;
        }
    }
    if (!Check(raw->PlaneData(-1) == nullptr && raw->PlaneData(expected_planes) == nullptr,
               "invalid video plane rejected")) {
        return false;
    }

    AVFrame* referenced = av_frame_alloc();
    if (!Check(referenced != nullptr, "reference frame allocated") ||
        !Check(av_frame_ref(referenced, raw->GetFrame()) == 0,
               "AVFrame reference succeeds") ||
        !Check(referenced->data[0] == raw->GetFrame()->data[0],
               "reference shares video payload")) {
        av_frame_free(&referenced);
        return false;
    }

    raw.reset();
    const bool payload_survives_owner_release =
        referenced->data[0] != nullptr && referenced->data[0][0] == 0x20;
    av_frame_free(&referenced);
    return Check(payload_survives_owner_release, "av_frame_ref survives owner release");
}

bool CheckAudio() {
    std::shared_ptr<FFmpegRawFrameBuffer> raw(
        new FFmpegRawFrameBuffer(MakeAudioFrame(AV_SAMPLE_FMT_FLTP, 2, 32)));
    if (!Check(raw->IsValid(), "audio frame is valid") ||
        !Check(raw->PlaneCount() == 2, "audio plane count") ||
        !Check(raw->PlaneSize(0) == 32 * sizeof(float), "audio plane size") ||
        !Check(raw->PlaneSize(1) == 32 * sizeof(float), "audio plane size for channel 2")) {
        return false;
    }
    return Check(raw->PlaneData(0) != nullptr && raw->PlaneData(1) != nullptr,
                 "audio plane addresses");
}

}  // namespace

int main() {
    const bool passed = CheckVideo(AV_PIX_FMT_YUV420P, 3) &&
                        CheckVideo(AV_PIX_FMT_NV12, 2) &&
                        CheckAudio();
    if (passed) {
        std::cout << "raw AVFrame contract test passed" << std::endl;
        return 0;
    }
    return 1;
}
