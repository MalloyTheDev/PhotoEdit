#include "pe/core/ImageIO.hpp"

#include <webp/decode.h>
#include <webp/encode.h>

#include <cstdint>
#include <cstring>
#include <memory>

namespace pe {

namespace {
struct WebPDeleter {
    void operator()(std::uint8_t* p) const noexcept { WebPFree(p); }
};
using WebPBuffer = std::unique_ptr<std::uint8_t, WebPDeleter>;
}  // namespace

namespace {
// Cap decoded dimensions before allocating (untrusted input); 64 MP of RGBA8 = 256 MB.
constexpr std::int64_t kMaxImagePixels = 64'000'000;
}  // namespace

std::vector<std::byte> encodeWebp(const PixelBuffer& image) {
    if (image.isEmpty()) return {};

    std::uint8_t* output = nullptr;  // allocated by libwebp
    const std::size_t n =
        WebPEncodeLosslessRGBA(reinterpret_cast<const std::uint8_t*>(image.data()), image.width(),
                               image.height(), image.width() * 4 /*stride*/, &output);
    // Owned from here: result.resize below can throw on a large image, and the explicit
    // WebPFree it replaces was after it.
    const WebPBuffer owned(output);

    std::vector<std::byte> result;
    if (n > 0 && owned != nullptr) {
        result.resize(n);
        std::memcpy(result.data(), owned.get(), n);
    }
    return result;
}

std::optional<PixelBuffer> decodeWebp(std::span<const std::byte> data) {
    if (data.empty()) return std::nullopt;

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
    int w = 0;
    int h = 0;
    // Read just the header first so an oversized image is rejected before any decode
    // buffer is allocated.
    if (WebPGetInfo(bytes, data.size(), &w, &h) == 0 || w <= 0 || h <= 0) return std::nullopt;
    if (static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h) >
        static_cast<std::uint64_t>(kMaxImagePixels)) {
        return std::nullopt;
    }

    PixelBuffer out(w, h);
    // Decode straight into the pre-sized buffer (one allocation). Returns the buffer
    // pointer on success, null on failure (truncated / corrupt stream).
    const std::size_t bufSize = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4;
    if (WebPDecodeRGBAInto(bytes, data.size(), reinterpret_cast<std::uint8_t*>(out.data()), bufSize,
                           w * 4 /*stride*/) == nullptr) {
        return std::nullopt;
    }
    return out;
}

}  // namespace pe
