#include "render/osd/i420_osd_renderer.h"
#include "render/osd/osd_renderer_internal.h"

namespace osd {
OsdStatus I420OsdRenderer::Draw(MediaFrame& frame, const OverlayBatch& batch) const {
    std::array<detail::Plane, 3> planes{};
    const auto status = detail::PrepareFrame(frame, batch, true, planes);
    if (status != OsdStatus::kOk || batch.Empty()) {
        return status;
    }
    I420View view{planes[0].data, planes[1].data, planes[2].data, frame.Width(), frame.Height(),
                  planes[0].stride, planes[1].stride, planes[2].stride};
    detail::DrawBatch(view, batch);
    return OsdStatus::kOk;
}
}  // namespace osd
