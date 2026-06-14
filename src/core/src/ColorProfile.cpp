#include "pe/core/ColorProfile.hpp"

#include <lcms2.h>

namespace pe {

ColorProfile::~ColorProfile() {
    if (handle_ != nullptr) cmsCloseProfile(static_cast<cmsHPROFILE>(handle_));
}

namespace {
// (ColorProfile::fromHandle wraps a raw handle; defined below.)

// Build an RGB profile from a white point, primaries, and one tone curve shared by
// all three channels. Returns null on any lcms failure. Frees the tone curve.
cmsHPROFILE buildRgbProfile(cmsCIExyY white, cmsCIExyYTRIPLE primaries, cmsToneCurve* curve) {
    if (curve == nullptr) return nullptr;
    cmsToneCurve* curves[3] = {curve, curve, curve};
    cmsHPROFILE p = cmsCreateRGBProfile(&white, &primaries, curves);
    cmsFreeToneCurve(curve);
    return p;
}
}  // namespace

std::shared_ptr<ColorProfile> ColorProfile::fromHandle(void* handle) {
    if (handle == nullptr) return nullptr;
    return std::shared_ptr<ColorProfile>(new ColorProfile(handle));
}

std::shared_ptr<ColorProfile> ColorProfile::sRGB() {
    return ColorProfile::fromHandle(cmsCreate_sRGBProfile());
}

std::shared_ptr<ColorProfile> ColorProfile::builtin(BuiltinSpace space) {
    // The sRGB/IEC 61966-2-1 transfer curve as a parametric tone curve (type 4).
    const auto srgbCurve = []() {
        const cmsFloat64Number p[5] = {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};
        return cmsBuildParametricToneCurve(nullptr, 4, p);
    };
    const cmsCIExyY kD65{0.3127, 0.3290, 1.0};
    const cmsCIExyY kD50{0.34567, 0.35850, 1.0};

    switch (space) {
        case BuiltinSpace::sRGB:
            return sRGB();
        case BuiltinSpace::sRGBLinear: {
            const cmsCIExyYTRIPLE prim{
                {0.640, 0.330, 1.0}, {0.300, 0.600, 1.0}, {0.150, 0.060, 1.0}};
            return ColorProfile::fromHandle(
                buildRgbProfile(kD65, prim, cmsBuildGamma(nullptr, 1.0)));
        }
        case BuiltinSpace::DisplayP3: {
            const cmsCIExyYTRIPLE prim{
                {0.680, 0.320, 1.0}, {0.265, 0.690, 1.0}, {0.150, 0.060, 1.0}};
            return ColorProfile::fromHandle(buildRgbProfile(kD65, prim, srgbCurve()));
        }
        case BuiltinSpace::AdobeRGB1998: {
            const cmsCIExyYTRIPLE prim{
                {0.640, 0.330, 1.0}, {0.210, 0.710, 1.0}, {0.150, 0.060, 1.0}};
            return ColorProfile::fromHandle(
                buildRgbProfile(kD65, prim, cmsBuildGamma(nullptr, 2.19921875)));
        }
        case BuiltinSpace::ProPhotoRGB: {
            const cmsCIExyYTRIPLE prim{
                {0.7347, 0.2653, 1.0}, {0.1596, 0.8404, 1.0}, {0.0366, 0.0001, 1.0}};
            return ColorProfile::fromHandle(
                buildRgbProfile(kD50, prim, cmsBuildGamma(nullptr, 1.8)));
        }
    }
    return nullptr;
}

std::shared_ptr<ColorProfile> ColorProfile::fromIccData(std::span<const std::byte> icc) {
    // ICC bytes are untrusted (embedded in opened files); reject empty input and any
    // size that would not fit lcms2's 32-bit length (truncation would mis-parse).
    if (icc.empty() || icc.size() > 0xFFFFFFFFull) return nullptr;
    return fromHandle(cmsOpenProfileFromMem(icc.data(), static_cast<cmsUInt32Number>(icc.size())));
}

ColorMode ColorProfile::mode() const noexcept {
    if (handle_ == nullptr) return ColorMode::RGB;
    switch (cmsGetColorSpace(static_cast<cmsHPROFILE>(handle_))) {
        case cmsSigCmykData:
            return ColorMode::CMYK;
        case cmsSigGrayData:
            return ColorMode::Gray;
        case cmsSigLabData:
            return ColorMode::Lab;
        case cmsSigRgbData:
        default:
            return ColorMode::RGB;
    }
}

std::string ColorProfile::description() const {
    if (handle_ == nullptr) return {};
    auto h = static_cast<cmsHPROFILE>(handle_);
    const cmsUInt32Number n = cmsGetProfileInfoASCII(h, cmsInfoDescription, "en", "US", nullptr, 0);
    if (n == 0) return {};
    std::string out(n, '\0');
    cmsGetProfileInfoASCII(h, cmsInfoDescription, "en", "US", out.data(), n);
    // lcms writes a NUL-terminated string into the buffer; drop the trailing NUL(s).
    const std::size_t end = out.find('\0');
    if (end != std::string::npos) out.resize(end);
    return out;
}

}  // namespace pe
