#pragma once

#include "pe/core/PixelBuffer.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace pe {

class Document;

// PNG codec over libpng's simplified API. The header is libpng-free; the codec is
// compiled only when libpng is available (PHOTOEDIT_HAVE_PNG) — guard usage with the
// macro. See docs/systems/16-file-formats.md.

// Encode an 8-bit RGBA image to the bytes of a PNG file. Returns an empty vector on
// failure or for an empty image.
[[nodiscard]] std::vector<std::byte> encodePng(const PixelBuffer& image);

// Decode the bytes of a PNG file to an 8-bit RGBA image. Returns nullopt on malformed
// input or if the declared dimensions exceed the decode safety cap (untrusted input).
[[nodiscard]] std::optional<PixelBuffer> decodePng(std::span<const std::byte> data);

// Encode an 8-bit RGBA image to the bytes of a JPEG file. JPEG is opaque, so the
// alpha channel is dropped. `quality` is 1..100 (clamped). Empty on failure or for an
// empty image. Only built with libjpeg-turbo (PHOTOEDIT_HAVE_JPEG).
[[nodiscard]] std::vector<std::byte> encodeJpeg(const PixelBuffer& image, int quality = 90);

// Decode the bytes of a JPEG file to an 8-bit RGBA image (alpha set opaque). Returns
// nullopt on malformed input or if the dimensions exceed the decode safety cap.
[[nodiscard]] std::optional<PixelBuffer> decodeJpeg(std::span<const std::byte> data);

// Document-level convenience over the codec:
// Flatten `doc` to a single raster and encode it as the bytes of a PNG file. Returns
// an empty vector on failure (e.g. an empty document).
[[nodiscard]] std::vector<std::byte> exportDocumentPng(const Document& doc);
// Decode PNG bytes into a new single-(pixel-)layer document sized to the image.
// Returns nullptr on malformed input or an image past the decode cap.
[[nodiscard]] std::unique_ptr<Document> importDocumentPng(std::span<const std::byte> data);

}  // namespace pe
