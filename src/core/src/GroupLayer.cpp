#include "pe/core/GroupLayer.hpp"

#include "pe/core/Compositor.hpp"

#include <algorithm>

namespace pe {

GroupLayer::GroupLayer(std::string name) : Layer(LayerKind::Group, std::move(name)) {}

void GroupLayer::addChild(std::unique_ptr<Layer> child) {
    if (child) children_.push_back(std::move(child));
}

void GroupLayer::insertChild(std::size_t index, std::unique_ptr<Layer> child) {
    if (!child) return;
    index = std::min(index, children_.size());
    children_.insert(children_.begin() + static_cast<std::ptrdiff_t>(index), std::move(child));
}

std::unique_ptr<Layer> GroupLayer::removeChild(LayerId id) {
    const std::size_t idx = indexOf(id);
    if (idx == npos) return nullptr;
    std::unique_ptr<Layer> taken = std::move(children_[idx]);
    children_.erase(children_.begin() + static_cast<std::ptrdiff_t>(idx));
    return taken;
}

std::size_t GroupLayer::indexOf(LayerId id) const noexcept {
    for (std::size_t i = 0; i < children_.size(); ++i) {
        if (children_[i] && children_[i]->id() == id) return i;
    }
    return npos;
}

Layer* GroupLayer::findChild(LayerId id) noexcept {
    const std::size_t idx = indexOf(id);
    return idx == npos ? nullptr : children_[idx].get();
}

Layer* GroupLayer::findDescendant(LayerId id) noexcept {
    for (const auto& child : children_) {
        if (!child) continue;
        if (child->id() == id) return child.get();
        if (child->kind() == LayerKind::Group) {
            if (Layer* found = static_cast<GroupLayer*>(child.get())->findDescendant(id)) {
                return found;
            }
        }
    }
    return nullptr;
}

const Layer* GroupLayer::findDescendant(LayerId id) const noexcept {
    return const_cast<GroupLayer*>(this)->findDescendant(id);
}

Rect GroupLayer::contentBounds() const noexcept {
    Rect bounds{};
    for (const auto& child : children_) {
        if (child) bounds = bounds.united(child->contentBounds());
    }
    return bounds;
}

void GroupLayer::renderInto(TileCoord coord, std::span<Rgbaf> dst) const {
    // Composite children over transparency (isolated group). The compositor's
    // own loop normally handles groups with depth tracking; this path supports
    // rendering a group standalone.
    for (auto& p : dst) p = Rgbaf{};
    compositeStack(children_, coord, dst, 0);
}

std::unique_ptr<Layer> GroupLayer::clone() const {
    auto copy = std::make_unique<GroupLayer>(name());
    copyPropsTo(*copy);
    copy->isolated_ = isolated_;
    copy->children_.reserve(children_.size());
    for (const auto& child : children_) {
        if (child) copy->children_.push_back(child->clone());
    }
    return copy;
}

}  // namespace pe
