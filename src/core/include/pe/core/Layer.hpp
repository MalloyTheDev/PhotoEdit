#pragma once

#include "pe/core/BlendMode.hpp"
#include "pe/core/Color.hpp"
#include "pe/core/Geometry.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/Tile.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace pe {

using LayerId = uint64_t;
inline constexpr LayerId kNoLayer = 0;

// Allocate a process-unique, non-zero layer id (thread-safe).
[[nodiscard]] LayerId nextLayerId() noexcept;

enum class LayerKind : uint8_t {
    Pixel = 0,
    Background,   // opaque base, transparency-locked, bottom of the stack
    Text,         // M8
    Shape,        // M8
    Fill,         // solid (M1), gradient/pattern (M5)
    Adjustment,   // M5
    SmartObject,  // M8
    Group,        // nested sub-stack
    Artboard,     // M8 first-class
    Video,        // later
    Generative,   // later
};

[[nodiscard]] const char* layerKindName(LayerKind) noexcept;

// Independent per-layer locks. lockTransparency is the common one (paint color
// but not alpha). Honored by editing tools (M3+); modeled here in M1.
struct LayerLocks {
    bool transparency = false;
    bool pixels = false;
    bool position = false;
    bool all = false;

    constexpr bool operator==(const LayerLocks&) const = default;
};

// The polymorphic node the compositor walks. Every kind satisfies this contract
// so the compositor is uniform. Property setters are the command-facing mutation
// surface (see docs/systems/03-layer-system.md); readers are const.
class Layer {
public:
    explicit Layer(LayerKind kind, std::string name);
    // Defined out-of-line so the unique_ptr<Mask> member only needs a forward
    // declaration of Mask in this header.
    virtual ~Layer();

    Layer(const Layer&) = delete;
    Layer& operator=(const Layer&) = delete;

    [[nodiscard]] LayerId id() const noexcept { return id_; }
    [[nodiscard]] LayerKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    [[nodiscard]] bool visible() const noexcept { return visible_; }
    [[nodiscard]] LayerLocks locks() const noexcept { return locks_; }
    [[nodiscard]] float opacity() const noexcept { return opacity_; }
    [[nodiscard]] float fillOpacity() const noexcept { return fillOpacity_; }
    [[nodiscard]] BlendMode blendMode() const noexcept { return blendMode_; }
    [[nodiscard]] bool clipped() const noexcept { return clipped_; }

    // Command-facing setters. Opacity values are clamped to [0,1].
    void setName(std::string name) { name_ = std::move(name); }
    void setVisible(bool v) noexcept { visible_ = v; }
    void setLocks(LayerLocks l) noexcept { locks_ = l; }
    void setOpacity(float o) noexcept { opacity_ = clamp01(o); }
    void setFillOpacity(float o) noexcept { fillOpacity_ = clamp01(o); }
    void setBlendMode(BlendMode m) noexcept { blendMode_ = m; }
    void setClipped(bool c) noexcept { clipped_ = c; }

    // Optional layer mask (raster). The compositor multiplies its coverage into
    // this layer's alpha before blending. nullptr == no mask (the common case).
    [[nodiscard]] const Mask* mask() const noexcept { return mask_.get(); }
    [[nodiscard]] Mask* mask() noexcept { return mask_.get(); }
    void setMask(std::unique_ptr<Mask> m) noexcept { mask_ = std::move(m); }
    [[nodiscard]] std::unique_ptr<Mask> takeMask() noexcept { return std::move(mask_); }

    // Tightest document-space rect this layer can affect (for cull). Empty rect
    // means "no content"; a Fill/Group may report finite or content-derived bounds.
    [[nodiscard]] virtual Rect contentBounds() const noexcept = 0;

    // Produce this layer's straight-alpha contribution for one tile into `dst`
    // (size == kTilePixels, tile-local row-major). Must be reentrant and
    // side-effect free: the compositor may call it concurrently across workers.
    // Implementations must fill every pixel (transparent where there is none).
    virtual void renderInto(TileCoord coord, std::span<Rgbaf> dst) const = 0;

    // Deep copy with a fresh id (for DuplicateLayer). Each kind clones itself.
    [[nodiscard]] virtual std::unique_ptr<Layer> clone() const = 0;

protected:
    // Copy universal properties into a freshly-constructed clone.
    void copyPropsTo(Layer& dst) const;

private:
    LayerId id_;
    LayerKind kind_;
    std::string name_;
    bool visible_ = true;
    LayerLocks locks_{};
    float opacity_ = 1.0f;
    float fillOpacity_ = 1.0f;
    BlendMode blendMode_ = BlendMode::Normal;
    bool clipped_ = false;
    std::unique_ptr<Mask> mask_;  // optional layer mask
};

}  // namespace pe
