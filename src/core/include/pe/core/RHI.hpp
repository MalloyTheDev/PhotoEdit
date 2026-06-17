#pragma once

#include "pe/core/PixelBuffer.hpp"
#include "pe/core/Tile.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace pe {

// Thin Render Hardware Interface (RHI) skeleton per ADR-0002 and systems/23.
// Software backend (CPU) that must exactly match CPU reference for correctness.
// Start with minimal: upload display tiles, present region.
// D3D12/Vulkan later.

class RHIDevice {
public:
    virtual ~RHIDevice() = default;

    // Software impl just keeps CPU pixels; GPU would upload to texture.
    virtual void uploadTile(TileCoord coord, std::span<const Rgba8> pixels) = 0;

    // Present / composite to output (for software: copy to provided buffer).
    virtual void presentRegion(Rect region, std::span<Rgba8> out) = 0;

    // For future compute dispatch etc.
    virtual void dispatchCompute(std::function<void()> cpuFallback) { cpuFallback(); }
};

class RHISoftware : public RHIDevice {
public:
    void uploadTile(TileCoord coord, std::span<const Rgba8> pixels) override {
        // skeleton: no-op or store if needed
    }
    void presentRegion(Rect region, std::span<Rgba8> out) override {
        std::fill(out.begin(), out.end(), Rgba8{128,128,128,255}); // gray placeholder
    }

private:
    std::vector<Rgba8> backing_;
    int width_ = 0, height_ = 0;
};

std::unique_ptr<RHIDevice> createSoftwareRHI() {
    return std::make_unique<RHISoftware>();
}

} // namespace pe
