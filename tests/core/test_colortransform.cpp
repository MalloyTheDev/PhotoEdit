#include "pe/core/ColorProfile.hpp"
#include "pe/core/ColorTransform.hpp"
#include "pe_test.hpp"

#ifdef PHOTOEDIT_HAVE_LCMS2

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

#endif  // PHOTOEDIT_HAVE_LCMS2
