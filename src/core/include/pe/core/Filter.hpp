#pragma once

#include "pe/core/Brush.hpp"  // PaintCommand (reused as the generic tile-delta command)
#include "pe/core/Color.hpp"
#include "pe/core/Geometry.hpp"
#include "pe/core/Gradient.hpp"
#include "pe/core/Layer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Refusal.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace pe {

class Document;
class Selection;
class PixelBuffer;

// Max content area a destructive filter rasterizes at once. Tighter than the compositor's
// 8-bit cap because filters allocate several full-region FLOAT buffers (premultiplied src,
// tmp, dst, blurred) at about 16 bytes a pixel each; 16 MP keeps transient memory near
// 1 GB. Larger regions need tile-streaming (a later increment).
//
// Public because a caller has to be able to explain a refusal: over this cap every
// destructive filter and adjustment returns nullptr, and the shell can only tell the user
// why if it can name the limit.
inline constexpr std::int64_t kMaxFilterPixels = 16'000'000;

// The most memory one Move may commit: the tiles it touches at the source plus the tiles it
// touches at the destination, at the layer's own pixel size.
//
// A Move is an integer translation, so it is built one destination tile at a time at native
// depth and allocates nothing region-sized. kMaxFilterPixels does not apply to it: that
// bounds a filter's full-region float buffers, and applying it to a Move refused every
// document over about 15.6 MP, which is any photograph from a modern camera (#180).
//
// Stated in BYTES rather than tiles, and this matters. A Move's tiles are the layer's own
// pixels, so one tile is 256 KB at 8 bit but 1 MB at 32-bit float. A flat tile count would
// have let a one-pixel nudge on a large float layer build a four-gigabyte undo record while
// the identical count at 8 bit cost a quarter of that. The brush and the selection can count
// tiles because theirs hold one byte per pixel; this one cannot.
//
// 1 GiB matches kDefaultHistoryBytes, so the largest single Move is bounded by the same
// figure as the whole undo stack rather than by an unrelated one.
inline constexpr std::int64_t kMaxMoveBytes = 1LL << 30;

// Diagnostics: how many destination tiles a Move has BUILT since the process started.
//
// A Move emits one sample per pixel it relocates either way; what must not scale is the
// number of tiles it allocates and sweeps. The first version of the native-depth Move swept
// every tile in the bounding box of the source and the destination, so a small layer dragged
// a long way built a thousand tiles to relocate one, and nothing could observe it: the
// command's own touchedTileCount() reports tiles that CHANGED, which was two either way.
// Same purpose and same idiom as NativeFormat's gatherTileLookupCount.
[[nodiscard]] std::uint64_t moveTileBuildCount() noexcept;

// Why a destructive pixel edit on `layerId` would be refused, or a Refusal with code None
// if it would proceed. bakePixelEdit and everything built on it (every filter, every
// destructive adjustment, bucket and gradient fill, stamp, and the RESAMPLING half of
// transform) share these preconditions, and they return a bare nullptr, so a caller has no
// way to tell the cases apart from the null alone.
//
// moveLayerContent is NOT among them any more and this function must not be used to explain
// it. A Move is bounded by kMaxMoveBytes and validates its own rects, so asking here about a
// layer a Move declined gives a confidently wrong answer: a 4000x4000 layer reports
// "OverSizeBudget, 16 megapixels" while the Move on it in fact succeeds. Explaining a
// refused Move needs its own predicate against its own constants; until that exists a Move
// still declines silently past its budget, which is tracked in the follow-ups to #180.
//
// This puts the reasons in the engine, beside the code that enforces them and using the
// same constants, so the two cannot drift. It is deliberately a PREDICATE rather than an
// out-parameter on every entry point: threading a Refusal through two dozen call sites
// would be a large mechanical change for the same answer. The remaining gap is honest and
// worth stating: this re-evaluates the conditions rather than reporting the refusal from
// the call that actually declined, so a future reason added inside bakePixelEditImpl (as
// opposed to its preconditions) would not appear here until it is added here too.
[[nodiscard]] Refusal bakeRefusal(const Document& doc, LayerId layerId);

// Why moveLayerContent would refuse a shift of (dx, dy) on `layerId`, or code None if it
// would proceed.
//
// A Move needs its own predicate because it no longer shares bakeRefusal's preconditions: it
// is bounded by kMaxMoveBytes rather than by kMaxFilterPixels, and it validates its own
// rects. Asking bakeRefusal about a Move gives a confidently wrong answer, which is worse
// than none: a 4000x4000 layer reports "over budget, 16 megapixels" for a Move that in fact
// succeeds.
//
// This exists because the Move tool declining SILENTLY is the whole of #180. Fixing the
// threshold only moved where the silence starts; the drag still has to be able to say why.
// Same shape as bakeRefusal: it re-evaluates the preconditions beside the code that enforces
// them, using the same constants, rather than threading a Refusal back through the command.
[[nodiscard]] Refusal moveRefusal(const Document& doc, LayerId layerId, int dx, int dy);

// Run an in-place per-pixel transform over a pixel layer's content as a reversible
// tile-delta command (the shared machinery behind destructive filters and
// adjustments). `transform(img, w, h)` mutates the extracted content image in
// place. Optional selection gating (result lerped toward original by 1-coverage).
// Returns nullptr if not a pixel layer, no content, or content over budget.
[[nodiscard]] std::unique_ptr<PaintCommand> bakePixelEdit(
    Document& doc, LayerId layerId, std::string name,
    const std::function<void(std::span<Rgbaf>, int, int)>& transform,
    const Selection* selection = nullptr);

// Like bakePixelEdit, but operates over an explicit document-space `region` instead of the
// layer's current content bounds — for edits that grow the touched area (e.g. moving content
// to new tiles). The transform sees the region's pixels (row-major, w==region.width); pixels
// outside the layer's content read as transparent. Same caps/return contract as bakePixelEdit.
[[nodiscard]] std::unique_ptr<PaintCommand> bakePixelEditRegion(
    Document& doc, LayerId layerId, std::string name, Rect region,
    const std::function<void(std::span<Rgbaf>, int, int)>& transform,
    const Selection* selection = nullptr);

// Translate a pixel layer's entire content by (dx, dy) as a reversible tile-delta command
// (the Move tool). The vacated source area becomes transparent. Returns nullptr for a zero
// move, a non-pixel/empty layer, or an offset/region beyond the engine's size caps.
[[nodiscard]] std::unique_ptr<PaintCommand> moveLayerContent(Document& doc, LayerId layerId, int dx,
                                                             int dy);

// Affine-transform a pixel layer's content (scale / rotate / translate / skew) as a reversible
// tile-delta command — the engine behind the Transform tool. `srcToDst` maps source document
// coordinates to destination document coordinates; the layer's content is inverse-mapped and
// resampled (premultiplied bilinear, transparent outside the source). Returns nullptr for a
// non-pixel/empty layer, a singular/non-finite transform, or a destination beyond the engine's
// per-op size caps.
// `regionOfInterest`, when non-empty, narrows the pixels the command rewrites; everything
// outside it keeps its ORIGINAL content, so the result is not the full transform. It exists
// for the interactive preview, which repeats this on every motion event and only has to be
// right where the user can see it: the cost then follows the viewport instead of the layer,
// which on a 16 MP document is the difference between a second per mouse-move and a twelfth
// of one. The command committed on release must be built WITHOUT it.
//
// The size budget below is deliberately still charged on the whole source and destination,
// not on the narrowed region. A transform too large to commit must not preview either, or
// the user would drag something that cannot be applied.
[[nodiscard]] std::unique_ptr<PaintCommand> transformLayerContent(Document& doc, LayerId layerId,
                                                                  const Affine2D& srcToDst,
                                                                  Rect regionOfInterest = Rect{});

// Resample a pixel layer's content, sampled over srcCanvas onto dstCanvas, as a reversible
// tile-delta command -- the per-layer engine behind Image Size. Separable Catmull-Rom (the same
// kernel as resampleImage), tile-streamed so it allocates nothing region-sized and is bounded in
// bytes by the tiles it touches rather than by kMaxFilterPixels. nullptr for a non-pixel/empty
// layer, a non-positive/non-finite scale, or a destination beyond the engine's size caps.
[[nodiscard]] std::unique_ptr<PaintCommand> resampleLayerContent(Document& doc, LayerId layerId,
                                                                 Rect srcCanvas, Rect dstCanvas);

// Paint Bucket: flood-fill the contiguous (4-connected) region of layer `layerId` reachable
// from the seed whose color is within `tolerance` (max per-channel, 0..255) of the seed's,
// compositing `fillColor` (straight alpha, Normal) over each. Bounded by the canvas; honors the
// selection. nullptr for a non-pixel layer, an off-canvas seed, or an over-budget canvas.
[[nodiscard]] std::unique_ptr<PaintCommand> bucketFill(Document& doc, LayerId layerId, int seedX,
                                                       int seedY, Rgbaf fillColor, int tolerance,
                                                       const Selection* selection = nullptr);

// Stamp a straight-alpha RGBA8 source raster onto a pixel layer at `origin`, compositing it
// (Normal, straight alpha) over the existing pixels as a reversible tile-delta command — the
// general primitive behind the Type tool (rasterized text) and future paste/clone paths. Honors
// the selection. `name` labels the History entry. nullptr for a non-pixel layer, an empty source,
// or a region beyond the engine's size caps.
[[nodiscard]] std::unique_ptr<PaintCommand> stampBuffer(Document& doc, LayerId layerId,
                                                        Point origin, const PixelBuffer& src,
                                                        std::string name,
                                                        const Selection* selection = nullptr);

// ---- Region copy / clear (the engine half of cut, copy and paste) ----

// The region a copy should take: the selection's tight pixel bounds clipped to the canvas, or
// the whole canvas when there is no selection. Empty when the selection is active but selects
// nothing, which is the case a caller has to refuse rather than copy.
[[nodiscard]] Rect copyRegionFor(const Document& doc, const Selection* selection);

// Read a rectangular region of a pixel layer's content, with the selection folded into alpha:
// a pixel the selection only half covers comes out half transparent, so a feathered selection
// copies with a soft edge instead of a stair-stepped one. Pixels outside the layer's content
// read as transparent.
//
// Not a command: nothing is modified. Empty for a non-pixel layer, an empty region, or a
// region beyond the engine's per-op size caps.
[[nodiscard]] PixelBuffer copyLayerRegion(const Document& doc, LayerId layerId, Rect region,
                                          const Selection* selection = nullptr);

// Fold a selection's coverage into an already-extracted raster's alpha, where `origin` is the
// document position of the raster's top-left. Exposed separately because Copy Merged reads its
// pixels from the renderer's composite rather than from a layer, and both must apply the
// selection the same way.
void applySelectionAlpha(PixelBuffer& img, Point origin, const Selection* selection);

// Clear a region of a pixel layer to transparent as a reversible tile-delta command: the second
// half of Cut, and Edit > Clear on its own. Honors the selection, so a feathered edge clears
// proportionally. nullptr for a non-pixel layer, an empty region, or one over the size caps.
[[nodiscard]] std::unique_ptr<PaintCommand> clearRegion(Document& doc, LayerId layerId, Rect region,
                                                        const Selection* selection = nullptr);

// A new pixel layer holding `src`, its top-left at `origin`. The caller wraps it in an
// AddLayerCommand, so a paste is one undo step rather than an add followed by a stamp.
// `selectionMask`, when given, becomes the layer's mask, which is what Paste Into is: the
// pasted pixels are all there, and the selection decides how much of them shows.
[[nodiscard]] std::unique_ptr<PixelLayer> layerFromBuffer(const PixelBuffer& src, Point origin,
                                                          std::string name,
                                                          const Selection* selectionMask = nullptr);

// Gradient: composite a linear gradient over layer `layerId`, from c0 (at `start`) to c1 (at
// `end`), each pixel's stop color interpolated by its projection onto the start->end axis (clamped
// to [0,1]) and composited straight-alpha (Normal) over the existing pixel — so a semi-transparent
// stop lets the backdrop show through, matching bucketFill. Honors the selection (confines to it).
// nullptr for a non-pixel layer, a zero-length drag, or an over-budget canvas.
[[nodiscard]] std::unique_ptr<PaintCommand> gradientFill(Document& doc, LayerId layerId,
                                                         Point start, Point end, Rgbaf c0, Rgbaf c1,
                                                         const Selection* selection = nullptr);

// The same, through a multi-stop pe::Gradient. `foreground` and `background` fill in the stops
// that follow the loaded colours, so a preset like "Foreground to Transparent" draws with
// whatever is loaded now. The two-colour overload above is this one with a two-stop ramp.
[[nodiscard]] std::unique_ptr<PaintCommand> gradientFill(Document& doc, LayerId layerId,
                                                         Point start, Point end,
                                                         const Gradient& gradient, Rgbaf foreground,
                                                         Rgbaf background,
                                                         const Selection* selection = nullptr);

// ---- Reference filter kernels ----
// Operate on a contiguous w*h straight-alpha Rgbaf image (row-major). Edges clamp
// to the border. These define correctness; tiled/SIMD/GPU paths must match.
// See docs/systems/12-filter-engine.md.

// Separable box blur of the given integer radius (radius <= 0 copies src to dst).
void boxBlur(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h, int radius);

// Separable Gaussian blur (sigma <= 0 copies src to dst).
void gaussianBlur(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h, float sigma);

// Unsharp-mask sharpen: result = src + amount*(src - gaussianBlur(src, radius)),
// only where |detail| exceeds threshold. amount == 0 is identity.
void unsharpMask(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h, float radius,
                 float amount, float threshold);

// Mosaic (pixelate): partition into cell x cell blocks and fill each block with its
// average color (averaged in premultiplied alpha so transparent pixels don't bleed).
// cell <= 1 copies src to dst. Edge blocks are clamped to the image bounds.
void mosaic(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h, int cell);

// Median (noise reduction / despeckle): replace each pixel with the per-channel median
// of its (2r+1)x(2r+1) clamped neighborhood. radius <= 0 copies src to dst. Removes
// salt-and-pepper speckle while preserving edges better than a blur.
void medianFilter(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h, int radius);

// Find Edges (stylize): per-channel Sobel gradient magnitude, inverted so flat areas
// are white and edges are dark (as in Photoshop). Alpha is preserved. Edges clamp.
void findEdges(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h);

// Add Noise: add zero-mean noise scaled by amount in [0,1]. The noise is derived
// from a per-pixel hash of (index, seed), so it is fully deterministic and
// reproducible. monochromatic adds the same value to R/G/B; otherwise each channel
// gets independent noise. gaussian selects a Gaussian distribution (vs uniform).
// Alpha is preserved; results clamp on write. amount <= 0 copies src to dst.
void addNoise(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h, float amount,
              bool monochromatic, bool gaussian, uint32_t seed);

// ---- Polymorphic filter ----

class Filter {
public:
    virtual ~Filter() = default;
    [[nodiscard]] virtual std::string id() const = 0;
    [[nodiscard]] virtual std::string displayName() const = 0;
    // Apply src -> dst (same w*h dimensions, contiguous Rgbaf).
    virtual void apply(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h) const = 0;
    [[nodiscard]] virtual std::unique_ptr<Filter> clone() const = 0;
};

class GaussianBlurFilter final : public Filter {
public:
    explicit GaussianBlurFilter(float sigma = 1.0f) : sigma_(sigma < 0.0f ? 0.0f : sigma) {}
    [[nodiscard]] std::string id() const override { return "blur.gaussian"; }
    [[nodiscard]] std::string displayName() const override { return "Gaussian Blur"; }
    void apply(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h) const override {
        gaussianBlur(src, dst, w, h, sigma_);
    }
    [[nodiscard]] std::unique_ptr<Filter> clone() const override {
        return std::make_unique<GaussianBlurFilter>(*this);
    }

private:
    float sigma_;
};

class BoxBlurFilter final : public Filter {
public:
    explicit BoxBlurFilter(int radius = 1) : radius_(radius < 0 ? 0 : radius) {}
    [[nodiscard]] std::string id() const override { return "blur.box"; }
    [[nodiscard]] std::string displayName() const override { return "Box Blur"; }
    void apply(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h) const override {
        boxBlur(src, dst, w, h, radius_);
    }
    [[nodiscard]] std::unique_ptr<Filter> clone() const override {
        return std::make_unique<BoxBlurFilter>(*this);
    }

private:
    int radius_;
};

class SharpenFilter final : public Filter {
public:
    explicit SharpenFilter(float radius = 1.0f, float amount = 1.0f, float threshold = 0.0f)
        : radius_(radius < 0.0f ? 0.0f : radius),
          amount_(amount),
          threshold_(threshold < 0.0f ? 0.0f : threshold) {}
    [[nodiscard]] std::string id() const override { return "sharpen.unsharp"; }
    [[nodiscard]] std::string displayName() const override { return "Sharpen"; }
    void apply(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h) const override {
        unsharpMask(src, dst, w, h, radius_, amount_, threshold_);
    }
    [[nodiscard]] std::unique_ptr<Filter> clone() const override {
        return std::make_unique<SharpenFilter>(*this);
    }

private:
    float radius_, amount_, threshold_;
};

class MosaicFilter final : public Filter {
public:
    explicit MosaicFilter(int cell = 8) : cell_(cell < 1 ? 1 : cell) {}
    [[nodiscard]] std::string id() const override { return "pixelate.mosaic"; }
    [[nodiscard]] std::string displayName() const override { return "Mosaic"; }
    void apply(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h) const override {
        mosaic(src, dst, w, h, cell_);
    }
    [[nodiscard]] std::unique_ptr<Filter> clone() const override {
        return std::make_unique<MosaicFilter>(*this);
    }

private:
    int cell_;
};

class MedianFilter final : public Filter {
public:
    // Radius is capped (the neighborhood cost grows as r^2); 15 is a generous bound.
    explicit MedianFilter(int radius = 1) : radius_(radius < 0 ? 0 : (radius > 15 ? 15 : radius)) {}
    [[nodiscard]] std::string id() const override { return "noise.median"; }
    [[nodiscard]] std::string displayName() const override { return "Median"; }
    void apply(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h) const override {
        medianFilter(src, dst, w, h, radius_);
    }
    [[nodiscard]] std::unique_ptr<Filter> clone() const override {
        return std::make_unique<MedianFilter>(*this);
    }

private:
    int radius_;
};

class FindEdgesFilter final : public Filter {
public:
    [[nodiscard]] std::string id() const override { return "stylize.findedges"; }
    [[nodiscard]] std::string displayName() const override { return "Find Edges"; }
    void apply(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h) const override {
        findEdges(src, dst, w, h);
    }
    [[nodiscard]] std::unique_ptr<Filter> clone() const override {
        return std::make_unique<FindEdgesFilter>(*this);
    }
};

class AddNoiseFilter final : public Filter {
public:
    explicit AddNoiseFilter(float amount = 0.1f, bool monochromatic = false, bool gaussian = true,
                            uint32_t seed = 1u)
        : amount_(amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount)),
          monochromatic_(monochromatic),
          gaussian_(gaussian),
          seed_(seed) {}
    [[nodiscard]] std::string id() const override { return "noise.add"; }
    [[nodiscard]] std::string displayName() const override { return "Add Noise"; }
    void apply(std::span<const Rgbaf> src, std::span<Rgbaf> dst, int w, int h) const override {
        addNoise(src, dst, w, h, amount_, monochromatic_, gaussian_, seed_);
    }
    [[nodiscard]] std::unique_ptr<Filter> clone() const override {
        return std::make_unique<AddNoiseFilter>(*this);
    }

private:
    float amount_;
    bool monochromatic_;
    bool gaussian_;
    uint32_t seed_;
};

// Apply a filter destructively to a pixel layer's content, as a reversible
// tile-delta command (PaintCommand). If `selection` is active, the filter is
// confined to it (result lerped toward original by 1-coverage). Returns nullptr if
// the layer is not a pixel layer, has no content, or the content is over budget.
[[nodiscard]] std::unique_ptr<PaintCommand> applyFilter(Document& doc, LayerId layerId,
                                                        const Filter& filter,
                                                        const Selection* selection = nullptr);

}  // namespace pe
