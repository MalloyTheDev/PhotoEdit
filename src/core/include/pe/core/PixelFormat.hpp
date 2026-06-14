#pragma once

#include <cstdint>

namespace pe {

// The document/layer color model and storage bit depth. These are pixel-format
// concepts shared by the document, layers, and the color-management system
// (docs/systems/15-color-management.md), so they live in this lightweight header
// rather than depending on the heavier Document header.
enum class ColorMode : uint8_t { RGB = 0, CMYK, Gray, Lab, Indexed, Bitmap };

// Storage depth of a layer's pixels. Compositing/adjustment math is always float
// regardless of storage depth; this only selects how pixels are stored.
enum class BitDepth : uint8_t { U8 = 8, U16 = 16, F32 = 32 };

}  // namespace pe
