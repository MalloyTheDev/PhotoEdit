#include "pe/core/ImageIO.hpp"

#include <png.h>

#include <cstdint>
#include <cstring>

namespace pe {

namespace {
// Cap decoded dimensions so a malicious/huge PNG header can't trigger an enormous
// allocation. 64 MP of RGBA8 is 256 MB — the same budget the rest of the engine uses.
constexpr std::int64_t kMaxImagePixels = 64'000'000;
}  // namespace

std::vector<std::byte> encodePng(const PixelBuffer& image) {
    if (image.isEmpty()) return {};

    png_image png;
    std::memset(&png, 0, sizeof(png));
    png.version = PNG_IMAGE_VERSION;
    png.width = static_cast<png_uint_32>(image.width());
    png.height = static_cast<png_uint_32>(image.height());
    png.format = PNG_FORMAT_RGBA;  // 4 bytes/pixel R,G,B,A — matches Rgba8

    // First call with a null buffer computes the required size; the second writes.
    png_alloc_size_t size = 0;
    if (png_image_write_to_memory(&png, nullptr, &size, 0, image.data(), 0, nullptr) == 0) {
        return {};
    }
    std::vector<std::byte> out(static_cast<std::size_t>(size));
    if (png_image_write_to_memory(&png, out.data(), &size, 0, image.data(), 0, nullptr) == 0) {
        return {};
    }
    out.resize(static_cast<std::size_t>(size));
    return out;
}

std::optional<PixelBuffer> decodePng(std::span<const std::byte> data) {
    if (data.empty()) return std::nullopt;

    png_image png;
    std::memset(&png, 0, sizeof(png));
    png.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_memory(&png, data.data(), data.size()) == 0) {
        return std::nullopt;  // not a PNG / malformed header
    }

    // Reject oversized images before allocating the output buffer. The product uses
    // uint64_t so it cannot overflow for any png_uint_32 pair (0xFFFFFFFF^2 fits) —
    // the bound is self-evident without relying on libpng's internal dimension limits.
    if (static_cast<std::uint64_t>(png.width) * static_cast<std::uint64_t>(png.height) >
        static_cast<std::uint64_t>(kMaxImagePixels)) {
        png_image_free(&png);
        return std::nullopt;
    }

    png.format = PNG_FORMAT_RGBA;
    PixelBuffer out(static_cast<int>(png.width), static_cast<int>(png.height));
    // row_stride 0 == default (width * 4); out's storage is exactly width*height*4.
    const int ok = png_image_finish_read(&png, nullptr, out.data(), 0, nullptr);
    png_image_free(&png);  // safe in all cases (idempotent)
    if (ok == 0) return std::nullopt;
    return out;
}

}  // namespace pe
