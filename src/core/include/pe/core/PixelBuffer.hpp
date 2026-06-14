#pragma once

#include "pe/core/Color.hpp"
#include "pe/core/Geometry.hpp"

#include <cstddef>
#include <vector>

namespace pe {

// A simple contiguous 8-bit RGBA raster. This is intentionally the "flat" buffer
// used for whole-image operations (import results, flattened export, thumbnails)
// and for tests. The tiled, paged layer storage that the document engine uses in
// production is a separate type built in a later milestone; both share the Rgba8
// pixel format so conversions are trivial. See docs/systems/01-document-system.md.
class PixelBuffer {
public:
    PixelBuffer() = default;
    PixelBuffer(int width, int height, Rgba8 fill = Rgba8{});

    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }
    [[nodiscard]] Size size() const noexcept { return Size{width_, height_}; }
    [[nodiscard]] Rect bounds() const noexcept { return Rect{0, 0, width_, height_}; }
    [[nodiscard]] bool isEmpty() const noexcept { return pixels_.empty(); }

    [[nodiscard]] Rgba8 at(int x, int y) const noexcept;
    void set(int x, int y, Rgba8 c) noexcept;

    void fill(Rgba8 c) noexcept;

    [[nodiscard]] const Rgba8* data() const noexcept { return pixels_.data(); }
    [[nodiscard]] Rgba8* data() noexcept { return pixels_.data(); }

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<Rgba8> pixels_;
};

}  // namespace pe
