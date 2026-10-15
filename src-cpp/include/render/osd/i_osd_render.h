#pragma once

#include "media/media_frame.h"
#include "render/osd/osd_batch.h"


enum class OsdStatus {
    kOk,    ///< 绘制成功
    kUnsupportedFormat, ///< 不支持的格式
    kInvalidFrame,      ///< 无效的帧
    kInvalidPlaneLayout, ///< 无效的平面布局
    kBufferTooSmall,     ///< 缓冲区太小
    kInvalidElement,     ///< 无效的元素
    kNotWritable,       ///< 无法获取可写的帧缓冲区
};

class IOsdRenderer {
public:
    virtual ~IOsdRenderer() = default;

    // Rect borders are drawn inward; nonpositive thickness/scale normalize to 1.
    // Planes require positive strides. Invalid batches are rejected before drawing.
    virtual OsdStatus Draw(MediaFrame& frame, const osd::OverlayBatch& batch) const = 0;
};

OsdStatus DrawOverlays(MediaFrame& frame, const osd::OverlayBatch& batch);
