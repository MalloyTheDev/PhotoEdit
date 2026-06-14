#pragma once

#include "pe/core/Layer.hpp"

namespace pe {

// A procedural solid-color fill confined to a document-space rectangle (no stored
// raster — resolution independent). Outside the rect it is transparent. Gradient
// and pattern fills extend this kind in M5.
class SolidColorLayer final : public Layer {
public:
    SolidColorLayer(Rgba8 color, Rect bounds, std::string name = "Color Fill");

    [[nodiscard]] Rgba8 color() const noexcept { return color_; }
    [[nodiscard]] Rect bounds() const noexcept { return bounds_; }
    void setColor(Rgba8 c) noexcept { color_ = c; }
    void setBounds(Rect r) noexcept { bounds_ = r; }

    [[nodiscard]] Rect contentBounds() const noexcept override { return bounds_; }
    void renderInto(TileCoord coord, std::span<Rgbaf> dst) const override;
    [[nodiscard]] std::unique_ptr<Layer> clone() const override;

private:
    Rgba8 color_;
    Rect bounds_;
};

}  // namespace pe
