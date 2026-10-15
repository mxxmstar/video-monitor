#pragma once

#include "render/osd/i_osd_render.h"

namespace osd {

class Nv12OsdRenderer final : public IOsdRenderer {
public:
    // Handles both NV12 (UV) and NV21 (VU).
    OsdStatus Draw(MediaFrame& frame, const OverlayBatch& batch) const override;
};

}  // namespace osd
