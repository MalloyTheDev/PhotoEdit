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

    // One tile's bytes, row-major, indexed localIndex(y) * kTileSize + localIndex(x).
    using GrayTile = std::array<uint8_t, kTilePixels>;

    [[nodiscard]] uint8_t value(int x, int y) const noexcept;  // kOpaque if absent
    // The whole tile at `c`, or nullptr when it is absent (which reads as kOpaque).
    // Mirrors TileStoreT::find, including the nullptr-means-default convention.
    //
    // For callers whose loop is confined to one tile, which is every pixel loop in the
    // compositor: resolving the tile once and indexing the array is the difference
    // between one map lookup and 65,536 of them. value() stays the right call for
    // scattered access.
    [[nodiscard]] const GrayTile* findTile(TileCoord c) const noexcept;

    // Replace a whole tile's bytes in one call, creating it if absent. For a writer that
    // derives an entire tile at once: setValue() resolves the tile per pixel, so writing a
    // tile through it costs 65,536 map lookups for one tile's worth of data.
    //
    // Unlike setValue this always materializes the tile, so only call it with content that
    // is not entirely kOpaque, or the buffer keeps a redundant fully-revealing tile that
    // empty()/contentBounds()/serialization would then have to carry.
    void setTile(TileCoord c, const GrayTile& bytes);
    void setValue(int x, int y, uint8_t v);
    void fillRect(Rect r, uint8_t v);

    [[nodiscard]] bool empty() const noexcept { return tiles_.empty(); }
    [[nodiscard]] std::size_t tileCount() const noexcept { return tiles_.size(); }
    // Union of allocated (non-default) tile bounds.
    [[nodiscard]] Rect contentBounds() const noexcept;

    // Drop any allocated tile within `region` whose bytes are all kOpaque: such a tile is
    // semantically identical to an absent one (absent reads as kOpaque), so erasing it is lossless
    // and keeps the buffer canonical. Callers that may write kOpaque into a previously-allocated
    // tile (e.g. undoing a mask-brush stroke) use this so empty()/contentBounds()/serialization
    // stay byte-exact rather than retaining a redundant fully-revealing tile.
    void compact(Rect region) noexcept;

    // Shift all coverage by (dx, dy) in document space, leaving the vacated area
    // reading kOpaque (absent). Used by the crop command, whose canvas-origin change
    // moves every layer's pixels: the mask is stored in document space too, so
    // without this it would stay behind and mask the wrong pixels.
    //
    // Returns false and leaves the buffer UNTOUCHED when the work would exceed
    // `maxPixels`, or when the destination would leave the representable coordinate
    // range. Callers that must not half-apply a multi-step edit check this first.
    //
    // Exactly invertible: translating by (dx, dy) then (-dx, -dy) restores the
    // buffer byte for byte, because values are copied verbatim rather than
    // resampled. The one normalization is that an allocated but entirely kOpaque
    // tile is dropped, which compact() already documents as lossless.
    [[nodiscard]] bool translate(int dx, int dy, int64_t maxPixels);

    // Whether translate() with the same arguments would succeed, without mutating.
    // A multi-layer edit (crop) checks every mask up front so it can refuse as a
    // whole rather than shifting some layers and leaving others behind.
    [[nodiscard]] bool canTranslate(int dx, int dy, int64_t maxPixels) const;

private:
    using Key = std::pair<int, int>;
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

    // The invert + density transform, applied to a mask byte already in hand. Split out
    // of evaluate() so a caller that resolved the tile itself can skip the per-pixel
    // buffer lookup without duplicating this arithmetic: there is one copy of it, so
    // the hoisted and per-pixel paths cannot drift.
    [[nodiscard]] float evaluateValue(uint8_t v) const noexcept {
        float m = static_cast<float>(v) / 255.0f;
        if (inverted_) m = 1.0f - m;
        // Density scales the HIDING, not the revealing: hide_effective = (1 - m) * density.
        //
        // This was `m * density`, which is the wrong end. The mask enters the compositor as a
        // multiply into alpha, so "the mask does nothing" is a factor of 1, not 0 - and under
        // the old formula density 0 made every masked layer vanish outright, while a black
        // region at density 0.5 stayed 100% hidden and the untouched white around it dropped
        // to half coverage instead. Density moved the wrong half of the mask.
        //
        // The difference is a uniform (1 - density) of coverage across the whole layer, so at
        // density 1 the two agree exactly, which is why nothing but the density tests noticed.
        //
        // clamp01 keeps the result valid even if density_ were ever set out of range by a
        // future path (e.g. deserialization) that bypasses setDensity.
        return clamp01(1.0f - (1.0f - m) * density_);
    }

    // Effective coverage in [0,1] at a document pixel: (value/255, inverted if set)
    // scaled by density. Live feather is added in a later increment.
    [[nodiscard]] float evaluate(int x, int y) const noexcept {
        return evaluateValue(buffer_.value(x, y));
    }

    // True when this mask reveals everything, so multiplying by it is a no-op and the caller
    // can skip its loop entirely.
    //
    // Two independent ways to be a no-op now that density scales the hiding: a density of zero
    // ignores the mask whatever is painted in it, and an empty non-inverted buffer reads as
    // fully revealing at ANY density. The old predicate also demanded density >= 1, which was
    // sound but is now stale reasoning: it would skip the loop for exactly the masks the old
    // formula happened to leave alone.
    [[nodiscard]] bool isFullyRevealing() const noexcept {
        return density_ <= 0.0f || (!inverted_ && buffer_.empty());
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

// Resample a mask's coverage from `srcCanvas` onto `dstCanvas` (the scaled canvas), for Image
// Size, returning a new buffer. Single-channel and clamp-to-edge to the canvas, matching the
// pixel resampler so a layer's mask stays aligned with its pixels; absent samples read kOpaque
// (255), never 0, so the parts a mask does not cover stay revealing. A free function, not a
// method, because it needs the Catmull-Rom kernel that lives outside Mask, and because a
// resample is lossy: the caller keeps the original for undo rather than inverting this.
[[nodiscard]] MaskBuffer resampleMask(const MaskBuffer& src, Rect srcCanvas, Rect dstCanvas);

// Whether a full-coverage mask over `canvas` can be materialized (within the mask buffer's tile/
// pixel caps). fillRect / maskFromSelection silently no-op past that bound, so callers that need a
// materialized mask (hide-all, mask-from-an-active-selection) check this first to avoid attaching a
// misleadingly empty (reveal-all) mask on an over-large canvas. A RevealAll mask (empty buffer)
// always fits — it allocates nothing.
[[nodiscard]] bool maskFillFits(Rect canvas) noexcept;

}  // namespace pe
