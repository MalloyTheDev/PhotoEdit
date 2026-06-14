#include "pe/core/BlendMode.hpp"
#include "pe/core/Color.hpp"
#include "pe_test.hpp"

using namespace pe;

PE_TEST(blend_channel_identities) {
    // Normal returns the source.
    PE_CHECK_NEAR(blendChannel(BlendMode::Normal, 0.3f, 0.7f), 0.7f);

    // Multiply by white is identity; by black is black.
    PE_CHECK_NEAR(blendChannel(BlendMode::Multiply, 0.6f, 1.0f), 0.6f);
    PE_CHECK_NEAR(blendChannel(BlendMode::Multiply, 0.6f, 0.0f), 0.0f);

    // Screen with black is identity; with white is white.
    PE_CHECK_NEAR(blendChannel(BlendMode::Screen, 0.6f, 0.0f), 0.6f);
    PE_CHECK_NEAR(blendChannel(BlendMode::Screen, 0.6f, 1.0f), 1.0f);

    // Darken / Lighten.
    PE_CHECK_NEAR(blendChannel(BlendMode::Darken, 0.6f, 0.2f), 0.2f);
    PE_CHECK_NEAR(blendChannel(BlendMode::Lighten, 0.6f, 0.2f), 0.6f);

    // Difference of equal channels is zero.
    PE_CHECK_NEAR(blendChannel(BlendMode::Difference, 0.5f, 0.5f), 0.0f);
}

PE_TEST(blend_multiply_known_value) {
    PE_CHECK_NEAR(blendChannel(BlendMode::Multiply, 0.5f, 0.5f), 0.25f);
    PE_CHECK_NEAR(blendChannel(BlendMode::Screen, 0.5f, 0.5f), 0.75f);
}

PE_TEST(composite_over_opaque_normal) {
    Rgbaf backdrop{0.0f, 0.0f, 0.0f, 1.0f};
    Rgbaf source{1.0f, 0.0f, 0.0f, 1.0f};
    Rgbaf out = compositeOver(BlendMode::Normal, backdrop, source, 1.0f);
    PE_CHECK_NEAR(out.r, 1.0f);
    PE_CHECK_NEAR(out.a, 1.0f);
}

PE_TEST(composite_over_transparent_source_is_noop) {
    Rgbaf backdrop{0.2f, 0.4f, 0.6f, 1.0f};
    Rgbaf source{1.0f, 1.0f, 1.0f, 0.0f};
    Rgbaf out = compositeOver(BlendMode::Normal, backdrop, source, 1.0f);
    PE_CHECK_NEAR(out.r, 0.2f);
    PE_CHECK_NEAR(out.g, 0.4f);
    PE_CHECK_NEAR(out.b, 0.6f);
    PE_CHECK_NEAR(out.a, 1.0f);
}

PE_TEST(composite_opacity_half) {
    // White over black at 50% opacity, Normal -> mid grey, fully opaque result.
    Rgbaf backdrop{0.0f, 0.0f, 0.0f, 1.0f};
    Rgbaf source{1.0f, 1.0f, 1.0f, 1.0f};
    Rgbaf out = compositeOver(BlendMode::Normal, backdrop, source, 0.5f);
    PE_CHECK_NEAR(out.r, 0.5f);
    PE_CHECK_NEAR(out.a, 1.0f);
}

PE_TEST(composite_over_empty_backdrop_keeps_source) {
    // Source over fully transparent backdrop yields the source unchanged.
    Rgbaf backdrop{0.0f, 0.0f, 0.0f, 0.0f};
    Rgbaf source{0.7f, 0.3f, 0.1f, 1.0f};
    Rgbaf out = compositeOver(BlendMode::Multiply, backdrop, source, 1.0f);
    PE_CHECK_NEAR(out.r, 0.7f);
    PE_CHECK_NEAR(out.g, 0.3f);
    PE_CHECK_NEAR(out.b, 0.1f);
    PE_CHECK_NEAR(out.a, 1.0f);
}
