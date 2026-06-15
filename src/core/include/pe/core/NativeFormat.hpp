#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace pe {

class Document;

// The native layered document format (.pedoc) — a self-contained binary serialization
// that, unlike a flattened PNG/JPEG export, preserves the layer stack and per-layer
// properties. See docs/systems/16-file-formats.md.
//
// v1 scope: canvas (size, color mode, bit depth, resolution) and the flat list of
// top-level PIXEL layers — each layer's visibility, opacity, blend mode, name, and
// 8-bit pixel content. Groups, masks, adjustment/fill layers, and embedded ICC
// profiles round-trip in later increments (non-pixel layers are skipped for now).
//
// The byte order is little-endian (the engine's target platforms). The reader is
// fully bounds-checked: any truncated, oversized, or inconsistent input yields nullptr
// rather than a crash, so it is safe on untrusted files.

[[nodiscard]] std::vector<std::byte> serializeDocument(const Document& doc);
[[nodiscard]] std::unique_ptr<Document> deserializeDocument(std::span<const std::byte> data);

}  // namespace pe
