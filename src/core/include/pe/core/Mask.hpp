#pragma once

#include "pe/core/Color.hpp"
#include "pe/core/Geometry.hpp"
#include "pe/core/Tile.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <utility>

namespace pe {

class Selection;

// Sparse, tiled, single-channel grayscale coverage buffer for masks. Absent tiles
// read as kOpaque (fully revealing), so an empty buffer means "no masking" and
// costs nothing. 8-bit today; 16-bit arrives with color management (M6). See
// docs/systems/06-masks.md.
class MaskBuffer {
public:
    static constexpr uint8_t kOpaque = 255;  // reveals
    static constexpr uint8_t kClear = 0;     // hides

    [[nodiscard]] uint8_t value(int x, int y) const noexcept;  // kOpaque if absent
    void setValue(int x, int y, uint8_t v);
    void fillRect(Rect r, uint8_t v);

    [[nodiscard]] bool empty() const noexcept { return tiles_.empty(); }
    [[nodiscard]] std::size_t tileCount() const noexcept { return tiles_.size(); }
    // Union of allocated (non-default) tile bounds.
    [[nodiscard]] Rect contentBounds() const noexcept;

private:
    using Key = std::pair<int, int>;
    using GrayTile = std::array<uint8_t, kTilePixels>;
    static constexpr Key keyOf(TileCoord c) noexcept { return {c.col, c.row}; }

    std::map<Key, GrayTile> tiles_;
};

// A raster mask attached to a layer (or, later, a filter / quick-mask session).
// The compositor multiplies its effective coverage into the layer's alpha before
// blending, so a 50% mask makes the layer 50% transparent there (color unchanged).
class Mask {
public:
    enum class Kind : uint8_t { Layer, Filter, Quick };

    explicit Mask(Kind kind = Kind::Layer) : kind_(kind) {}

    [[nodiscard]] Kind kind() const noexcept { return kind_; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] float density() const noexcept { return density_; }
    [[nodiscard]] bool inverted() const noexcept { return inverted_; }

    void setEnabled(bool e) noexcept { enabled_ = e; }
    void setDensity(float d) noexcept { density_ = clamp01(d); }
    void setInverted(bool v) noexcept { inverted_ = v; }

    [[nodiscard]] MaskBuffer& buffer() noexcept { return buffer_; }
    [[nodiscard]] const MaskBuffer& buffer() const noexcept { return buffer_; }
    [[nodiscard]] Rect contentBounds() const noexcept { return buffer_.contentBounds(); }

    // Effective coverage in [0,1] at a document pixel: (value/255, inverted if set)
    // scaled by density. Live feather is added in a later increment.
    [[nodiscard]] float evaluate(int x, int y) const noexcept {
        float m = static_cast<float>(buffer_.value(x, y)) / 255.0f;
        if (inverted_) m = 1.0f - m;
        return m * density_;
    }

private:
    Kind kind_;
    MaskBuffer buffer_;
    bool enabled_ = true;
    float density_ = 1.0f;
    bool inverted_ = false;
};

// Build a layer mask from a selection: reveal where selected, hide where not.
// Lossless (same grayscale format). An inactive selection yields an empty mask
// (fully revealing). Bounded to `canvas`.
[[nodiscard]] Mask maskFromSelection(const Selection& selection, Rect canvas);

}  // namespace pe
