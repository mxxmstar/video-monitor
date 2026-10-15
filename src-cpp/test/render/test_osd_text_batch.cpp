#include "render/osd/osd_render_impl.h"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void TestGlyphs() {
    std::size_t pixel_calls = 0;
    std::size_t batch_calls = 0;
    for (int scale : {0, 1, 2, 3}) {
        const int width = 8 * NormalizeScale(scale);
        const int height = 16 * NormalizeScale(scale);
        for (int ch = 0; ch < 256; ++ch) {
            std::vector<unsigned char> expected(width * height);
            auto actual = expected;
            DrawGlyph8x16([&](int x, int y) {
                expected[y * width + x] = 1;
                ++pixel_calls;
            }, static_cast<unsigned char>(ch), 0, 0, scale);
            DrawGlyph8x16Batch([&](int x, int y, int w, int h) {
                ++batch_calls;
                for (int yy = y; yy < y + h; ++yy) {
                    for (int xx = x; xx < x + w; ++xx) {
                        actual.at(yy * width + xx) = 1;
                    }
                }
            }, static_cast<unsigned char>(ch), 0, 0, scale);
            Require(actual == expected, "Glyph batch differs from pixel rendering");
        }
    }
    Require(batch_calls < pixel_calls, "Batch did not reduce callback count");
    std::cout << "Glyph callbacks: pixel=" << pixel_calls << ", batch=" << batch_calls << '\n';
}

void TestFrames() {
    constexpr int width = 65;
    constexpr int height = 49;
    constexpr int y_stride = 73;
    constexpr int uv_stride = 72;
    constexpr int u_stride = 38;
    constexpr int v_stride = 40;
    for (int format = 0; format < 3; ++format) {
        for (int scale : {-1, 1, 2, 3}) {
            for (int x : {-90, -3, 1, 60, 80}) {
                for (int y : {-70, -3, 1, 45, 60}) {
                    for (bool background : {false, true}) {
                        std::vector<std::uint8_t> luma(y_stride * height + 16, 0xCD);
                        std::vector<std::uint8_t> uv(uv_stride * ((height + 1) / 2) + 16, 0xCD);
                        std::vector<std::uint8_t> u(u_stride * ((height + 1) / 2) + 16, 0xCD);
                        std::vector<std::uint8_t> v(v_stride * ((height + 1) / 2) + 16, 0xCD);
                        auto expected_y = luma;
                        auto expected_uv = uv;
                        auto expected_u = u;
                        auto expected_v = v;
                        OverlayText text;
                        text.text = "AB\tZ\r\nW!\n";
                        text.text.push_back(static_cast<char>(255));
                        text.x = x;
                        text.y = y;
                        text.scale = scale;
                        text.char_spacing = scale == 2 ? -1 : 2;
                        text.line_spacing = scale == 2 ? -1 : 1;
                        text.draw_background = background;
                        text.background_padding = scale == 3 ? -1 : 3;
                        text.color = OsdColors::Yuv::kRed;
                        text.background_color = OsdColors::Yuv::kBlue;
                        const auto pixel = [&](int px, int py, const YuvColor& color) {
                            if (px < 0 || py < 0 || px >= width || py >= height) {
                                return;
                            }
                            expected_y[py * y_stride + px] = color.y;
                            expected_uv[(py / 2) * uv_stride + (px / 2) * 2] = format == 1 ? color.v : color.u;
                            expected_uv[(py / 2) * uv_stride + (px / 2) * 2 + 1] = format == 1 ? color.u : color.v;
                            expected_u[(py / 2) * u_stride + px / 2] = color.u;
                            expected_v[(py / 2) * v_stride + px / 2] = color.v;
                        };
                        if (background) {
                            const auto bounds = MeasureText8x16(text.text, scale, text.char_spacing, text.line_spacing);
                            const int padding = std::max(0, text.background_padding);
                            DrawFilledRectPixel([&](int px, int py) { pixel(px, py, text.background_color); },
                                                x - padding, y - padding,
                                                bounds.width + padding * 2, bounds.height + padding * 2);
                        }
                        DrawText8x16([&](int px, int py) { pixel(px, py, text.color); }, text.text,
                                     x, y, scale, text.char_spacing, text.line_spacing);
                        if (format == 2) {
                            I420View view{luma.data(), u.data(), v.data(), width, height, y_stride, u_stride, v_stride};
                            DrawText(view, text);
                            Require(u == expected_u && v == expected_v, "I420 chroma mismatch or padding overwritten");
                        } else {
                            Nv12View view{luma.data(), uv.data(), width, height, y_stride, uv_stride, format == 1};
                            DrawText(view, text);
                            Require(uv == expected_uv, "NV12/NV21 chroma mismatch or padding overwritten");
                        }
                        Require(luma == expected_y, "Luma mismatch or padding overwritten");
                    }
                }
            }
        }
    }
    int calls = 0;
    DrawText8x16Batch([&](int, int, int, int) { ++calls; }, "", 0, 0, 1, 0, 0);
    Require(calls == 0, "Empty text issued writes");
}
}  // namespace

int main() {
    try {
        TestGlyphs();
        TestFrames();
        std::cout << "OSD text batch tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
