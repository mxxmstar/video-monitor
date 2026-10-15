#pragma once

#include <string>

#include "render/osd/osd_color.h"

enum class OverlayType {
    kRect,  // 矩形
    kLine,  // 线
    kText,  // 文本
};

class OverlayElement {
public:
    virtual ~OverlayElement() = default;

    OverlayType Type() const noexcept { return type_; }

    YuvColor color{OsdColors::Yuv::kWhite};
    int thickness{2};

protected:
    explicit OverlayElement(OverlayType type) : type_(type) {}

private:
    OverlayType type_;
};

/// @brief 矩形叠加元素
class OverlayRect final : public OverlayElement {
public:
    OverlayRect() : OverlayElement(OverlayType::kRect) {}

    int x{0};    ///< 矩形左上角x坐标
    int y{0};    ///< 矩形左上角y坐标
    int width{0};    ///< 矩形宽度
    int height{0};   ///< 矩形高度
};

/// @brief 线叠加元素
class OverlayLine final : public OverlayElement {
public:
    OverlayLine() : OverlayElement(OverlayType::kLine) {}

    int x1{0};    ///< 线起点x坐标
    int y1{0};    ///< 线起点y坐标
    int x2{0};    ///< 线终点x坐标
    int y2{0};    ///< 线终点y坐标
};

/// @brief 文本叠加元素
class OverlayText final : public OverlayElement {
public:
    OverlayText() : OverlayElement(OverlayType::kText) {}

    int x{0};    ///< 文本左上角x坐标
    int y{0};    ///< 文本左上角y坐标
    int scale{1};    ///< 文本缩放比例
    int char_spacing{0};   ///< 字符间距
    int line_spacing{0};   ///< 行间距
    bool draw_background{false};    ///< 是否绘制背景
    int background_padding{1};    ///< 背景内边距
    YuvColor background_color{OsdColors::Yuv::kBlack};    ///< 背景颜色
    std::string text;    ///< 文本内容
};
