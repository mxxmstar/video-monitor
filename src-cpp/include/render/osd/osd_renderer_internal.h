#pragma once

#include <array>
#include <cstdlib>

#include "render/osd/i_osd_render.h"
#include "render/osd/osd_render_impl.h"

namespace osd::detail {
struct Plane {
    std::uint8_t* data{};
    std::size_t stride{};
};

OsdStatus PrepareFrame(MediaFrame& frame, const OverlayBatch& batch, bool planar,
                       std::array<Plane, 3>& planes);

template <typename View>
void FillRect(View& view, std::int64_t x, std::int64_t y, std::int64_t width,
              std::int64_t height, const YuvColor& color) {
    const auto right = std::min<std::int64_t>(view.width, x + width);
    const auto bottom = std::min<std::int64_t>(view.height, y + height);
    x = std::max<std::int64_t>(0, x);
    y = std::max<std::int64_t>(0, y);
    if (right > x && bottom > y) {
        FillTextRect(view, static_cast<int>(x), static_cast<int>(y),
                     static_cast<int>(right - x), static_cast<int>(bottom - y), color);
    }
}

template <typename View>
void DrawRect(View& view, const OverlayRect& rect) {
    const std::int64_t x = rect.x;
    const std::int64_t y = rect.y;
    const int horizontal = std::min(NormalizeThickness(rect.thickness), rect.height);
    const int vertical = std::min(NormalizeThickness(rect.thickness), rect.width);
    FillRect(view, x, y, rect.width, horizontal, rect.color);
    FillRect(view, x, y + rect.height - horizontal, rect.width, horizontal, rect.color);
    FillRect(view, x, y, vertical, rect.height, rect.color);
    FillRect(view, x + rect.width - vertical, y, vertical, rect.height, rect.color);
}

template <typename View>
void DrawLine(View& view, const OverlayLine& line) {
    const std::int64_t thickness = NormalizeThickness(line.thickness);
    const auto start = -(thickness / 2);
    const auto dx = std::abs(static_cast<std::int64_t>(line.x2) - line.x1);
    const auto dy = std::abs(static_cast<std::int64_t>(line.y2) - line.y1);
    const bool x_major = dx >= dy;
    const auto major = x_major ? dx : dy;
    const auto minor = x_major ? dy : dx;
    const int major_sign = x_major ? (line.x1 <= line.x2 ? 1 : -1) : (line.y1 <= line.y2 ? 1 : -1);
    const int minor_sign = x_major ? (line.y1 <= line.y2 ? 1 : -1) : (line.x1 <= line.x2 ? 1 : -1);
    const std::int64_t origin = x_major ? line.x1 : line.y1;
    const std::int64_t minor_origin = x_major ? line.y1 : line.x1;
    const std::int64_t low = -start - thickness + 1;
    const std::int64_t high = (x_major ? view.width : view.height) - 1 - start;
    const auto first = std::max<std::int64_t>(0, major_sign > 0 ? low - origin : origin - high);
    const auto last = std::min(major, major_sign > 0 ? high - origin : origin - low);
    // Evaluate the original Bresenham raster at visible major-axis steps, preserving
    // its rounding phase instead of starting a new raster at clipped endpoints.
    for (auto i = first; i <= last; ++i) {
        const auto offset = major == 0 ? 0 : static_cast<std::int64_t>(
            (static_cast<std::uint64_t>(i) * minor + major / 2) / major);
        const auto a = origin + major_sign * i;
        const auto b = minor_origin + minor_sign * offset;
        FillRect(view, (x_major ? a : b) + start, (x_major ? b : a) + start,
                 thickness, thickness, line.color);
    }
}

template <typename View>
void DrawBatch(View& view, const OverlayBatch& batch) {
    for (const auto& item : batch.Items()) {
        switch (item->Type()) {
        case OverlayType::kRect:
            DrawRect(view, static_cast<const OverlayRect&>(*item));
            break;
        case OverlayType::kLine:
            DrawLine(view, static_cast<const OverlayLine&>(*item));
            break;
        case OverlayType::kText:
            DrawText(view, static_cast<const OverlayText&>(*item));
            break;
        }
    }
}
}  // namespace osd::detail
