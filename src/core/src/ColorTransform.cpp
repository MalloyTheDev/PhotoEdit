#include "pe/core/ColorTransform.hpp"

#include <lcms2.h>

namespace pe {

namespace {
cmsUInt32Number intentToLcms(RenderingIntent intent) noexcept {
    switch (intent) {
        case RenderingIntent::Perceptual:
            return INTENT_PERCEPTUAL;
        case RenderingIntent::Saturation:
            return INTENT_SATURATION;
        case RenderingIntent::AbsoluteColorimetric:
            return INTENT_ABSOLUTE_COLORIMETRIC;
        case RenderingIntent::RelativeColorimetric:
        default:
            return INTENT_RELATIVE_COLORIMETRIC;
    }
}
}  // namespace

ColorTransform::~ColorTransform() {
    if (handle_ != nullptr) cmsDeleteTransform(static_cast<cmsHTRANSFORM>(handle_));
}

std::shared_ptr<ColorTransform> ColorTransform::create(const ColorProfile& src,
                                                       const ColorProfile& dst,
                                                       RenderingIntent intent,
                                                       bool blackPointCompensation) {
    if (!src.valid() || !dst.valid()) return nullptr;
    // First cut: RGB working/display leg only. Gray/CMYK device links arrive later.
    if (src.mode() != ColorMode::RGB || dst.mode() != ColorMode::RGB) return nullptr;

    cmsUInt32Number flags = cmsFLAGS_COPY_ALPHA;  // carry the 4th (alpha) channel
    if (blackPointCompensation) flags |= cmsFLAGS_BLACKPOINTCOMPENSATION;

    // TYPE_RGBA_FLT: four contiguous floats per pixel (R,G,B,A), matching Rgbaf.
    cmsHTRANSFORM t = cmsCreateTransform(
        static_cast<cmsHPROFILE>(src.nativeHandle()), TYPE_RGBA_FLT,
        static_cast<cmsHPROFILE>(dst.nativeHandle()), TYPE_RGBA_FLT, intentToLcms(intent), flags);
    if (t == nullptr) return nullptr;
    return std::shared_ptr<ColorTransform>(new ColorTransform(t));
}

void ColorTransform::apply(const Rgbaf* src, Rgbaf* dst, std::size_t count) const noexcept {
    if (handle_ == nullptr || src == nullptr || dst == nullptr || count == 0) return;
    // cmsDoTransform takes a 32-bit pixel count; chunk to stay within it on huge runs.
    constexpr std::size_t kChunk = 0x4000000;  // 64M pixels per call, well under 2^32
    while (count > 0) {
        const std::size_t n = count < kChunk ? count : kChunk;
        cmsDoTransform(static_cast<cmsHTRANSFORM>(handle_), src, dst,
                       static_cast<cmsUInt32Number>(n));
        src += n;
        dst += n;
        count -= n;
    }
}

void ColorTransform::applyInPlace(std::span<Rgbaf> pixels) const noexcept {
    apply(pixels.data(), pixels.data(), pixels.size());
}

}  // namespace pe
