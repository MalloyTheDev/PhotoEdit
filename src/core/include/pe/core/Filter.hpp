#pragma once

#include "pe/core/Brush.hpp"  // PaintCommand (reused as the generic tile-delta command)
#include "pe/core/Color.hpp"
#include "pe/core/Geometry.hpp"
#include "pe/core/Layer.hpp"

#include <memory>
#include <span>
#include <string>

namespace pe {

class Document;
class Selection;

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

// Apply a filter destructively to a pixel layer's content, as a reversible
// tile-delta command (PaintCommand). If `selection` is active, the filter is
// confined to it (result lerped toward original by 1-coverage). Returns nullptr if
// the layer is not a pixel layer, has no content, or the content is over budget.
[[nodiscard]] std::unique_ptr<PaintCommand> applyFilter(Document& doc, LayerId layerId,
                                                        const Filter& filter,
                                                        const Selection* selection = nullptr);

}  // namespace pe
