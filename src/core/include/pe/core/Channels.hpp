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

// Which colour channels the display shows. Visibility is display state, not a pixel edit:
// it changes what the canvas draws and nothing about what is stored. See
// docs/systems/19-channels.md, "Per-channel visibility".
struct ChannelView {
    bool red = true;
    bool green = true;
    bool blue = true;

    [[nodiscard]] constexpr bool showsAll() const noexcept { return red && green && blue; }
    [[nodiscard]] constexpr int count() const noexcept {
        return (red ? 1 : 0) + (green ? 1 : 0) + (blue ? 1 : 0);
    }
    [[nodiscard]] constexpr bool operator==(const ChannelView&) const noexcept = default;
};

// Apply a channel view to an already-composited raster, in place:
//
//   all three visible - unchanged, the ordinary composite
//   exactly one       - that channel replicated to R=G=B, the grayscale single-channel view.
//                       Grayscale rather than tinted because inspecting a channel is reading
//                       its VALUES (where the noise is, which one makes the cleanest mask),
//                       and a red tint over the red channel makes those values harder to
//                       judge, not easier
//   two visible       - the hidden channel zeroed, so the pair still reads as colour
//   none              - black
//
// Alpha is never touched. Hiding a colour channel must not turn transparent pixels opaque,
// or the checkerboard would disappear the moment a channel view was switched on.
void applyChannelView(PixelBuffer& img, ChannelView view);

}  // namespace pe
