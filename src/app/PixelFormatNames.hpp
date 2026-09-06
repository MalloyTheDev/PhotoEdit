#pragma once

#include "pe/core/PixelFormat.hpp"

// User-facing names for the engine's pixel-format enums. Presentation only, so it
// lives in the app rather than the engine. Shared because the document identity in
// the title strip, the Properties panel and the zoom strip all render the same two
// enums and were drifting toward three private copies.

namespace pe::app {

// "RGB", "CMYK", ... as an Image > Mode menu would spell them.
[[nodiscard]] inline const char* colorModeName(pe::ColorMode mode) noexcept {
    switch (mode) {
        case pe::ColorMode::RGB:
            return "RGB";
        case pe::ColorMode::CMYK:
            return "CMYK";
        case pe::ColorMode::Gray:
            return "Gray";
        case pe::ColorMode::Lab:
            return "Lab";
        case pe::ColorMode::Indexed:
            return "Indexed";
        case pe::ColorMode::Bitmap:
            return "Bitmap";
    }
    return "RGB";
}

// Bits per channel as a number, for the compact "RGB/8" form.
[[nodiscard]] inline int bitDepthBits(pe::BitDepth depth) noexcept {
    switch (depth) {
        case pe::BitDepth::U8:
            return 8;
        case pe::BitDepth::U16:
            return 16;
        case pe::BitDepth::F32:
            return 32;
    }
    return 8;
}

// The long form used where there is room for it.
[[nodiscard]] inline const char* bitDepthName(pe::BitDepth depth) noexcept {
    switch (depth) {
        case pe::BitDepth::U8:
            return "8 Bits/Channel";
        case pe::BitDepth::U16:
            return "16 Bits/Channel";
        case pe::BitDepth::F32:
            return "32 Bits/Channel";
    }
    return "8 Bits/Channel";
}

}  // namespace pe::app
