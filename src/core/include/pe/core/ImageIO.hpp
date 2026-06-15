#pragma once

#include "pe/core/PixelBuffer.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace pe {

// PNG codec over libpng's simplified API. The header is libpng-free; the codec is
// compiled only when libpng is available (PHOTOEDIT_HAVE_PNG) — guard usage with the
// macro. See docs/systems/16-file-formats.md.

// Encode an 8-bit RGBA image to the bytes of a PNG file. Returns an empty vector on
// failure or for an empty image.
[[nodiscard]] std::vector<std::byte> encodePng(const PixelBuffer& image);

// Decode the bytes of a PNG file to an 8-bit RGBA image. Returns nullopt on malformed
// input or if the declared dimensions exceed the decode safety cap (untrusted input).
[[nodiscard]] std::optional<PixelBuffer> decodePng(std::span<const std::byte> data);

}  // namespace pe
