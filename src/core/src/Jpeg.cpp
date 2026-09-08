#include "pe/core/ImageIO.hpp"

#include <turbojpeg.h>

#include <cstdint>
#include <cstring>
#include <memory>

namespace pe {

namespace {
// Cap decoded dimensions before allocating (untrusted input); 64 MP of RGBA8 = 256 MB.
constexpr std::int64_t kMaxImagePixels = 64'000'000;

// TurboJPEG's handle and its malloc'd output buffer, owned rather than hand-released.
//
// The explicit tjDestroy/tjFree calls covered every path the code could SEE. What they
// missed is the one it could not: a PixelBuffer or std::vector allocation of up to 256 MB
// sits between acquiring these and releasing them, and a throw there skipped the release.
// The shell's worker thread catches that exception and returns the app to a usable state,
// so the user retries the same large file and leaks the handle again.
struct TjHandleDeleter {
    void operator()(void* h) const noexcept { tjDestroy(h); }
};
using TjHandle = std::unique_ptr<void, TjHandleDeleter>;

struct TjBufferDeleter {
    void operator()(unsigned char* p) const noexcept { tjFree(p); }
};
using TjBuffer = std::unique_ptr<unsigned char, TjBufferDeleter>;
}  // namespace

std::vector<std::byte> encodeJpeg(const PixelBuffer& image, int quality) {
    if (image.isEmpty()) return {};

    const TjHandle handle(tjInitCompress());
    if (handle == nullptr) return {};

    const int q = quality < 1 ? 1 : (quality > 100 ? 100 : quality);
    unsigned char* jpegBuf = nullptr;  // allocated by TurboJPEG
    unsigned long jpegSize = 0;
    // TJPF_RGBA reads 4-byte R,G,B,A pixels (alpha ignored — JPEG is opaque). TJSAMP_444
    // keeps full chroma resolution for accurate color.
    const int rc = tjCompress2(handle.get(), reinterpret_cast<const unsigned char*>(image.data()),
                               image.width(), 0 /*pitch=width*4*/, image.height(), TJPF_RGBA,
                               &jpegBuf, &jpegSize, TJSAMP_444, q, 0);
    const TjBuffer owned(jpegBuf);  // takes ownership whether or not the compress succeeded

    std::vector<std::byte> out;
    if (rc == 0 && owned != nullptr) {
        out.resize(jpegSize);
        std::memcpy(out.data(), owned.get(), jpegSize);
    }
    return out;
}

std::optional<PixelBuffer> decodeJpeg(std::span<const std::byte> data) {
    if (data.empty()) return std::nullopt;

    const TjHandle handle(tjInitDecompress());
    if (handle == nullptr) return std::nullopt;

    const auto* jpeg = reinterpret_cast<const unsigned char*>(data.data());
    const auto jpegSize = static_cast<unsigned long>(data.size());

    int width = 0;
    int height = 0;
    int subsamp = 0;
    int colorspace = 0;
    if (tjDecompressHeader3(handle.get(), jpeg, jpegSize, &width, &height, &subsamp, &colorspace) !=
            0 ||
        width <= 0 || height <= 0) {
        return std::nullopt;
    }
    if (static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) >
        static_cast<std::uint64_t>(kMaxImagePixels)) {
        return std::nullopt;
    }

    PixelBuffer out(width, height);
    // TJPF_RGBA writes 4-byte R,G,B,A; alpha is set to 0xFF (opaque). pitch 0 == width*4.
    const int rc =
        tjDecompress2(handle.get(), jpeg, jpegSize, reinterpret_cast<unsigned char*>(out.data()),
                      width, 0, height, TJPF_RGBA, 0);
    if (rc != 0) return std::nullopt;
    return out;
}

}  // namespace pe
