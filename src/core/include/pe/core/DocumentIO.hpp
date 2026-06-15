#pragma once

#include "pe/core/PixelBuffer.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace pe {

class Document;

// Path/format-level document I/O: the layer the application's Open/Save use, sitting
// on top of the byte-level codecs. See docs/systems/16-file-formats.md.
enum class ImageFormat : std::uint8_t { Unknown, Png, Jpeg, Tiff, WebP, Native };

// Map a file path (or bare extension) to a format, case-insensitively. ".png"->Png,
// ".jpg"/".jpeg"->Jpeg, ".tif"/".tiff"->Tiff, ".webp"->WebP, ".pedoc"->Native; else
// Unknown.
[[nodiscard]] ImageFormat formatFromExtension(std::string_view path);

// Whether this binary was built with the codec for `fmt` (Native is always available).
[[nodiscard]] bool formatAvailable(ImageFormat fmt);

// Build a new single-(pixel-)layer document from a raster. nullptr if the image is
// empty or its size exceeds the document's canvas limits.
[[nodiscard]] std::unique_ptr<Document> documentFromImage(const PixelBuffer& image);

// Decode file bytes into a document. Native uses the layered .pedoc reader; raster
// formats decode and flatten into one layer. Returns nullptr on malformed input or if
// the codec for `fmt` is not built into this binary.
[[nodiscard]] std::unique_ptr<Document> importDocument(std::span<const std::byte> data,
                                                       ImageFormat fmt);

// Encode a document to file bytes. Native preserves the full layer tree; raster
// formats flatten (composite) to a single image. Empty on failure or unavailable codec.
[[nodiscard]] std::vector<std::byte> exportDocument(const Document& doc, ImageFormat fmt);

}  // namespace pe
