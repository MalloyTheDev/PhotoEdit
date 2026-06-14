#include "pe/core/Layer.hpp"

#include <atomic>

namespace pe {

LayerId nextLayerId() noexcept {
    static std::atomic<LayerId> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

const char* layerKindName(LayerKind k) noexcept {
    switch (k) {
        case LayerKind::Pixel:
            return "Pixel";
        case LayerKind::Background:
            return "Background";
        case LayerKind::Text:
            return "Text";
        case LayerKind::Shape:
            return "Shape";
        case LayerKind::Fill:
            return "Fill";
        case LayerKind::Adjustment:
            return "Adjustment";
        case LayerKind::SmartObject:
            return "Smart Object";
        case LayerKind::Group:
            return "Group";
        case LayerKind::Artboard:
            return "Artboard";
        case LayerKind::Video:
            return "Video";
        case LayerKind::Generative:
            return "Generative";
    }
    return "?";
}

Layer::Layer(LayerKind kind, std::string name)
    : id_(nextLayerId()), kind_(kind), name_(std::move(name)) {}

void Layer::copyPropsTo(Layer& dst) const {
    dst.name_ = name_;
    dst.visible_ = visible_;
    dst.locks_ = locks_;
    dst.opacity_ = opacity_;
    dst.fillOpacity_ = fillOpacity_;
    dst.blendMode_ = blendMode_;
    dst.clipped_ = clipped_;
    // id_ is intentionally NOT copied: a clone gets a fresh identity.
}

}  // namespace pe
