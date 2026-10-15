#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "render/osd/osd_element.h"

namespace osd {

class OverlayBatch {
public:
    void Add(std::shared_ptr<OverlayElement> item) {
        items_.push_back(std::move(item));
    }

    void Clear() { items_.clear(); }
    bool Empty() const noexcept { return items_.empty(); }
    const std::vector<std::shared_ptr<OverlayElement>>& Items() const noexcept {
        return items_;
    }

private:
    std::vector<std::shared_ptr<OverlayElement>> items_;
};

}  // namespace osd
