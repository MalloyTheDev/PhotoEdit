#include "pe/core/ColorProfile.hpp"
#include "pe/core/ColorTransform.hpp"
#include "pe_test.hpp"

#ifdef PHOTOEDIT_HAVE_LCMS2

#include <cmath>
#include <span>
#include <vector>

using namespace pe;

PE_TEST(colortransform_srgb_identity_preserves_color_and_alpha) {
    auto srgb = ColorProfile::sRGB();
    PE_CHECK(srgb != nullptr);
    auto t = ColorTransform::create(*srgb, *srgb);  // sRGB -> sRGB is ~identity
    PE_CHECK(t != nullptr);
    PE_CHECK(t->valid());

    std::vector<Rgbaf> px = {
        Rgbaf{0.2f, 0.5f, 0.8f, 0.5f},
        Rgbaf{1.0f, 0.0f, 0.0f, 1.0f},
        Rgbaf{0.0f, 0.0f, 0.0f, 0.0f},
    };
    std::vector<Rgbaf> out(px.size());
    t->apply(px.data(), out.data(), px.size());
    for (std::size_t i = 0; i < px.size(); ++i) {
        PE_CHECK_NEAR(out[i].r, px[i].r);
        PE_CHECK_NEAR(out[i].g, px[i].g);
        PE_CHECK_NEAR(out[i].b, px[i].b);
        PE_CHECK_NEAR(out[i].a, px[i].a);  // alpha copied through, not transformed
    }
}

PE_TEST(colortransform_in_place) {
    auto srgb = ColorProfile::sRGB();
    auto t = ColorTransform::create(*srgb, *srgb);
    PE_CHECK(t != nullptr);
    std::vector<Rgbaf> px = {Rgbaf{0.3f, 0.6f, 0.9f, 0.7f}};
    t->applyInPlace(px);
    PE_CHECK_NEAR(px[0].r, 0.3f);
    PE_CHECK_NEAR(px[0].a, 0.7f);
}

PE_TEST(colortransform_zero_count_is_noop) {
    auto srgb = ColorProfile::sRGB();
    auto t = ColorTransform::create(*srgb, *srgb);
    PE_CHECK(t != nullptr);
    Rgbaf dummy{0.1f, 0.2f, 0.3f, 0.4f};
    t->apply(&dummy, &dummy, 0);  // must not touch memory / crash
    PE_CHECK_NEAR(dummy.r, 0.1f);
}

PE_TEST(colortransform_srgb_to_adobergb_is_non_identity_and_roundtrips) {
    auto srgb = ColorProfile::builtin(BuiltinSpace::sRGB);
    auto adobe = ColorProfile::builtin(BuiltinSpace::AdobeRGB1998);
    PE_CHECK(srgb != nullptr);
    PE_CHECK(adobe != nullptr);
    PE_CHECK(adobe->mode() == ColorMode::RGB);

    auto toAdobe = ColorTransform::create(*srgb, *adobe);
    auto toSrgb = ColorTransform::create(*adobe, *srgb);
    PE_CHECK(toAdobe != nullptr);
    PE_CHECK(toSrgb != nullptr);

    const Rgbaf red{1.0f, 0.0f, 0.0f, 1.0f};
    Rgbaf inAdobe = red;
    toAdobe->applyInPlace(std::span<Rgbaf>(&inAdobe, 1));
    // Converting saturated sRGB red into a wider gamut genuinely changes the
    // encoded numbers (not an identity pass-through).
    const bool changed = std::fabs(inAdobe.r - red.r) > 0.01f ||
                         std::fabs(inAdobe.g - red.g) > 0.01f ||
                         std::fabs(inAdobe.b - red.b) > 0.01f;
    PE_CHECK(changed);
    PE_CHECK_NEAR(inAdobe.a, 1.0f);  // alpha untouched

    // Round-trip sRGB -> AdobeRGB -> sRGB recovers the in-gamut color closely.
    Rgbaf back = inAdobe;
    toSrgb->applyInPlace(std::span<Rgbaf>(&back, 1));
    PE_CHECK(std::fabs(back.r - red.r) < 0.02f);
    PE_CHECK(std::fabs(back.g - red.g) < 0.02f);
    PE_CHECK(std::fabs(back.b - red.b) < 0.02f);
}

PE_TEST(colortransform_builtin_spaces_construct) {
    for (auto sp : {BuiltinSpace::sRGB, BuiltinSpace::sRGBLinear, BuiltinSpace::DisplayP3,
                    BuiltinSpace::AdobeRGB1998, BuiltinSpace::ProPhotoRGB}) {
        auto p = ColorProfile::builtin(sp);
        PE_CHECK(p != nullptr);
        PE_CHECK(p->valid());
        PE_CHECK(p->mode() == ColorMode::RGB);
    }
}

#endif  // PHOTOEDIT_HAVE_LCMS2
