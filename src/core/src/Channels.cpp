#include "pe/core/Channels.hpp"

#include <cstddef>

namespace pe {

namespace {
std::uint8_t channelValue(Rgba8 p, Channel channel) noexcept {
    switch (channel) {
        case Channel::Green:
            return p.g;
        case Channel::Blue:
            return p.b;
        case Channel::Alpha:
            return p.a;
        case Channel::Red:
        default:
            return p.r;
    }
}
}  // namespace

PixelBuffer extractChannel(const PixelBuffer& img, Channel channel) {
    PixelBuffer out(img.width(), img.height());
    if (img.isEmpty()) return out;
    const std::size_t n =
        static_cast<std::size_t>(img.width()) * static_cast<std::size_t>(img.height());
    const Rgba8* src = img.data();
    Rgba8* dst = out.data();
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t v = channelValue(src[i], channel);
        dst[i] = Rgba8{v, v, v, 255};  // grayscale, opaque
    }
    return out;
}

PixelBuffer mergeChannels(const PixelBuffer& red, const PixelBuffer& green, const PixelBuffer& blue,
                          const PixelBuffer& alpha) {
    const int w = red.width();
    const int h = red.height();
    if (w <= 0 || h <= 0) return PixelBuffer{};
    if (green.width() != w || green.height() != h || blue.width() != w || blue.height() != h) {
        return PixelBuffer{};
    }
    const bool hasAlpha = !alpha.isEmpty();
    if (hasAlpha && (alpha.width() != w || alpha.height() != h)) return PixelBuffer{};

    PixelBuffer out(w, h);
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    const Rgba8* r = red.data();
    const Rgba8* g = green.data();
    const Rgba8* b = blue.data();
    const Rgba8* a = hasAlpha ? alpha.data() : nullptr;
    Rgba8* dst = out.data();
    for (std::size_t i = 0; i < n; ++i) {
        dst[i] =
            Rgba8{r[i].r, g[i].r, b[i].r, a != nullptr ? a[i].r : static_cast<std::uint8_t>(255)};
    }
    return out;
}

}  // namespace pe
