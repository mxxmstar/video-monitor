#include "render/osd/osd_render.h"
#include "render/osd/osd_render_impl.h"
#include "media/ffmpeg_raw_frame_buffer.h"
#include "media/simple_buffer.h"

#include <array>
#include <climits>
#include <iostream>
#include <stdexcept>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {
using osd::OverlayBatch;

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

MediaFrame MakeFrame(PixelFormat format) {
    VideoFrameMeta meta;
    meta.pixel_format = format;
    meta.width = 35;
    meta.height = 33;
    meta.plane_count = format == PixelFormat::kI420 ? 3 : 2;
    meta.plane_info[0] = {7, 40, 40 * 33};
    meta.plane_info[1] = {7 + 40 * 33 + 5, format == PixelFormat::kI420 ? 21 : 40,
                          (format == PixelFormat::kI420 ? 21 : 40) * 17};
    meta.plane_info[2] = {meta.plane_info[1].offset + meta.plane_info[1].size + 9, 23, 23 * 17};
    const auto& last = meta.plane_info[meta.plane_count - 1];
    MediaFrame frame;
    frame.meta = meta;
    frame.buffer = std::make_shared<SimpleBuffer>(std::vector<std::uint8_t>(last.offset + last.size + 16, 0xCD));
    return frame;
}

void Pixel(MediaFrame& frame, int x, int y, const YuvColor& color) {
    if (x < 0 || y < 0 || x >= frame.Width() || y >= frame.Height()) {
        return;
    }
    auto* data = frame.buffer->Data();
    data[frame.PlaneOffset(0) + y * frame.Stride(0) + x] = color.y;
    if (frame.PixelFormat() == PixelFormat::kI420) {
        data[frame.PlaneOffset(1) + (y / 2) * frame.Stride(1) + x / 2] = color.u;
        data[frame.PlaneOffset(2) + (y / 2) * frame.Stride(2) + x / 2] = color.v;
    } else {
        const int offset = frame.PlaneOffset(1) + (y / 2) * frame.Stride(1) + (x / 2) * 2;
        const bool nv21 = frame.PixelFormat() == PixelFormat::kNV21;
        data[offset] = nv21 ? color.v : color.u;
        data[offset + 1] = nv21 ? color.u : color.v;
    }
}

void TestDrawing() {
    for (auto format : {PixelFormat::kI420, PixelFormat::kNV12, PixelFormat::kNV21}) {
        auto frame = MakeFrame(format);
        auto expected = MakeFrame(format);
        OverlayBatch batch;
        auto rect = std::make_shared<OverlayRect>();
        rect->x = -3;
        rect->y = 2;
        rect->width = 17;
        rect->height = 14;
        rect->thickness = 3;
        rect->color = OsdColors::Yuv::kRed;
        batch.Add(rect);
        for (int y = 2; y < 16; ++y) {
            for (int x = -3; x < 14; ++x) {
                if (x < 0 || x >= 11 || y < 5 || y >= 13) {
                    Pixel(expected, x, y, rect->color);
                }
            }
        }
        auto line = std::make_shared<OverlayLine>();
        line->x1 = 1;
        line->y1 = 2;
        line->x2 = 30;
        line->y2 = 27;
        line->thickness = 3;
        line->color = OsdColors::Yuv::kBlue;
        batch.Add(line);
        DrawBresenhamLine([&](int x, int y) { Pixel(expected, x, y, line->color); },
                          line->x1, line->y1, line->x2, line->y2, line->thickness);
        auto text = std::make_shared<OverlayText>();
        text->x = 3;
        text->y = 10;
        text->text = "A\tB\r\nC";
        text->scale = 2;
        text->draw_background = true;
        text->background_color = OsdColors::Yuv::kBlack;
        text->color = OsdColors::Yuv::kGreen;
        batch.Add(text);
        const auto bounds = MeasureText8x16(text->text, text->scale, 0, 0);
        DrawFilledRectPixel([&](int x, int y) { Pixel(expected, x, y, text->background_color); },
                            2, 9, bounds.width + 2, bounds.height + 2);
        DrawText8x16([&](int x, int y) { Pixel(expected, x, y, text->color); }, text->text, 3, 10, 2, 0, 0);
        Require(DrawOverlays(frame, batch) == OsdStatus::kOk, "Drawing failed");
        Require(frame.buffer->Size() == expected.buffer->Size() &&
                std::memcmp(frame.buffer->Data(), expected.buffer->Data(), frame.buffer->Size()) == 0,
                "Pixel mismatch or padding overwritten");

        batch.Clear();
        line->x1 = INT_MIN;
        line->x2 = INT_MAX;
        line->y1 = line->y2 = 0;
        line->thickness = 1;
        batch.Add(line);
        Require(DrawOverlays(frame, batch) == OsdStatus::kOk, "Extreme line failed");
        for (int x = 0; x < frame.Width(); ++x) {
            Require(frame.buffer->Data()[frame.PlaneOffset(0) + x] == line->color.y, "Clipped line mismatch");
        }
    }
}

void TestErrors() {
    auto frame = MakeFrame(PixelFormat::kI420);
    OverlayBatch batch;
    Require(DrawOverlays<osd::Nv12OsdRenderer>(frame, batch) == OsdStatus::kUnsupportedFormat, "Wrong format accepted");
    Require(DrawOverlays<osd::I420OsdRenderer>(frame, batch) == OsdStatus::kOk, "Empty batch failed");
    batch.Add(std::make_shared<OverlayRect>());
    batch.Add(nullptr);
    Require(DrawOverlays(frame, batch) == OsdStatus::kInvalidElement, "Null element accepted");
    for (std::size_t i = 0; i < frame.buffer->Size(); ++i) {
        Require(frame.buffer->Data()[i] == 0xCD, "Invalid batch modified frame");
    }
    batch.Clear();
    auto text = std::make_shared<OverlayText>();
    text->text = "A";
    text->scale = INT_MAX;
    batch.Add(text);
    Require(DrawOverlays(frame, batch) == OsdStatus::kInvalidElement, "Text overflow accepted");
    batch.Clear();
    auto& meta = std::get<VideoFrameMeta>(frame.meta);
    meta.plane_info[1].offset = meta.plane_info[0].offset;
    Require(DrawOverlays(frame, batch) == OsdStatus::kInvalidPlaneLayout, "Overlapping planes accepted");
    frame = MakeFrame(PixelFormat::kI420);
    std::get<VideoFrameMeta>(frame.meta).plane_info[1].stride = -21;
    Require(DrawOverlays(frame, batch) == OsdStatus::kInvalidPlaneLayout, "Negative stride accepted");
    frame = MakeFrame(PixelFormat::kI420);
    frame.buffer = std::make_shared<SimpleBuffer>(std::vector<std::uint8_t>(1));
    Require(DrawOverlays(frame, batch) == OsdStatus::kBufferTooSmall, "Small buffer accepted");
    frame.meta = AudioFrameMeta{};
    Require(DrawOverlays(frame, batch) == OsdStatus::kInvalidFrame, "Invalid variant accepted");
}

void TestClippedLines() {
    for (int x1 : {-27, 0, 13, 50}) {
        for (int y1 : {-12, 0, 17, 45}) {
            for (int x2 : {-15, 2, 26, 47}) {
                for (int y2 : {-8, 1, 22, 39}) {
                    for (int thickness : {0, 1, 2, 5}) {
                        auto frame = MakeFrame(PixelFormat::kI420);
                        auto expected = MakeFrame(PixelFormat::kI420);
                        auto line = std::make_shared<OverlayLine>();
                        line->x1 = x1;
                        line->y1 = y1;
                        line->x2 = x2;
                        line->y2 = y2;
                        line->thickness = thickness;
                        OverlayBatch batch;
                        batch.Add(line);
                        Require(DrawOverlays<osd::I420OsdRenderer>(frame, batch) == OsdStatus::kOk,
                                "Clipped line failed");
                        DrawBresenhamLine([&](int x, int y) { Pixel(expected, x, y, line->color); },
                                          x1, y1, x2, y2, thickness);
                        Require(std::memcmp(frame.buffer->Data(), expected.buffer->Data(), frame.buffer->Size()) == 0,
                                "Clipped line differs from original raster");
                    }
                }
            }
        }
    }
}

void TestRaw() {
    for (auto format : {PixelFormat::kI420, PixelFormat::kNV12, PixelFormat::kNV21}) {
        auto frame = MakeFrame(format);
        auto* av = av_frame_alloc();
        Require(av != nullptr, "AVFrame allocation failed");
        auto raw = std::make_shared<FFmpegRawFrameBuffer>(av);
        av->format = format == PixelFormat::kI420 ? AV_PIX_FMT_YUV420P :
            (format == PixelFormat::kNV12 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_NV21);
        av->width = frame.Width();
        av->height = frame.Height();
        Require(av_frame_get_buffer(av, 32) == 0, "Plane allocation failed");
        const int count = format == PixelFormat::kI420 ? 3 : 2;
        for (int i = 0; i < count; ++i) {
            std::memset(av->data[i], 128, av->linesize[i] * (i ? 17 : 33));
        }
        auto* reference = av_frame_clone(av);
        Require(reference != nullptr, "AVFrame clone failed");
        frame.buffer = raw;
        OverlayBatch batch;
        auto rect = std::make_shared<OverlayRect>();
        rect->width = rect->height = 8;
        rect->color = OsdColors::Yuv::kRed;
        batch.Add(rect);
        const auto status = DrawOverlays(frame, batch);
        const bool unchanged = reference->data[0][0] == 128 && reference->data[1][0] == 128;
        av_frame_free(&reference);
        Require(status == OsdStatus::kOk && unchanged, "Raw drawing modified reference frame");
        Require(raw->PlaneData(0)[0] == rect->color.y, "Raw luma mismatch");
        Require(raw->PlaneData(1)[0] == (format == PixelFormat::kNV21 ? rect->color.v : rect->color.u), "Raw chroma mismatch");
        Require(raw->PlaneData(0)[10 * raw->PlaneStride(0) + 10] == 128, "Raw background changed");
        std::get<VideoFrameMeta>(frame.meta).width += 1;
        Require(DrawOverlays(frame, batch) == OsdStatus::kInvalidPlaneLayout, "Mismatched raw dimensions accepted");
    }
}
}  // namespace

int main() {
    try {
        TestDrawing();
        TestErrors();
        TestClippedLines();
        TestRaw();
        std::cout << "OSD renderer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
