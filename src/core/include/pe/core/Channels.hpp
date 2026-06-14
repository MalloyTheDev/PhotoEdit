#pragma once

#include "pe/core/PixelBuffer.hpp"

#include <cstdint>

namespace pe {

// The addressable channels of an RGBA raster. Spot channels and saved-selection
// alpha channels (the rest of the channels system) build on this. See
// docs/systems/19-channels.md.
enum class Channel : uint8_t { Red, Green, Blue, Alpha };

// Extract one channel of `img` as a grayscale, fully-opaque raster: the channel's
// 8-bit value is replicated to R=G=B with alpha 255 (the Channels-panel single-
// channel view). Empty input yields an empty buffer.
[[nodiscard]] PixelBuffer extractChannel(const PixelBuffer& img, Channel channel);

// Merge four single-channel (grayscale) sources into one RGBA image: each source
// contributes its RED channel as the corresponding output channel (grayscale
// sources have R==G==B, so this is the gray value). All sources must share the
// red/green/blue dimensions (a mismatch yields an empty buffer). An empty alpha
// source defaults the output alpha to fully opaque. This is the inverse of
// extractChannel: merge(split(img)) reproduces img.
[[nodiscard]] PixelBuffer mergeChannels(const PixelBuffer& red, const PixelBuffer& green,
                                        const PixelBuffer& blue,
                                        const PixelBuffer& alpha = PixelBuffer{});

}  // namespace pe
