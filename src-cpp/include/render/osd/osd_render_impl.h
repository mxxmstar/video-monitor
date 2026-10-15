#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <variant>

#include "render/osd/font_8x16.h"
#include "render/osd/osd_element.h"

constexpr int kFont8x16Width = 8;   // 每个字符宽 8 像素
constexpr int kFont8x16Height = 16; // 每个字符高 16 像素

/// @brief 文本框结构体，用于存储文本占据的宽度和高度
struct TextBounds {
    int width{0};
    int height{0};
};
inline int NormalizeThickness(int thickness) {
    return std::max(1, thickness);
}

inline int NormalizeScale(int scale) {
    return std::max(1, scale);
}

/// @brief 绘制一个填充矩形,以像素为单位
/// @tparam DrawPixel 回调函数类型
/// @param draw_pixel 回调函数，用于绘制每个像素的值
/// @param x 矩形左上角 x 坐标
/// @param y 矩形左上角 y 坐标
/// @param width 矩形宽度
/// @param height 矩形高度
template <typename DrawPixel>
void DrawFilledRectPixel(DrawPixel&& draw_pixel, int x, int y, int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }

    for (int yy = 0; yy < height; ++yy) {
        for (int xx = 0; xx < width; ++xx) {
            draw_pixel(x + xx, y + yy);
        }
    }
}

/// @brief 绘制一个填充矩形,以行为单位
/// @tparam DrawRow 回调函数类型
/// @param draw_row 回调函数，用于绘制每一行的像素值
/// @param x 矩形左上角 x 坐标
/// @param y 矩形左上角 y 坐标
/// @param width 矩形宽度
/// @param height 矩形高度
template <typename DrawRow>
void DrawFilledRectRow(DrawRow&& draw_row, int x, int y, int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }

    for (int yy = 0; yy < height; ++yy) {
        draw_row(x, y + yy, width);
    }
}

/// @brief 绘制一个带厚度的点
/// @tparam DrawPixel 回调函数类型
/// @param draw_pixel 回调函数，用于绘制每个像素的值
/// @param x 点的 x 坐标
/// @param y 点的 y 坐标
/// @param thickness 点的厚度
template <typename DrawPixel>
void DrawThickPoint(DrawPixel&& draw_pixel, int x, int y, int thickness) {
    thickness = NormalizeThickness(thickness);
    const int start = -(thickness / 2);
    DrawFilledRectPixel(draw_pixel, x + start, y + start, thickness, thickness);
}

/// @brief 绘制一条线段，使用 Bresenham 算法
/// @tparam DrawPixel 回调函数类型
/// @param draw_pixel 回调函数，用于绘制每个像素的值
/// @param x1 线段起始点 x 坐标
/// @param y1 线段起始点 y 坐标
/// @param x2 线段结束点 x 坐标
/// @param y2 线段结束点 y 坐标
/// @param thickness 线段的厚度
template <typename DrawPixel>
void DrawBresenhamLine(DrawPixel&& draw_pixel, int x1, int y1, int x2, int y2, int thickness) {
    const int dx = std::abs(x2 - x1);
    const int sx = x1 < x2 ? 1 : -1;
    const int dy = -std::abs(y2 - y1);
    const int sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;

    while (true) {
        DrawThickPoint(draw_pixel, x1, y1, thickness);
        if (x1 == x2 && y1 == y2) {
            break;
        }

        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x1 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y1 += sy;
        }
    }
}

/// @brief 测量文本的宽度和高度
/// @param text 要测量的文本
/// @param scale 文本缩放比例
/// @param char_spacing 字符间距
/// @param line_spacing 行间距
/// @return 文本的宽度和高度
inline TextBounds MeasureText8x16(const std::string& text, int scale, int char_spacing, int line_spacing) {
    if (text.empty()) {
        return {};
    }

    scale = NormalizeScale(scale);
    char_spacing = std::max(0, char_spacing);
    line_spacing = std::max(0, line_spacing);

    const int glyph_width = kFont8x16Width * scale;
    const int glyph_height = kFont8x16Height * scale;
    int max_width = 0;
    int line_width = 0;
    int lines = 1;
    bool line_has_chars = false;

    const auto append_glyph = [&]() {
        if (line_has_chars) {
            line_width += char_spacing;
        }
        line_width += glyph_width;
        line_has_chars = true;
    };

    for (const char ch : text) {
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            max_width = std::max(max_width, line_width);
            line_width = 0;
            line_has_chars = false;
            ++lines;
            continue;
        }

        for (int index = 0; index < (ch == '\t' ? 4 : 1); ++index) {
            append_glyph();
        }
    }

    max_width = std::max(max_width, line_width);
    return {max_width, lines * glyph_height + (lines - 1) * line_spacing};
}

/// @brief 绘制一个字符
/// @tparam DrawPixel 回调函数类型
/// @param draw_pixel 回调函数，用于绘制每个像素的值
/// @param ch 要绘制的字符
/// @param x 字符左上角 x 坐标
/// @param y 字符左上角 y 坐标
/// @param scale 字符缩放比例
template <typename DrawPixel>
void DrawGlyph8x16(DrawPixel&& draw_pixel, unsigned char ch, int x, int y, int scale) {
    scale = NormalizeScale(scale);
    for (int row = 0; row < kFont8x16Height; ++row) {
        const auto bits = font_8x16[ch][row];
        for (int column = 0; column < kFont8x16Width; ++column) {
            if ((bits & (0x80 >> column)) != 0) {
                DrawFilledRectPixel(draw_pixel, x + column * scale, y + row * scale, scale, scale);
            }
        }
    }
}

/// @brief 绘制一个字符,以行为单位
/// @tparam DrawRow 回调函数类型
/// @param draw_row 回调函数，用于绘制每一行的像素值
/// @param ch 要绘制的字符
/// @param x 字符左上角 x 坐标
/// @param y 字符左上角 y 坐标
/// @param scale 字符缩放比例
template <typename DrawRow>
void DrawGlyph8x16Batch(DrawRow&& draw_row, unsigned char ch, int x, int y, int scale) {
    scale = NormalizeScale(scale);
    
    for (int row = 0; row < kFont8x16Height; ++row) {
        const auto bits = font_8x16[ch][row];
        
        // 扫描连续 '1' 的段
        int segment_start = -1;
        for (int column = 0; column <= kFont8x16Width; ++column) {
            bool is_set = (column < kFont8x16Width) && ((bits & (0x80 >> column)) != 0);
            
            if (is_set && segment_start == -1) {
                segment_start = column;  // 开始新段
            } else if (!is_set && segment_start != -1) {
                // 段结束，批量绘制
                int seg_width = (column - segment_start) * scale;
                int seg_x = x + segment_start * scale;
                int seg_y = y + row * scale;
                draw_row(seg_x, seg_y, seg_width, scale);  // 一次绘制一行段
                segment_start = -1;
            }
        }
    }
}

/// @brief 绘制文本
/// @tparam DrawPixel 回调函数类型
/// @param draw_pixel 回调函数，用于绘制每个像素的值
/// @param text 要绘制的文本
/// @param x 文本左上角 x 坐标
/// @param y 文本左上角 y 坐标
/// @param scale 文本缩放比例
/// @param char_spacing 字符间距
/// @param line_spacing 行间距
template <typename DrawGlyph>
void DrawText8x16Impl(DrawGlyph&& draw_glyph, const std::string& text, int x, int y, int scale, int char_spacing, int line_spacing) {
    if (text.empty()) {
        return;
    }

    scale = NormalizeScale(scale);
    char_spacing = std::max(0, char_spacing);
    line_spacing = std::max(0, line_spacing);

    const int glyph_width = kFont8x16Width * scale;
    const int line_height = kFont8x16Height * scale + line_spacing;
    int cursor_x = x;
    int cursor_y = y;
    bool line_has_chars = false;

    const auto draw_character = [&](unsigned char ch) {
        if (line_has_chars) {
            cursor_x += char_spacing;
        }
        draw_glyph(ch, cursor_x, cursor_y, scale);
        cursor_x += glyph_width;
        line_has_chars = true;
    };

    for (const char ch : text) {
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            cursor_x = x;
            cursor_y += line_height;
            line_has_chars = false;
        } else if (ch == '\t') {
            for (int index = 0; index < 4; ++index) {
                draw_character(' ');
            }
        } else {
            draw_character(static_cast<unsigned char>(ch));
        }
    }
}

template <typename DrawPixel>
void DrawText8x16(DrawPixel&& draw_pixel, const std::string& text, int x, int y, int scale, int char_spacing, int line_spacing) {
    DrawText8x16Impl([&](unsigned char ch, int gx, int gy, int normalized_scale) {
        DrawGlyph8x16(draw_pixel, ch, gx, gy, normalized_scale);
    }, text, x, y, scale, char_spacing, line_spacing);
}

/// @brief 绘制文本,以矩形为单位
/// @tparam DrawRect 回调函数类型
/// @param draw_rect 回调函数，用于绘制每个矩形的像素值
/// @param text 要绘制的文本
/// @param x 文本左上角 x 坐标
/// @param y 文本左上角 y 坐标
/// @param scale 文本缩放比例
/// @param char_spacing 字符间距
/// @param line_spacing 行间距
template <typename DrawRect>
void DrawText8x16Batch(DrawRect&& draw_rect, const std::string& text, int x, int y, int scale, int char_spacing, int line_spacing) {
    DrawText8x16Impl([&](unsigned char ch, int gx, int gy, int normalized_scale) {
        DrawGlyph8x16Batch(draw_rect, ch, gx, gy, normalized_scale);
    }, text, x, y, scale, char_spacing, line_spacing);
}

struct Nv12View {
    std::uint8_t* y{nullptr};
    std::uint8_t* uv{nullptr};
    int width{0};
    int height{0};
    std::size_t y_stride{0};
    std::size_t uv_stride{0};
    bool is_nv21{false};
};

struct I420View {
    std::uint8_t* y{nullptr};
    std::uint8_t* u{nullptr};
    std::uint8_t* v{nullptr};
    int width{0};
    int height{0};
    std::size_t y_stride{0};
    std::size_t u_stride{0};
    std::size_t v_stride{0};
};

/// @brief 在NV12/NV21格式的图像上绘制文本矩形
/// @param view NV12/NV21格式的图像视图
/// @param x 文本左上角 x 坐标
/// @param y 文本左上角 y 坐标
/// @param width 文本宽度
/// @param height 文本高度
/// @param color 文本颜色
inline void FillTextRect(Nv12View& view, int x, int y, int width, int height, const YuvColor& color) {
    if (!view.y || !view.uv || width <= 0 || height <= 0) {
        return;
    }
    const int left = std::max(0, x);
    const int top = std::max(0, y);
    const int right = static_cast<int>(std::min<std::int64_t>(view.width, static_cast<std::int64_t>(x) + width));
    const int bottom = static_cast<int>(std::min<std::int64_t>(view.height, static_cast<std::int64_t>(y) + height));
    if (left >= right || top >= bottom) {
        return;
    }
    for (int row = top; row < bottom; ++row) {
        std::memset(view.y + static_cast<std::size_t>(row) * view.y_stride + left, color.y, right - left);
    }

    // Include every chroma sample touched by a luma pixel, including odd edges.
    const int uv_left = left / 2;
    const int uv_right = (right - 1) / 2 + 1;
    const int uv_bottom = (bottom - 1) / 2 + 1;
    const auto first = view.is_nv21 ? color.v : color.u;
    const auto second = view.is_nv21 ? color.u : color.v;
    for (int row = top / 2; row < uv_bottom; ++row) {
        auto* dst = view.uv + static_cast<std::size_t>(row) * view.uv_stride;
        for (int column = uv_left; column < uv_right; ++column) {
            dst[static_cast<std::size_t>(column) * 2] = first;
            dst[static_cast<std::size_t>(column) * 2 + 1] = second;
        }
    }
}

/// @brief 在I420格式的图像上绘制文本矩形
/// @param view I420格式的图像视图
/// @param x 文本左上角 x 坐标
/// @param y 文本左上角 y 坐标
/// @param width 文本宽度
/// @param height 文本高度
/// @param color 文本颜色
inline void FillTextRect(I420View& view, int x, int y, int width, int height, const YuvColor& color) {
    if (!view.y || !view.u || !view.v || width <= 0 || height <= 0) {
        return;
    }
    const int left = std::max(0, x);
    const int top = std::max(0, y);
    const int right = static_cast<int>(std::min<std::int64_t>(view.width, static_cast<std::int64_t>(x) + width));
    const int bottom = static_cast<int>(std::min<std::int64_t>(view.height, static_cast<std::int64_t>(y) + height));
    if (left >= right || top >= bottom) {
        return;
    }
    for (int row = top; row < bottom; ++row) {
        std::memset(view.y + static_cast<std::size_t>(row) * view.y_stride + left, color.y, right - left);
    }
    const int uv_left = left / 2;
    const int uv_width = (right - 1) / 2 + 1 - uv_left;
    const int uv_bottom = (bottom - 1) / 2 + 1;
    for (int row = top / 2; row < uv_bottom; ++row) {
        std::memset(view.u + static_cast<std::size_t>(row) * view.u_stride + uv_left, color.u, uv_width);
        std::memset(view.v + static_cast<std::size_t>(row) * view.v_stride + uv_left, color.v, uv_width);
    }
}

/// @brief 在图像上绘制文本
/// @param view 图像视图
/// @param text 要绘制的文本
/// @param x 文本左上角 x 坐标
/// @param y 文本左上角 y 坐标
/// @param scale 文本缩放比例
/// @param char_spacing 字符间距
/// @param line_spacing 行间距
template <typename View>
void DrawText(View& view, const OverlayText& text) {
    if (text.text.empty()) {
        return;
    }
    if (text.draw_background) {
        const auto bounds = MeasureText8x16(text.text, text.scale, text.char_spacing, text.line_spacing);
        const int padding = std::max(0, text.background_padding);
        FillTextRect(view, text.x - padding, text.y - padding,
                     bounds.width + padding * 2, bounds.height + padding * 2, text.background_color);
    }
    DrawText8x16Batch([&](int x, int y, int width, int height) {
        FillTextRect(view, x, y, width, height, text.color);
    }, text.text, text.x, text.y, text.scale, text.char_spacing, text.line_spacing);
}
