#include "pe/core/HitTest.hpp"

#include "pe/core/Color.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Tile.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <span>
#include <vector>

namespace pe {

namespace {

std::atomic<std::uint64_t> g_hitTestTileRenders{0};

// One tile of float pixels, allocated once per hit test and reused for every layer it has
// to sample. A tile is 256x256 RGBA float, which is 1 MiB: far too much for the stack, and
// not worth allocating per layer either.
using TileScratch = std::vector<Rgbaf>;

// Alpha of one pixel of a raster layer, read straight out of its native-depth store.
[[nodiscard]] float pixelLayerAlphaAt(const PixelLayer& l, Point p) {
    switch (l.depth()) {
        case BitDepth::U16:
            return toFloat(l.tiles16().pixel(p.x, p.y)).a;
        case BitDepth::F32:
            return l.tilesF().pixel(p.x, p.y).a;
        case BitDepth::U8:
        default:
            return toFloat(l.tiles().pixel(p.x, p.y)).a;
    }
}

// This layer's own alpha at `p`, before its mask and opacity.
[[nodiscard]] float rawAlphaAt(const Layer& l, Point p, TileScratch& scratch) {
    // A first reject, and it does most of the work: a click misses most layers entirely.
    // It is TILE aligned though (TileStore::contentBounds unions whole tiles), so passing
    // it does not mean the point is anywhere near the layer's pixels.
    if (!l.contentBounds().contains(p)) return 0.0f;

    // Which is why a raster layer is read directly rather than rendered: one hash lookup
    // and one array read, with an absent tile reading as transparent. Rendering a tile is
    // 256x256 RGBA float, so doing it to read a single pixel would otherwise be the COMMON
    // case for a stack of raster layers, not the rare one. dynamic_cast rather than a kind
    // check because this reaches into the concrete type's stores and has to be certain.
    if (const auto* pl = dynamic_cast<const PixelLayer*>(&l); pl != nullptr) {
        return pixelLayerAlphaAt(*pl, p);
    }

    // Everything else goes through the same renderInto contract the compositor uses, so a
    // layer kind with no tile store of its own still answers correctly.
    g_hitTestTileRenders.fetch_add(1, std::memory_order_relaxed);
    const TileCoord coord{floorDiv(p.x, kTileSize), floorDiv(p.y, kTileSize)};
    scratch.assign(static_cast<std::size_t>(kTilePixels), Rgbaf{});
    l.renderInto(coord, std::span<Rgbaf>(scratch));

    const int lx = p.x - coord.col * kTileSize;
    const int ly = p.y - coord.row * kTileSize;
    const std::size_t i = static_cast<std::size_t>(ly) * kTileSize + static_cast<std::size_t>(lx);
    if (i >= scratch.size()) return 0.0f;  // unreachable given floorDiv, but not assumed
    return scratch[i].a;
}

// What `l` contributes at `p` once its mask and opacity are applied. `inherited` carries
// the opacity of the groups above it, which the compositor applies to the group as a whole
// and which therefore dims everything inside it.
[[nodiscard]] float coverageAt(const Layer& l, Point p, float inherited, TileScratch& scratch) {
    float a = rawAlphaAt(l, p, scratch) * l.opacity() * inherited;
    if (a <= 0.0f) return 0.0f;
    if (const Mask* m = l.mask(); m != nullptr && m->enabled()) a *= m->evaluate(p.x, p.y);
    return a;
}

// Walk one level of the stack from the top down. Returns the first layer that covers `p`,
// or kNoLayer.
[[nodiscard]] LayerId hitIn(std::span<const std::unique_ptr<Layer>> layers, Point p,
                            float threshold, float inherited, TileScratch& scratch) {
    // Index 0 is the BOTTOM of the stack, so the topmost layer is the last one.
    for (std::size_t i = layers.size(); i-- > 0;) {
        const Layer* l = layers[i].get();
        if (l == nullptr || !l->visible() || l->opacity() <= 0.0f) continue;
        // An adjustment has no pixels of its own to grab, and its content bounds are the
        // whole plane, so without this it would answer every click.
        if (l->isAdjustment()) continue;

        if (l->kind() == LayerKind::Group) {
            const auto* g = static_cast<const GroupLayer*>(l);
            // A group's own mask hides its children, so it gates the descent: a click on a
            // masked-out part of a group must fall through to what is behind the group,
            // not select a child the user cannot see.
            float gate = inherited * g->opacity();
            if (const Mask* m = g->mask(); m != nullptr && m->enabled()) {
                gate *= m->evaluate(p.x, p.y);
            }
            if (gate <= threshold) continue;
            const LayerId inside = hitIn(g->children(), p, threshold, gate, scratch);
            if (inside != kNoLayer) return inside;
            continue;
        }

        if (coverageAt(*l, p, inherited, scratch) > threshold) return l->id();
    }
    return kNoLayer;
}

}  // namespace

std::uint64_t hitTestTileRenderCount() noexcept {
    return g_hitTestTileRenders.load(std::memory_order_relaxed);
}

LayerId layerAt(const Document& doc, Point p, float threshold) {
    TileScratch scratch;
    return hitIn(doc.topLevelLayers(), p, std::max(threshold, 0.0f), 1.0f, scratch);
}

LayerId topLevelAncestorOf(const Document& doc, LayerId id) {
    if (id == kNoLayer) return kNoLayer;
    for (const std::unique_ptr<Layer>& l : doc.topLevelLayers()) {
        if (l == nullptr) continue;
        if (l->id() == id) return id;  // already top level
        if (l->kind() != LayerKind::Group) continue;
        const auto* g = static_cast<const GroupLayer*>(l.get());
        if (g->findDescendant(id) != nullptr) return l->id();
    }
    return kNoLayer;
}

}  // namespace pe
