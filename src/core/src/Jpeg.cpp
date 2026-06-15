#include "pe/core/ImageIO.hpp"

#include <turbojpeg.h>

#include <cstdint>
#include <cstring>

namespace pe {

namespace {
// Cap decoded dimensions before allocating (untrusted input); 64 MP of RGBA8 = 256 MB.
constexpr std::int64_t kMaxImagePixels = 64'000'000;
}  // namespace

std::vector<std::byte> encodeJpeg(const PixelBuffer& image, int quality) {
    if (image.isEmpty()) return {};

    tjhandle handle = tjInitCompress();
    if (handle == nullptr) return {};

    const int q = quality < 1 ? 1 : (quality > 100 ? 100 : quality);
    unsigned char* jpegBuf = nullptr;  // allocated by TurboJPEG
    unsigned long jpegSize = 0;
    // TJPF_RGBA reads 4-byte R,G,B,A pixels (alpha ignored — JPEG is opaque). TJSAMP_444
    // keeps full chroma resolution for accurate color.
    const int rc = tjCompress2(handle, reinterpret_cast<const unsigned char*>(image.data()),
                               image.width(), 0 /*pitch=width*4*/, image.height(), TJPF_RGBA,
                               &jpegBuf, &jpegSize, TJSAMP_444, q, 0);

    std::vector<std::byte> out;
    if (rc == 0 && jpegBuf != nullptr) {
        out.resize(jpegSize);
        std::memcpy(out.data(), jpegBuf, jpegSize);
    }
    tjFree(jpegBuf);
    tjDestroy(handle);
    return out;
}

std::optional<PixelBuffer> decodeJpeg(std::span<const std::byte> data) {
    if (data.empty()) return std::nullopt;

    tjhandle handle = tjInitDecompress();
    if (handle == nullptr) return std::nullopt;

    const auto* jpeg = reinterpret_cast<const unsigned char*>(data.data());
    const auto jpegSize = static_cast<unsigned long>(data.size());

    int width = 0;
    int height = 0;
    int subsamp = 0;
    int colorspace = 0;
    if (tjDecompressHeader3(handle, jpeg, jpegSize, &width, &height, &subsamp, &colorspace) != 0 ||
        width <= 0 || height <= 0) {
        tjDestroy(handle);
        return std::nullopt;
    }
    if (static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) >
        static_cast<std::uint64_t>(kMaxImagePixels)) {
        tjDestroy(handle);
        return std::nullopt;
    }

    PixelBuffer out(width, height);
    // TJPF_RGBA writes 4-byte R,G,B,A; alpha is set to 0xFF (opaque). pitch 0 == width*4.
    const int rc =
        tjDecompress2(handle, jpeg, jpegSize, reinterpret_cast<unsigned char*>(out.data()), width,
                      0, height, TJPF_RGBA, 0);
    tjDestroy(handle);
    if (rc != 0) return std::nullopt;
    return out;
}

}  // namespace pe
