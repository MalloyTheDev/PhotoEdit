#pragma once

#include "pe/core/PixelFormat.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>

namespace pe {

// The four ICC rendering intents. Values match lcms2's INTENT_* so the wrapper can
// pass them straight through. See docs/systems/15-color-management.md.
enum class RenderingIntent : uint8_t {
    Perceptual = 0,            // compress whole gamut pleasingly; photos
    RelativeColorimetric = 1,  // map in-gamut exactly, clip out-of-gamut; default
    Saturation = 2,            // maximise saturation; charts/graphics
    AbsoluteColorimetric = 3,  // exact incl. white point; proofing paper white
};

// The built-in RGB working spaces the engine ships. Constructed from their
// primaries/white point/transfer curve (no ICC files needed).
enum class BuiltinSpace : uint8_t {
    sRGB,          // sRGB, D65, sRGB transfer
    sRGBLinear,    // sRGB primaries, D65, linear (gamma 1.0)
    DisplayP3,     // P3 primaries, D65, sRGB transfer
    AdobeRGB1998,  // Adobe RGB primaries, D65, gamma 2.19921875
    ProPhotoRGB,   // ProPhoto primaries, D50, gamma 1.8
};

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
    // Any of the built-in RGB working spaces (constructed from primaries/curve).
    [[nodiscard]] static std::shared_ptr<ColorProfile> builtin(BuiltinSpace space);
    // Parse an embedded ICC profile from raw bytes; nullptr if invalid/empty.
    [[nodiscard]] static std::shared_ptr<ColorProfile> fromIccData(std::span<const std::byte> icc);

    [[nodiscard]] ColorMode mode() const noexcept;  // RGB/CMYK/Gray/Lab
    [[nodiscard]] std::string description() const;  // human-readable label
    [[nodiscard]] void* nativeHandle() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

private:
    explicit ColorProfile(void* handle) noexcept : handle_(handle) {}
    // Wrap a raw cmsHPROFILE (or null) into a ref-counted ColorProfile (or null).
    [[nodiscard]] static std::shared_ptr<ColorProfile> fromHandle(void* handle);
    void* handle_ = nullptr;  // cmsHPROFILE
};

using ColorProfileRef = std::shared_ptr<ColorProfile>;

}  // namespace pe
