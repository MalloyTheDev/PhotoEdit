#pragma once

#include "pe/core/ColorProfile.hpp"
#include "pe/core/ColorTransform.hpp"

#include <cstddef>
#include <map>
#include <mutex>
#include <tuple>

namespace pe {

// Owns a cache of built color transforms, keyed by (src, dst, intent, bpc). Building
// an lcms2 transform is comparatively expensive (it links a pipeline), so the cache
// turns the per-tile path into a lookup plus a buffered apply. The cache holds strong
// references to the cached profiles, so a cached profile cannot be destroyed (and its
// address reused) while it keys an entry — keeping the pointer keys sound.
//
// build-or-fetch (transform()) is thread-safe; the returned transforms are immutable
// once built and safe to share for read across worker threads. Like the rest of the
// color module, this is only built when lcms2 is available (PHOTOEDIT_HAVE_LCMS2);
// guard usage with the macro.
class ColorEngine {
public:
    // Build-or-fetch a cached transform for the key. Returns nullptr if either profile
    // is null or the transform cannot be built (e.g. a non-RGB profile, for now).
    [[nodiscard]] ColorTransformRef transform(
        const ColorProfileRef& src, const ColorProfileRef& dst,
        RenderingIntent intent = RenderingIntent::RelativeColorimetric,
        bool blackPointCompensation = true);

    [[nodiscard]] std::size_t cachedTransformCount() const;
    void clearCache();

private:
    using Key = std::tuple<const ColorProfile*, const ColorProfile*, RenderingIntent, bool>;
    struct Entry {
        ColorProfileRef src;  // keep the keyed profiles alive
        ColorProfileRef dst;
        ColorTransformRef transform;
    };

    mutable std::mutex mutex_;
    std::map<Key, Entry> cache_;
};

}  // namespace pe
