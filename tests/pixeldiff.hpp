#pragma once

// Where and how much two images differ.
//
// Three separable correctness contracts fall out of having this:
//
//   1. Numerical fidelity: how far any channel moved, and how far on average.
//   2. Semantic behaviour: asserted by each test itself (a blur lowers contrast, a
//      gradient stays monotonic, a median keeps a hard edge).
//   3. Spatial containment: WHERE the image changed, which is what this header adds.
//
// The third is the one that was missing, and it is the one that catches an optimization
// that is numerically plausible but quietly touches pixels outside the region it was
// allowed to touch. Every bounded operation has a mathematically permitted influence box;
// asserting the changed-pixel bounds sit inside it turns "looks close" into a proof of
// locality. Reusable for brushes, masks, filters, transforms and dirty-region rendering.

#include "pe/core/Color.hpp"
#include "pe/core/Geometry.hpp"
#include "pe/core/TileStore.hpp"

#include <algorithm>
#include <cstdlib>

namespace pe_diff {

struct Diff {
    pe::Rect bounds{};         // pixel-tight bounds of every differing pixel; empty if identical
    int changed = 0;           // how many pixels differ at all
    int worstChannel = 0;      // the largest single-channel absolute difference
    double meanChannel = 0.0;  // mean absolute difference over every channel compared

    [[nodiscard]] bool identical() const { return changed == 0; }
};

// Compare two 8-bit stores over `over`. Sampling both through pixel() means an absent tile
// compares as transparent on either side, so a store that grew tiles without changing any
// value still reports identical, which is the right answer.
inline Diff compare(const pe::TileStoreT<pe::Rgba8>& a, const pe::TileStoreT<pe::Rgba8>& b,
                    pe::Rect over) {
    Diff d;
    std::int64_t sum = 0;
    std::int64_t samples = 0;
    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;
    bool any = false;
    for (int y = over.top(); y < over.bottom(); ++y) {
        for (int x = over.left(); x < over.right(); ++x) {
            const pe::Rgba8 pa = a.pixel(x, y);
            const pe::Rgba8 pb = b.pixel(x, y);
            const int delta[4] = {std::abs(static_cast<int>(pa.r) - static_cast<int>(pb.r)),
                                  std::abs(static_cast<int>(pa.g) - static_cast<int>(pb.g)),
                                  std::abs(static_cast<int>(pa.b) - static_cast<int>(pb.b)),
                                  std::abs(static_cast<int>(pa.a) - static_cast<int>(pb.a))};
            int worst = 0;
            for (const int v : delta) {
                worst = std::max(worst, v);
                sum += v;
                ++samples;
            }
            if (worst == 0) continue;
            ++d.changed;
            d.worstChannel = std::max(d.worstChannel, worst);
            if (!any) {
                minX = maxX = x;
                minY = maxY = y;
                any = true;
            } else {
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
    }
    if (any) d.bounds = pe::Rect{minX, minY, maxX - minX + 1, maxY - minY + 1};
    d.meanChannel = samples > 0 ? static_cast<double>(sum) / static_cast<double>(samples) : 0.0;
    return d;
}

// Whether `inner` lies entirely within `outer`. An empty inner is contained in anything,
// which is what an operation that changed nothing should report.
inline bool containedIn(pe::Rect inner, pe::Rect outer) {
    if (inner.isEmpty()) return true;
    return inner.left() >= outer.left() && inner.top() >= outer.top() &&
           inner.right() <= outer.right() && inner.bottom() <= outer.bottom();
}

}  // namespace pe_diff
