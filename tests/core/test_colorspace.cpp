#include "pe/core/ColorSpace.hpp"
#include "pe_test.hpp"

#include <cmath>

using namespace pe;

PE_TEST(colorspace_srgb_endpoints) {
    PE_CHECK_NEAR(srgbToLinear(0.0f), 0.0f);
    PE_CHECK_NEAR(srgbToLinear(1.0f), 1.0f);
    PE_CHECK_NEAR(linearToSrgb(0.0f), 0.0f);
    PE_CHECK_NEAR(linearToSrgb(1.0f), 1.0f);
}

PE_TEST(colorspace_srgb_midpoint) {
    // sRGB 0.5 decodes to ~0.2140 linear (a well-known reference value).
    PE_CHECK_NEAR(srgbToLinear(0.5f), 0.214041f);
    // ...and that linear value re-encodes back to 0.5.
    PE_CHECK_NEAR(linearToSrgb(0.214041f), 0.5f);
}

PE_TEST(colorspace_knee_continuity) {
    // The piecewise segments meet at the knee: gamma 0.04045 <-> linear 0.0031308.
    PE_CHECK_NEAR(srgbToLinear(0.04045f), 0.0031308f);
    PE_CHECK_NEAR(linearToSrgb(0.0031308f), 0.04045f);
}

PE_TEST(colorspace_roundtrip) {
    for (float x : {0.0f, 0.02f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f}) {
        PE_CHECK_NEAR(linearToSrgb(srgbToLinear(x)), x);
        PE_CHECK_NEAR(srgbToLinear(linearToSrgb(x)), x);
    }
}

PE_TEST(colorspace_monotonic_and_darkening) {
    // Decoding gamma->linear darkens the midtones (gamma > 1 perceptual budget).
    PE_CHECK(srgbToLinear(0.5f) < 0.5f);
    // Encoding lifts them back up.
    PE_CHECK(linearToSrgb(0.2f) > 0.2f);
    // Strictly increasing across a sweep.
    float prev = srgbToLinear(0.0f);
    for (int i = 1; i <= 20; ++i) {
        const float v = srgbToLinear(static_cast<float>(i) / 20.0f);
        PE_CHECK(v > prev);
        prev = v;
    }
}

PE_TEST(colorspace_handles_out_of_range_without_nan) {
    // Negative and >1 (HDR / scene-linear) inputs stay finite and are preserved in
    // spirit: negatives go through the linear segment, values >1 stay >1.
    const float negDecode = srgbToLinear(-0.1f);
    PE_CHECK(negDecode == negDecode);  // not NaN
    PE_CHECK(negDecode < 0.0f);
    const float negEncode = linearToSrgb(-0.1f);
    PE_CHECK(negEncode == negEncode);
    PE_CHECK(negEncode < 0.0f);
    const float hdr = linearToSrgb(2.0f);
    PE_CHECK(hdr == hdr);
    PE_CHECK(hdr > 1.0f);
    const float hdrDecode = srgbToLinear(2.0f);
    PE_CHECK(hdrDecode == hdrDecode);
    PE_CHECK(hdrDecode > 1.0f);
}

PE_TEST(colorspace_pixel_helpers_preserve_alpha) {
    Rgbaf p{0.5f, 0.25f, 0.75f, 0.4f};
    Rgbaf lin = toLinear(p);
    PE_CHECK_NEAR(lin.r, srgbToLinear(0.5f));
    PE_CHECK_NEAR(lin.a, 0.4f);  // alpha untouched
    Rgbaf back = toGammaSrgb(lin);
    PE_CHECK_NEAR(back.r, 0.5f);
    PE_CHECK_NEAR(back.g, 0.25f);
    PE_CHECK_NEAR(back.b, 0.75f);
    PE_CHECK_NEAR(back.a, 0.4f);
}
