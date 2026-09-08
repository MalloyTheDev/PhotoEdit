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

    // --- edge refinements (Select menu) ---
    // Each is a no-op when the selection is inactive/empty, when `radius` is non-positive, or when
    // the working region would exceed the selection size caps (so a pathological input never
    // over-allocates). grow/shrink treat the mask as binary (coverage >= 50% is "in") and move the
    // boundary by ~`radius` px using a chamfer distance transform (round, not boxy); they operate
    // relative to the selection itself (the exterior, on- or off-canvas, counts as unselected), so
    // they preserve any off-canvas coverage and shrink contracts inward from a canvas edge too.
    // feather softens the edge with a Gaussian of standard deviation `radius` px (partial
    // coverage), confined to `canvas`: the canvas border is treated as "the selection continues",
    // so a selection touching/filling the canvas is not faded there.
    void grow(int radius);
    void shrink(int radius);
    void feather(float radius, Rect canvas);

    // Value equality (active flag + coverage tiles). Lets callers skip a no-op undo step.
    [[nodiscard]] bool operator==(const Selection&) const = default;

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

    // One tile's bytes, row-major, indexed tileLocalOffset(y) * kTileSize + tileLocalOffset(x).
    using GrayTile = std::array<uint8_t, kTilePixels>;

    // The stored coverage tile at `c`, or nullptr when it is absent (which reads as 0).
    // Mirrors TileStoreT::find, including the nullptr-means-default convention.
    //
    // This exposes the STORED bytes, so it deliberately ignores active(): an inactive
    // selection means "everything editable", which has no tile to return. Callers must
    // have established active() themselves, which every gated loop already does, or
    // handle the inactive case before asking.
    //
    // For a loop confined to one tile, resolving the tile once and indexing the array
    // replaces one map lookup per pixel. coverage() stays the right call for scattered
    // access.
    [[nodiscard]] const GrayTile* findTile(TileCoord c) const noexcept;

private:
    using Key = std::pair<int, int>;
    static constexpr Key keyOf(TileCoord c) noexcept { return {c.col, c.row}; }

    [[nodiscard]] uint8_t stored(int x, int y) const noexcept;  // 0 if absent
    void setValue(int x, int y, uint8_t v);

    // Write one horizontal run of coverage, resolving the tile ONCE instead of once per
    // pixel.
    //
    // PRECONDITION: the run lies wholly inside one tile, i.e. xEnd > xBegin and
    // floorDiv(xBegin, kTileSize) == floorDiv(xEnd - 1, kTileSize). forEachRun below is the
    // only intended caller and guarantees it.
    //
    // `src(x, y, current)` returns the new value; `current` is the stored coverage there, or
    // 0 when the tile is absent, so a read-modify-write caller such as invert() reuses the
    // one resolution for both halves. It must be pure and must not read back through
    // stored()/value().
    //
    // Semantics are setValue's, byte for byte. An absent tile is created only when the run
    // holds at least one non-zero value; once the tile exists every value is written
    // verbatim, zeros included. That is what keeps tiles_ free of all-zero tiles, which
    // selectedBounds() and the defaulted operator== both read directly. It does NOT replace
    // dropEmptyTiles(): zeros written into an EXISTING tile can empty it, and only a
    // full-tile scan sees that.
    template <class Src>
    void setRun(int y, int xBegin, int xEnd, Src&& src);

    // Walk `r` as runs that each lie wholly inside one tile, calling setRun for each.
    //
    // The column range is computed once and iterated FIXED, rather than advancing x to a
    // computed run end. That is the shape #176 established for the .pedoc gather, and the
    // reason is recorded there: an off-by-one in such an advance livelocks rather than
    // corrupting, and no test can tell a hang from a slow operation.
    template <class Src>
    void forEachRun(Rect r, Src&& src);
    // Replace coverage inside the mask's region, leaving coverage outside it untouched.
    // loadMask() clears everything first, which is only safe when the region provably
    // covers the whole selection.
    void writeMaskRegion(const PixelBuffer& mask, int originX, int originY);
    void fillRect(Rect r, uint8_t v);
    void dropEmptyTiles();  // erase all-zero tiles (keeps selectedBounds tight)

    std::map<Key, GrayTile> tiles_;
    bool active_ = false;
};

// Magic Wand: select the contiguous (4-connected) region of `image` reachable from the
// seed pixel whose color is within `tolerance` (max per-channel difference, 0..255) of the
// seed's. Returns an inactive selection for an out-of-bounds seed, an empty image, or an
// image past the engine's selection size cap. Sample from the composited canvas.
// Diagnostics: how many coordinate-to-tile resolutions the mask WRITE paths have performed
// since the process started. A write emits one value per pixel of its rect either way; what
// must not scale with the pixel count is this. Same purpose and idiom as NativeFormat's
// gatherTileLookupCount. Atomic because the wand runs on a worker thread.
[[nodiscard]] std::uint64_t maskWriteTileLookupCount() noexcept;

// Diagnostics: how many coverage tiles the mask write paths have ALLOCATED since the process
// started. Separate from the lookup count because it answers a different question, and the
// answer is invisible in the finished selection: dropEmptyTiles() erases an all-zero tile
// after the fact, so a writer that materialises the whole region and then throws most of it
// away produces exactly the same result. What it does not produce is the same peak, and on a
// canvas-sized write that difference is hundreds of megabytes.
[[nodiscard]] std::uint64_t maskTileAllocCount() noexcept;

[[nodiscard]] Selection magicWandSelection(const PixelBuffer& image, int seedX, int seedY,
                                           int tolerance);

}  // namespace pe
