#include "pe/core/ColorEngine.hpp"

namespace pe {

ColorTransformRef ColorEngine::transform(const ColorProfileRef& src, const ColorProfileRef& dst,
                                         RenderingIntent intent, bool blackPointCompensation) {
    if (!src || !dst) return nullptr;

    const Key key{src.get(), dst.get(), intent, blackPointCompensation};
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = cache_.find(key); it != cache_.end()) return it->second.transform;

    ColorTransformRef built = ColorTransform::create(*src, *dst, intent, blackPointCompensation);
    if (!built) return nullptr;  // don't cache failures; the inputs are immutable anyway
    cache_.emplace(key, Entry{src, dst, built});
    return built;
}

std::size_t ColorEngine::cachedTransformCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

void ColorEngine::clearCache() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
}

}  // namespace pe
