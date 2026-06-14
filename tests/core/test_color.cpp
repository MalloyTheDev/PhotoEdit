#include "pe/core/Color.hpp"
#include "pe_test.hpp"

using namespace pe;

PE_TEST(color_roundtrip_endpoints) {
    Rgba8 black{0, 0, 0, 255};
    Rgba8 white{255, 255, 255, 255};
    PE_CHECK_EQ(toRgba8(toFloat(black)), black);
    PE_CHECK_EQ(toRgba8(toFloat(white)), white);
}

PE_TEST(color_unit_conversion) {
    PE_CHECK_NEAR(toLinearUnit(255), 1.0f);
    PE_CHECK_NEAR(toLinearUnit(0), 0.0f);
    PE_CHECK_EQ(fromUnit(1.0f), static_cast<uint8_t>(255));
    PE_CHECK_EQ(fromUnit(0.0f), static_cast<uint8_t>(0));
    // Clamping out-of-range floats.
    PE_CHECK_EQ(fromUnit(2.0f), static_cast<uint8_t>(255));
    PE_CHECK_EQ(fromUnit(-1.0f), static_cast<uint8_t>(0));
}

PE_TEST(color_premultiply_roundtrip) {
    Rgbaf c{0.8f, 0.4f, 0.2f, 0.5f};
    Rgbaf pm = premultiply(c);
    PE_CHECK_NEAR(pm.r, 0.4f);
    PE_CHECK_NEAR(pm.a, 0.5f);

    Rgbaf back = unpremultiply(pm);
    PE_CHECK_NEAR(back.r, 0.8f);
    PE_CHECK_NEAR(back.g, 0.4f);
    PE_CHECK_NEAR(back.b, 0.2f);

    // Fully transparent unpremultiplies to zero colour, not NaN.
    Rgbaf transparent = unpremultiply(Rgbaf{0.0f, 0.0f, 0.0f, 0.0f});
    PE_CHECK_NEAR(transparent.r, 0.0f);
}
