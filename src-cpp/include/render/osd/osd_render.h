#pragma once

#include <concepts>

#include "render/osd/i420_osd_renderer.h"
#include "render/osd/nv12_osd_renderer.h"
#include "render/osd/osd_batch.h"
#include "render/osd/osd_element.h"

/// @brief 在图像上绘制OSD
/// @tparam Renderer OSD渲染器类型，必须是IOsdRenderer的派生类，默认可初始化
/// @param frame 图像帧
/// @param batch OSD覆盖批次
/// @return OSD渲染状态
template <typename Renderer>
    requires std::derived_from<Renderer, IOsdRenderer> && std::default_initializable<Renderer>
OsdStatus DrawOverlays(MediaFrame& frame, const osd::OverlayBatch& batch) {
    return Renderer{}.Draw(frame, batch);
}
