#pragma once

#include "pe/core/Geometry.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/Tile.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <span>
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
    // Replace the selection with the filled interior of a closed polygon (even-odd rule),
    // for the Lasso tool. Fewer than 3 vertices, or a bounding box past the selection caps,
    // selects nothing.
    void selectPolygon(std::span<const Point> vertices);

    // --- saved selections <-> alpha channels (systems/19) ---
    // Save the selection's coverage over `bounds` to an 8-bit grayscale mask (the
    // alpha-channel representation): the value is replicated to R=G=B, alpha opaque.
    // An inactive selection reads as fully selected (255). Empty bounds -> empty.
    [[nodiscard]] PixelBuffer toMask(Rect bounds) const;
    // Replace the selection with an 8-bit grayscale mask (load an alpha channel as a
    // selection), placing the mask's top-left at (originX, originY). The mask's red
    // channel is the coverage value. Becomes active; an empty mask deselects.
    void loadMask(const PixelBuffer& mask, int originX, int originY);

    // Bounding box of selected tiles (empty if inactive / nothing selected). Tile-granular
    // (snaps to 256px); cheap (iterates tiles, not pixels).
    [[nodiscard]] Rect selectedBounds() const noexcept;
    // Tight pixel bounding box of the actually-selected pixels (empty if inactive / nothing
    // selected). Pixel-accurate, unlike selectedBounds — for the marching-ants outline. Scans
    // the selected tiles' pixels, so compute it on selection change, not every repaint.
    [[nodiscard]] Rect tightBounds() const noexcept;
    [[nodiscard]] std::size_t tileCount() const noexcept { return tiles_.size(); }

private:
    using Key = std::pair<int, int>;
    using GrayTile = std::array<uint8_t, kTilePixels>;
    static constexpr Key keyOf(TileCoord c) noexcept { return {c.col, c.row}; }

    [[nodiscard]] uint8_t stored(int x, int y) const noexcept;  // 0 if absent
    void setValue(int x, int y, uint8_t v);
    void fillRect(Rect r, uint8_t v);
    void dropEmptyTiles();  // erase all-zero tiles (keeps selectedBounds tight)

    std::map<Key, GrayTile> tiles_;
    bool active_ = false;
};

// Magic Wand: select the contiguous (4-connected) region of `image` reachable from the
// seed pixel whose color is within `tolerance` (max per-channel difference, 0..255) of the
// seed's. Returns an inactive selection for an out-of-bounds seed, an empty image, or an
// image past the engine's selection size cap. Sample from the composited canvas.
[[nodiscard]] Selection magicWandSelection(const PixelBuffer& image, int seedX, int seedY,
                                           int tolerance);

}  // namespace pe
