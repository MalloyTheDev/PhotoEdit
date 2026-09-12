#include "pe/core/ImageIO.hpp"

#include "pe/core/Document.hpp"
#include "pe/core/DocumentIO.hpp"
#include "pe/core/PixelLayer.hpp"

#include <png.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace pe {

namespace {
// Cap decoded dimensions so a malicious/huge PNG header can't trigger an enormous
// allocation. 64 MP of RGBA8 is 256 MB — the same budget the rest of the engine uses.
constexpr std::int64_t kMaxImagePixels = 64'000'000;

// Frees whatever libpng hung off `png_image` however this scope ends.
//
// The explicit calls this replaces covered every path the code could SEE, but not the one
// it could not: the buffers allocated between acquiring the struct's internals and freeing
// them are up to 256 MB, and a throw there skipped the free entirely. That is not a
// process-ending leak either, which is what makes it worth fixing: decoding runs on the
// shell's worker thread, which catches the exception and returns the app to a usable state,
// so the user retries the same large file and leaks again.
//
// png_image_free is idempotent and a no-op on a zeroed struct, so the guard is safe to arm
// before libpng has allocated anything.
class PngImageGuard {
public:
    explicit PngImageGuard(png_image& image) noexcept : image_(image) {}
    ~PngImageGuard() { png_image_free(&image_); }
    PngImageGuard(const PngImageGuard&) = delete;
    PngImageGuard& operator=(const PngImageGuard&) = delete;

private:
    png_image& image_;
};
}  // namespace

std::vector<std::byte> encodePng(const PixelBuffer& image) {
    if (image.isEmpty()) return {};

    png_image png;
    std::memset(&png, 0, sizeof(png));
    const PngImageGuard guard(png);
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

namespace {
// libpng write callback: append to the std::vector hung off the io ptr.
void pngAppendToVector(png_structp png, png_bytep data, png_size_t len) {
    auto* out = static_cast<std::vector<std::byte>*>(png_get_io_ptr(png));
    const auto* p = reinterpret_cast<const std::byte*>(data);
    out->insert(out->end(), p, p + len);
}
void pngNoopFlush(png_structp /*png*/) {}
}  // namespace

std::vector<std::byte> encodePngStreamed(int width, int height, int bandRows,
                                         const std::function<PixelBuffer(int, int)>& band) {
    if (width <= 0 || height <= 0 || bandRows <= 0) return {};
    // The full write API (not the simplified one encodePng uses), because only png_write_row lets
    // the image arrive a band at a time instead of as one buffer.
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (png == nullptr) return {};
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_write_struct(&png, nullptr);
        return {};
    }
    std::vector<std::byte> out;
    // libpng reports errors by longjmp'ing here; every path below then cleans up and returns empty.
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        return {};
    }
    png_set_write_fn(png, &out, pngAppendToVector, pngNoopFlush);
    png_set_IHDR(png, info, static_cast<png_uint_32>(width), static_cast<png_uint_32>(height), 8,
                 PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (int y = 0; y < height; y += bandRows) {
        const int rows = std::min(bandRows, height - y);
        const PixelBuffer b = band(y, rows);
        if (b.width() != width || b.height() != rows) {  // band provider failed
            png_destroy_write_struct(&png, &info);
            return {};
        }
        for (int r = 0; r < rows; ++r) {
            png_write_row(png, reinterpret_cast<png_const_bytep>(
                                   b.data() + static_cast<std::size_t>(r) * width));
        }
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    return out;
}

std::optional<PixelBuffer> decodePng(std::span<const std::byte> data) {
    if (data.empty()) return std::nullopt;

    png_image png;
    std::memset(&png, 0, sizeof(png));
    png.version = PNG_IMAGE_VERSION;
    const PngImageGuard guard(png);
    if (png_image_begin_read_from_memory(&png, data.data(), data.size()) == 0) {
        return std::nullopt;  // not a PNG / malformed header
    }

    // Reject oversized images before allocating the output buffer. The product uses
    // uint64_t so it cannot overflow for any png_uint_32 pair (0xFFFFFFFF^2 fits) —
    // the bound is self-evident without relying on libpng's internal dimension limits.
    if (static_cast<std::uint64_t>(png.width) * static_cast<std::uint64_t>(png.height) >
        static_cast<std::uint64_t>(kMaxImagePixels)) {
        return std::nullopt;
    }

    png.format = PNG_FORMAT_RGBA;
    PixelBuffer out(static_cast<int>(png.width), static_cast<int>(png.height));
    // row_stride 0 == default (width * 4); out's storage is exactly width*height*4.
    const int ok = png_image_finish_read(&png, nullptr, out.data(), 0, nullptr);
    if (ok == 0) return std::nullopt;
    return out;
}

std::vector<std::byte> exportDocumentPng(const Document& doc) {
    return encodePng(doc.compositeImage());  // flatten -> encode
}

std::unique_ptr<Document> importDocumentPng(std::span<const std::byte> data) {
    std::optional<PixelBuffer> image = decodePng(data);
    if (!image) return nullptr;
    return documentFromImage(*image);  // shared raster -> single-layer document
}

}  // namespace pe
