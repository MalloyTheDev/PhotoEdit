#include "pe/core/PixelBuffer.hpp"

#include <cstddef>

namespace pe {

PixelBuffer::PixelBuffer(int width, int height, Rgba8 fill)
    : width_(width < 0 ? 0 : width), height_(height < 0 ? 0 : height) {
    const std::size_t count =
        static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
    pixels_.assign(count, fill);
}

Rgba8 PixelBuffer::at(int x, int y) const noexcept {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return Rgba8{};
    return pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                   static_cast<std::size_t>(x)];
}

void PixelBuffer::set(int x, int y, Rgba8 c) noexcept {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
    pixels_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
            static_cast<std::size_t>(x)] = c;
}

void PixelBuffer::fill(Rgba8 c) noexcept {
    for (auto& p : pixels_) p = c;
}

} // namespace pe
