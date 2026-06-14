#pragma once

#include "pe/core/PixelFormat.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>

namespace pe {

// Wraps a loaded ICC profile (an lcms2 `cmsHPROFILE` under the hood). Immutable;
// shared by ref-counted handle so many transforms/documents reuse one profile.
// See docs/systems/15-color-management.md and ADR-0004.
//
// This header is intentionally lcms2-free (the native profile is held as an opaque
// `void*`), so it can be included regardless of whether the engine was built with
// color management. The implementation (ColorProfile.cpp) is compiled only when
// lcms2 is available; guard usage with `#ifdef PHOTOEDIT_HAVE_LCMS2`.
class ColorProfile {
public:
    ~ColorProfile();
    ColorProfile(const ColorProfile&) = delete;
    ColorProfile& operator=(const ColorProfile&) = delete;

    // The built-in sRGB working space.
    [[nodiscard]] static std::shared_ptr<ColorProfile> sRGB();
    // Parse an embedded ICC profile from raw bytes; nullptr if invalid/empty.
    [[nodiscard]] static std::shared_ptr<ColorProfile> fromIccData(std::span<const std::byte> icc);

    [[nodiscard]] ColorMode mode() const noexcept;  // RGB/CMYK/Gray/Lab
    [[nodiscard]] std::string description() const;  // human-readable label
    [[nodiscard]] void* nativeHandle() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

private:
    explicit ColorProfile(void* handle) noexcept : handle_(handle) {}
    void* handle_ = nullptr;  // cmsHPROFILE
};

using ColorProfileRef = std::shared_ptr<ColorProfile>;

}  // namespace pe
