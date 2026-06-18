#pragma once

#include "pe/core/PixelBuffer.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pe {

class Document;

// Path/format-level document I/O: the layer the application's Open/Save use, sitting
// on top of the byte-level codecs. See docs/systems/16-file-formats.md.
enum class ImageFormat : std::uint8_t { Unknown, Png, Jpeg, Tiff, WebP, Native };

// Per-format encode options for export/save. Fields not relevant to the chosen format are
// ignored (only JPEG is lossy/tunable today; the lossless codecs have no knobs). Defaults
// match the codecs' own defaults, so the no-options overloads below behave unchanged.
struct ExportOptions {
    int jpegQuality = 90;  // 1..100 (clamped by the JPEG encoder); ignored by other formats
};

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
// The first overload uses default encode options; the second honors `opts` (e.g. JPEG quality).
[[nodiscard]] std::vector<std::byte> exportDocument(const Document& doc, ImageFormat fmt);
[[nodiscard]] std::vector<std::byte> exportDocument(const Document& doc, ImageFormat fmt,
                                                    const ExportOptions& opts);

// --- filesystem convenience (the layer the app's Open/Save call) ---
// Load a document from a file on disk; the format is inferred from the extension.
// Returns nullptr on a missing/unreadable file, an unknown extension, a file larger
// than the read cap, or malformed/oversized content.
[[nodiscard]] std::unique_ptr<Document> loadDocument(const std::string& path);

// Save a document to a file on disk; the format is inferred from the extension.
// Returns false on an unknown extension, an encode failure, or a write error. The first
// overload uses default encode options; the second honors `opts` (e.g. JPEG quality).
[[nodiscard]] bool saveDocument(const Document& doc, const std::string& path);
[[nodiscard]] bool saveDocument(const Document& doc, const std::string& path,
                                const ExportOptions& opts);

}  // namespace pe
