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
enum class ImageFormat : std::uint8_t { Unknown, Png, Jpeg, Tiff, WebP, Psd, Native };

// Per-format encode options for export/save. Fields not relevant to the chosen format are
// ignored (only JPEG is lossy/tunable today; the lossless codecs have no knobs). Defaults
// match the codecs' own defaults, so the no-options overloads below behave unchanged.
struct ExportOptions {
    int jpegQuality = 90;  // 1..100 (clamped by the JPEG encoder); ignored by other formats
};

// Map a file path (or bare extension) to a format, case-insensitively. ".png"->Png,
// ".jpg"/".jpeg"->Jpeg, ".tif"/".tiff"->Tiff, ".webp"->WebP, ".psd"->Psd, ".pedoc"->Native;
// else Unknown.
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

// Why a load failed. Distinguishing these is the difference between "you cannot read
// this file" and "this file is not a supported format", which a user needs in order to
// know whether the situation is recoverable.
enum class LoadError : std::uint8_t {
    None,
    UnsupportedFormat,  // extension not recognized, or its codec is not in this build
    NotFound,           // no such file or directory
    PermissionDenied,   // exists, but cannot be opened for reading
    TooLarge,           // above the read cap, so it is never loaded into memory
    Truncated,          // shorter than its own header/records claim
    DecodeFailed,       // well-formed enough to read, but the decoder rejected it
};

// Why a save failed. The save path is the one where a bad diagnosis costs work: a user
// losing a document to a full disk must not be told the same thing as one who picked an
// extension this build cannot write.
enum class SaveError : std::uint8_t {
    None,
    UnsupportedFormat,  // extension not recognized
    CodecUnavailable,   // recognized, but that codec is not compiled into this build
    TooLargeToFlatten,  // over the composite cap; every raster format goes through it
    ContentOutOfRange,  // native only: a layer's content reaches past the coordinate range
                        // the format can store, so writing it would produce a file this
                        // build's own reader refuses. Refused while the work is still in
                        // memory rather than written and lost.
    CannotCreate,       // the temp file could not be created (permissions, read-only, path)
    WriteFailed,        // ran out of space, or the device reported an error mid-write
    ReplaceFailed,      // written, but could not replace the destination
};

// Load a document from a file on disk; the format is inferred from the extension.
// Returns nullptr on failure; pass `err` to learn which failure.
[[nodiscard]] std::unique_ptr<Document> loadDocument(const std::string& path,
                                                     LoadError* err = nullptr);

// Save a document to a file on disk; the format is inferred from the extension.
// Returns false on failure; pass `err` to learn which failure. The first overload uses
// default encode options; the second honors `opts` (e.g. JPEG quality).
[[nodiscard]] bool saveDocument(const Document& doc, const std::string& path,
                                SaveError* err = nullptr);
[[nodiscard]] bool saveDocument(const Document& doc, const std::string& path,
                                const ExportOptions& opts, SaveError* err = nullptr);

}  // namespace pe
