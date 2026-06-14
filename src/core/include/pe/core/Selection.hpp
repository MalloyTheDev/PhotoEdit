#pragma once

#include "pe/core/Geometry.hpp"
#include "pe/core/Tile.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <utility>

namespace pe {

// A document-wide selection: a grayscale coverage mask where 255 == fully
// selected, 0 == not selected, and intermediate values are partial (feathered /
// anti-aliased) selection. Stored sparsely in 256px tiles.
//
// When INACTIVE (the default, or after selectNone), the whole document is
// editable: coverage() is 1.0 everywhere and value() is 255. When active, only
// selected pixels are editable. The selection gates painting/fills/filters by
// multiplying into their per-pixel coverage. Marching ants (the UI visualization)
// derive from this mask's boundary in the app shell. See
// docs/systems/07-selection-system.md.
class Selection {
public:
    [[nodiscard]] bool active() const noexcept { return active_; }

    // Coverage in [0,1] used to gate edits. 1.0 everywhere when inactive.
    [[nodiscard]] float coverage(int x, int y) const noexcept;
    // Raw 0..255 selection value (255 everywhere when inactive).
    [[nodiscard]] uint8_t value(int x, int y) const noexcept;

    void selectNone() noexcept;   // deactivate -> whole document editable
    void selectAll(Rect canvas);  // active; fully select the canvas
    void selectRect(Rect r);      // replace selection with a rectangle
    void addRect(Rect r);         // union a rectangle into the selection
    void subtractRect(Rect r);    // remove a rectangle from the selection
    void intersectRect(Rect r);   // keep only the overlap with a rectangle
    void invert(Rect canvas);     // invert selection within the canvas bounds

    // Bounding box of selected tiles (empty if inactive / nothing selected).
    [[nodiscard]] Rect selectedBounds() const noexcept;
    [[nodiscard]] std::size_t tileCount() const noexcept { return tiles_.size(); }

private:
    using Key = std::pair<int, int>;
    using GrayTile = std::array<uint8_t, kTilePixels>;
    static constexpr Key keyOf(TileCoord c) noexcept { return {c.col, c.row}; }

    [[nodiscard]] uint8_t stored(int x, int y) const noexcept;  // 0 if absent
    void setValue(int x, int y, uint8_t v);
    void fillRect(Rect r, uint8_t v);

    std::map<Key, GrayTile> tiles_;
    bool active_ = false;
};

}  // namespace pe
