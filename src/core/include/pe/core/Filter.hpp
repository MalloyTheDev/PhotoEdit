#pragma once

#include "pe/core/Brush.hpp"  // PaintCommand (reused as the generic tile-delta command)
#include "pe/core/Color.hpp"
#include "pe/core/Geometry.hpp"
#include "pe/core/Layer.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace pe {

class Document;
class Selection;

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
