#pragma once

#include "render/osd/i_osd_render.h"

namespace osd {

class I420OsdRenderer final : public IOsdRenderer {
public:
    OsdStatus Draw(MediaFrame& frame, const OverlayBatch& batch) const override;
};

}  // namespace osd
