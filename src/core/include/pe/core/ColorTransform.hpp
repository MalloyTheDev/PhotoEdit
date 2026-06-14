#pragma once

#include "pe/core/Color.hpp"
#include "pe/core/ColorProfile.hpp"

#include <cstddef>
#include <memory>
#include <span>

namespace pe {

// A built, reusable color transform: a baked lcms2 pipeline (cmsHTRANSFORM) from
// one profile to another. apply() is the per-tile hot path; the transform is
// immutable once built and safe to share for read across worker threads.
//
// This first cut handles RGB->RGB float transforms (the working-space / display
// leg), operating on straight-alpha Rgbaf with alpha copied through. Gray/CMYK and
// device-link soft-proofing arrive in later increments. The header is lcms2-free
// (opaque handle); the implementation is built only when PHOTOEDIT_HAVE_LCMS2.
class ColorTransform {
public:
    ~ColorTransform();
    ColorTransform(const ColorTransform&) = delete;
    ColorTransform& operator=(const ColorTransform&) = delete;

    // Build a transform src->dst with the given intent and black-point compensation.
    // Returns nullptr if either profile is null/invalid, either is not RGB (for now),
    // or lcms fails to link the transform.
    [[nodiscard]] static std::shared_ptr<ColorTransform> create(
        const ColorProfile& src, const ColorProfile& dst,
        RenderingIntent intent = RenderingIntent::RelativeColorimetric,
        bool blackPointCompensation = true);

    // Transform `count` straight-alpha Rgbaf pixels src -> dst. src and dst may alias
    // (in-place). Color channels are converted; alpha is copied unchanged.
    void apply(const Rgbaf* src, Rgbaf* dst, std::size_t count) const noexcept;
    void applyInPlace(std::span<Rgbaf> pixels) const noexcept;

    [[nodiscard]] void* nativeHandle() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

private:
    explicit ColorTransform(void* handle) noexcept : handle_(handle) {}
    void* handle_ = nullptr;  // cmsHTRANSFORM
};

using ColorTransformRef = std::shared_ptr<ColorTransform>;

}  // namespace pe
