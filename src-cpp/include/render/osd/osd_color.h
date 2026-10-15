#pragma once

#include <cstdint>

struct YuvColor {
    std::uint8_t y{255};
    std::uint8_t u{128};
    std::uint8_t v{128};

    constexpr YuvColor() = default;
    constexpr YuvColor(std::uint8_t y_value, std::uint8_t u_value, std::uint8_t v_value)
        : y(y_value), u(u_value), v(v_value) {}
};

namespace OsdColors {
namespace Yuv {
inline constexpr YuvColor kBlack{16, 128, 128};
inline constexpr YuvColor kWhite{235, 128, 128};
inline constexpr YuvColor kRed{76, 84, 255};
inline constexpr YuvColor kGreen{149, 43, 21};
inline constexpr YuvColor kBlue{29, 255, 107};
}  // namespace Yuv
}  // namespace OsdColors
