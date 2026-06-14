#include "pe/core/ColorProfile.hpp"

#include <lcms2.h>

namespace pe {

ColorProfile::~ColorProfile() {
    if (handle_ != nullptr) cmsCloseProfile(static_cast<cmsHPROFILE>(handle_));
}

std::shared_ptr<ColorProfile> ColorProfile::sRGB() {
    cmsHPROFILE p = cmsCreate_sRGBProfile();
    if (p == nullptr) return nullptr;
    return std::shared_ptr<ColorProfile>(new ColorProfile(p));
}

std::shared_ptr<ColorProfile> ColorProfile::fromIccData(std::span<const std::byte> icc) {
    if (icc.empty()) return nullptr;
    cmsHPROFILE p = cmsOpenProfileFromMem(icc.data(), static_cast<cmsUInt32Number>(icc.size()));
    if (p == nullptr) return nullptr;
    return std::shared_ptr<ColorProfile>(new ColorProfile(p));
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
