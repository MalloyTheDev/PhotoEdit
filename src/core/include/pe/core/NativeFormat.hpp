#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pe {

class Document;

// The native layered document format (.pedoc) — a self-contained binary serialization
// that, unlike a flattened PNG/JPEG export, preserves the layer stack and per-layer
// properties. See docs/systems/16-file-formats.md.
//
// Scope: canvas (size, color mode, bit depth, resolution), the tree of PIXEL and GROUP
// layers, and per-layer masks — each layer's visibility, opacity, blend mode, name, and
// pixel content at the document's bit depth (8-bit, 16-bit, or 32-bit float; the matching
// store round-trips exactly). Adjustment/fill/text layers and embedded ICC profiles
// round-trip in later increments (non-serializable layers are skipped).
//
// The byte order is little-endian (the engine's target platforms). The reader is
// fully bounds-checked: any truncated, oversized, or inconsistent input yields nullptr
// rather than a crash, so it is safe on untrusted files.

// Default aggregate cap on the total decompressed layer + mask pixel bytes a single
// deserialize will allocate. Each block is independently bounded (content rect clamped
// to the canvas and to a per-layer pixel cap), but without an aggregate cap a small
// crafted file declaring many layers — or a zlib decompression bomb — could request
// unbounded memory. The cap is checked BEFORE each allocation, so an over-budget file is
// rejected (nullptr) rather than driven to bad_alloc. Hosts may tune it per environment.
inline constexpr std::int64_t kMaxNativeContentBytes = 4'000'000'000;  // ~3.7 GiB

[[nodiscard]] std::vector<std::byte> serializeDocument(const Document& doc);
[[nodiscard]] std::unique_ptr<Document> deserializeDocument(
    std::span<const std::byte> data, std::int64_t maxTotalContentBytes = kMaxNativeContentBytes);

}  // namespace pe
