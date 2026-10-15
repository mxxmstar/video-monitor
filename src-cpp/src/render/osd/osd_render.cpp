#include "render/osd/osd_renderer_internal.h"
#include "render/osd/osd_render.h"
#include "media/ffmpeg_raw_frame_buffer.h"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace osd::detail {
namespace {
bool ValidText(const OverlayText& text) {
    if (text.text.empty()) {
        return true;
    }
    const std::int64_t limit = std::numeric_limits<int>::max();
    const std::int64_t scale = NormalizeScale(text.scale);
    const std::int64_t spacing = std::max(0, text.char_spacing);
    const std::int64_t line_height = scale * kFont8x16Height + std::max(0, text.line_spacing);
    std::int64_t width = 0;
    std::int64_t max_width = 0;
    std::int64_t height = scale * kFont8x16Height;
    if (line_height > limit) {
        return false;
    }
    for (char ch : text.text) {
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            max_width = std::max(max_width, width);
            width = 0;
            height += line_height;
        } else {
            for (int i = 0; i < (ch == '\t' ? 4 : 1); ++i) {
                width += (width ? spacing : 0) + scale * kFont8x16Width;
            }
        }
        if (width > limit || height > limit) {
            return false;
        }
    }
    max_width = std::max(max_width, width);
    const std::int64_t padding = text.draw_background ? std::max(0, text.background_padding) : 0;
    return max_width + padding * 2 <= limit && height + padding * 2 <= limit &&
        static_cast<std::int64_t>(text.x) - padding >= std::numeric_limits<int>::min() &&
        static_cast<std::int64_t>(text.y) - padding >= std::numeric_limits<int>::min() &&
        text.x + max_width + padding <= limit && text.y + height + padding <= limit;
}

bool ValidBatch(const OverlayBatch& batch) {
    for (const auto& item : batch.Items()) {
        if (!item) {
            return false;
        }
        switch (item->Type()) {
        case OverlayType::kRect: {
            const auto* rect = dynamic_cast<const OverlayRect*>(item.get());
            if (!rect || rect->width < 0 || rect->height < 0) {
                return false;
            }
            break;
        }
        case OverlayType::kLine:
            if (!dynamic_cast<const OverlayLine*>(item.get())) {
                return false;
            }
            break;
        case OverlayType::kText: {
            const auto* text = dynamic_cast<const OverlayText*>(item.get());
            if (!text || !ValidText(*text)) {
                return false;
            }
            break;
        }
        default:
            return false;
        }
    }
    return true;
}
}  // namespace

OsdStatus PrepareFrame(MediaFrame& frame, const OverlayBatch& batch, bool planar,
                       std::array<Plane, 3>& planes) {
    const auto* meta = std::get_if<VideoFrameMeta>(&frame.meta);
    if (frame.type != MediaType::VIDEO || !meta || !frame.buffer ||
        meta->width <= 0 || meta->height <= 0) {
        return OsdStatus::kInvalidFrame;
    }
    if (planar ? meta->pixel_format != PixelFormat::kI420 :
        (meta->pixel_format != PixelFormat::kNV12 && meta->pixel_format != PixelFormat::kNV21)) {
        return OsdStatus::kUnsupportedFormat;
    }
    if (!ValidBatch(batch)) {
        return OsdStatus::kInvalidElement;
    }
    const int count = planar ? 3 : 2;
    const std::int64_t chroma_width = (static_cast<std::int64_t>(meta->width) + 1) / 2;
    auto* raw = dynamic_cast<FFmpegRawFrameBuffer*>(frame.buffer.get());
    if (raw) {
        const auto* av = raw->GetFrame();
        const auto expected_format = planar ? AV_PIX_FMT_YUV420P :
            (meta->pixel_format == PixelFormat::kNV21 ? AV_PIX_FMT_NV21 : AV_PIX_FMT_NV12);
        if (!raw->IsValid() || av->width != meta->width || av->height != meta->height ||
            av->format != expected_format || raw->PlaneCount() != count) {
            return OsdStatus::kInvalidPlaneLayout;
        }
    } else if (!frame.buffer->IsContiguous() || !frame.buffer->Data()) {
        return OsdStatus::kInvalidFrame;
    } else if (meta->plane_count != count) {
        return OsdStatus::kInvalidPlaneLayout;
    }

    std::array<std::uint64_t, 3> offsets{};
    std::array<std::uint64_t, 3> ends{};
    for (int i = 0; i < count; ++i) {
        const auto row_bytes = i == 0 ? meta->width : chroma_width * (planar ? 1 : 2);
        const int rows = i == 0 ? meta->height : meta->height / 2 + meta->height % 2;
        const int stride = raw ? raw->PlaneStride(i) : meta->plane_info[i].stride;
        if (stride < row_bytes) {
            return OsdStatus::kInvalidPlaneLayout;
        }
        const auto required = static_cast<std::uint64_t>(stride) * (rows - 1) + row_bytes;
        if (required > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
            return OsdStatus::kInvalidPlaneLayout;
        }
        planes[i].stride = static_cast<std::size_t>(stride);
        if (raw) {
            const auto* storage = av_frame_get_plane_buffer(raw->GetFrame(), i);
            const auto address = reinterpret_cast<std::uintptr_t>(raw->PlaneData(i));
            const auto base = storage ? reinterpret_cast<std::uintptr_t>(storage->data) : 0;
            if (!storage || address < base || address - base > storage->size ||
                required > storage->size - (address - base)) {
                return OsdStatus::kInvalidPlaneLayout;
            }
        } else {
            const auto& info = meta->plane_info[i];
            if (info.offset < 0 || info.size < 0) {
                return OsdStatus::kInvalidPlaneLayout;
            }
            offsets[i] = static_cast<std::uint64_t>(info.offset);
            ends[i] = offsets[i] + (info.size > 0 ? info.size : required);
            if (offsets[i] > frame.buffer->Size() || ends[i] > frame.buffer->Size() ||
                (info.size > 0 && static_cast<std::uint64_t>(info.size) < required)) {
                return OsdStatus::kBufferTooSmall;
            }
            for (int j = 0; j < i; ++j) {
                if (offsets[i] < ends[j] && offsets[j] < ends[i]) {
                    return OsdStatus::kInvalidPlaneLayout;
                }
            }
            planes[i].data = frame.buffer->Data() + info.offset;
        }
    }
    if (raw && !batch.Empty()) {
        if (av_frame_make_writable(raw->GetFrame()) < 0) {
            return OsdStatus::kNotWritable;
        }
        for (int i = 0; i < count; ++i) {
            planes[i] = {raw->PlaneData(i), static_cast<std::size_t>(raw->PlaneStride(i))};
        }
    }
    return OsdStatus::kOk;
}
}  // namespace osd::detail

OsdStatus DrawOverlays(MediaFrame& frame, const osd::OverlayBatch& batch) {
    const auto* meta = std::get_if<VideoFrameMeta>(&frame.meta);
    if (frame.type != MediaType::VIDEO || !meta) {
        return OsdStatus::kInvalidFrame;
    }
    switch (meta->pixel_format) {
    case PixelFormat::kI420:
        return DrawOverlays<osd::I420OsdRenderer>(frame, batch);
    case PixelFormat::kNV12:
    case PixelFormat::kNV21:
        return DrawOverlays<osd::Nv12OsdRenderer>(frame, batch);
    default:
        return OsdStatus::kUnsupportedFormat;
    }
}
