#include "render/osd/nv12_osd_renderer.h"
#include "render/osd/osd_renderer_internal.h"

namespace osd {
OsdStatus Nv12OsdRenderer::Draw(MediaFrame& frame, const OverlayBatch& batch) const {
    std::array<detail::Plane, 3> planes{};
    const auto status = detail::PrepareFrame(frame, batch, false, planes);
    if (status != OsdStatus::kOk || batch.Empty()) {
        return status;
    }
    Nv12View view{planes[0].data, planes[1].data, frame.Width(), frame.Height(),
                  planes[0].stride, planes[1].stride, frame.PixelFormat() == PixelFormat::kNV21};
    detail::DrawBatch(view, batch);
    return OsdStatus::kOk;
}
}  // namespace osd
