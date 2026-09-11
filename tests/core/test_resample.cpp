// The separable Catmull-Rom resampler. It is the oracle the tile-streamed layer resampler will
// be validated against, so these pin its PROPERTIES, not just that it runs: interpolating (exact
// at an integer scale), clamp-to-edge, premultiplied (no dark fringe from transparent pixels),
// alpha never negative (no ringing holes), and a real low-pass on downscale (every source row
// contributes, unlike bilinear).

#include "pe/core/Color.hpp"
#include "pe/core/Resample.hpp"
#include "pe_test.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace pe;

namespace {

Rgbaf opaque(float r, float g, float b) {
    return Rgbaf{r, g, b, 1.0f};
}

bool near(float a, float b, float slack = 0.002f) {
    return std::fabs(a - b) < slack;
}

bool sameColor(Rgbaf a, Rgbaf b) {
    return near(a.r, b.r) && near(a.g, b.g) && near(a.b, b.b) && near(a.a, b.a);
}

}  // namespace

PE_TEST(resample_axis_weights_sum_to_one_and_identity_is_a_delta) {
    // Every output sample's weights must sum to 1 or the image gains or loses energy. At a 1:1
    // scale the kernel collapses to a delta on the aligned source sample.
    const ResampleAxis id = buildResampleAxis(10, 10);
    PE_REQUIRE(!id.empty());
    for (int o = 0; o < 10; ++o) {
        float sum = 0.0f;
        float atSelf = 0.0f;
        for (int t = 0; t < id.support; ++t) {
            const float w = id.weights[static_cast<std::size_t>(o) * id.support + t];
            sum += w;
            if (id.first[o] + t == o) atSelf = w;
        }
        PE_CHECK(near(sum, 1.0f));
        PE_CHECK(near(atSelf, 1.0f));  // identity: all weight on the matching source sample
    }

    // On a 4x downscale the weights still sum to 1 (a stretched Catmull-Rom does not partition
    // unity for free, so this proves the normalisation).
    const ResampleAxis down = buildResampleAxis(16, 4);
    PE_REQUIRE(!down.empty());
    for (int o = 0; o < 4; ++o) {
        float sum = 0.0f;
        for (int t = 0; t < down.support; ++t) {
            sum += down.weights[static_cast<std::size_t>(o) * down.support + t];
        }
        PE_CHECK(near(sum, 1.0f));
    }
    PE_CHECK(buildResampleAxis(0, 4).empty());
    PE_CHECK(buildResampleAxis(4, 0).empty());
}

PE_TEST(resample_identity_reproduces_the_source_exactly) {
    // Interpolating: resampling to the same size must return the input, or every no-op trip
    // through the Image Size dialog would soften the picture.
    std::vector<Rgbaf> src;
    for (int y = 0; y < 6; ++y) {
        for (int x = 0; x < 6; ++x) {
            src.push_back(opaque(x / 5.0f, y / 5.0f, (x * y) / 25.0f));
        }
    }
    const std::vector<Rgbaf> out = resampleImage(src, 6, 6, 6, 6);
    PE_REQUIRE(out.size() == src.size());
    for (std::size_t i = 0; i < src.size(); ++i) PE_CHECK(sameColor(out[i], src[i]));
}

PE_TEST(resample_upscale_carries_the_gradient_in_the_right_direction) {
    // Red rises left->right, green rises top->bottom. Corners are APPROACHED, not reproduced,
    // under the half-pixel-centre convention (the identity test pins exactness); what an upscale
    // must get right is the direction of the gradient and staying within range bar a little
    // bicubic ringing. A transposed or non-interpolating resampler fails this.
    // A smooth 8x8 ramp, not a 2-pixel step: bicubic rings hard against a near-step, which is
    // correct kernel behaviour but a poor test vehicle. Over a gradient the overshoot is
    // negligible, so the directional properties read cleanly.
    constexpr int n = 8;
    std::vector<Rgbaf> src;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            src.push_back(
                opaque(x / static_cast<float>(n - 1), y / static_cast<float>(n - 1), 0.5f));
        }
    }
    constexpr int d = 24;
    const std::vector<Rgbaf> out = resampleImage(src, n, n, d, d);
    PE_REQUIRE(out.size() == static_cast<std::size_t>(d) * d);
    for (const Rgbaf& p : out) {
        PE_CHECK(near(p.a, 1.0f));                // opaque throughout
        PE_CHECK(p.r >= -0.03f && p.r <= 1.03f);  // smooth gradient: ringing is tiny
        PE_CHECK(p.g >= -0.03f && p.g <= 1.03f);
        PE_CHECK(near(p.b, 0.5f, 0.01f));  // a constant channel stays constant
    }
    PE_CHECK(out[0].r < 0.1f);      // red low on the left edge
    PE_CHECK(out[d - 1].r > 0.9f);  // and high on the right edge
    for (int x = 1; x < d; ++x) PE_CHECK(out[x].r >= out[x - 1].r - 0.001f);  // nondecreasing
    PE_CHECK(out[0].g < 0.1f);            // green low on the top edge
    PE_CHECK(out[(d - 1) * d].g > 0.9f);  // and high on the bottom edge
    for (int y = 1; y < d; ++y) PE_CHECK(out[y * d].g >= out[(y - 1) * d].g - 0.001f);
}

PE_TEST(resample_downscale_reads_every_source_row_unlike_bilinear) {
    // The defect this whole exercise fixes: bilinear at 0.25x reads a quarter of the rows and
    // drops the rest. A proper low-pass averages them, so a 4x1 downscale of a column that is
    // black at the top and white at the bottom must come out MID-grey, not one of the two
    // extremes a subsampler would pick.
    std::vector<Rgbaf> col;  // 1 wide, 8 tall, linear black->white
    for (int y = 0; y < 8; ++y) {
        const float v = y / 7.0f;
        col.push_back(opaque(v, v, v));
    }
    const std::vector<Rgbaf> out = resampleImage(col, 1, 8, 1, 1);
    PE_REQUIRE(out.size() == 1);
    // The average of the ramp is ~0.5; a subsampler would give ~0.0 or ~1.0. Allow the kernel's
    // edge weighting some slack, but it must be solidly in the middle.
    PE_CHECK(out[0].r > 0.35f && out[0].r < 0.65f);
}

PE_TEST(resample_clamps_to_the_edge_rather_than_fading_out) {
    // Image Size resamples a canvas-filling image, so a tap past the border must read the edge
    // pixel, not transparent. A fully opaque source must stay fully opaque everywhere, including
    // the outermost output pixels.
    std::vector<Rgbaf> src(4 * 4, opaque(0.2f, 0.6f, 0.9f));
    const std::vector<Rgbaf> out = resampleImage(src, 4, 4, 16, 16);
    PE_REQUIRE(out.size() == 256);
    for (const Rgbaf& p : out) {
        PE_CHECK(near(p.a, 1.0f));         // no transparent rim
        PE_CHECK(near(p.r, 0.2f, 0.03f));  // colour holds to the edge
        PE_CHECK(near(p.b, 0.9f, 0.03f));
    }
}

PE_TEST(resample_premultiplies_so_a_transparent_pixel_does_not_bleed_colour) {
    // A transparent pixel carries an arbitrary RGB (here hot magenta). Resampling straight-alpha
    // would mix that colour into its opaque neighbour as a fringe; premultiplied, it can only
    // reduce alpha. Half opaque green beside a transparent magenta, upscaled: the opaque side's
    // colour must stay green, never pick up magenta.
    const std::vector<Rgbaf> src = {opaque(0.0f, 1.0f, 0.0f),        // opaque green
                                    Rgbaf{1.0f, 0.0f, 1.0f, 0.0f}};  // transparent magenta
    const std::vector<Rgbaf> out = resampleImage(src, 2, 1, 8, 1);
    PE_REQUIRE(out.size() == 8);
    // The leftmost output is fully opaque green with no magenta in it.
    PE_CHECK(near(out[0].a, 1.0f));
    PE_CHECK(near(out[0].g, 1.0f, 0.02f));
    PE_CHECK(near(out[0].r, 0.0f, 0.02f));  // no magenta bleed
    PE_CHECK(near(out[0].b, 0.0f, 0.02f));
}

PE_TEST(resample_alpha_never_rings_negative_into_a_hole) {
    // Catmull-Rom overshoots at a hard edge. A premultiplied alpha edge can ring below zero, and
    // an un-clamped negative alpha becomes an invisible hole once unpremultiplied. Every output
    // alpha must stay within [0,1].
    std::vector<Rgbaf> src;  // 8x1: opaque left half, transparent right half
    for (int x = 0; x < 8; ++x) {
        src.push_back(x < 4 ? opaque(1.0f, 1.0f, 1.0f) : Rgbaf{0.0f, 0.0f, 0.0f, 0.0f});
    }
    const std::vector<Rgbaf> out = resampleImage(src, 8, 1, 32, 1);
    PE_REQUIRE(out.size() == 32);
    for (const Rgbaf& p : out) {
        PE_CHECK(p.a >= 0.0f);
        PE_CHECK(p.a <= 1.0f);
    }
    PE_CHECK(near(out[0].a, 1.0f));   // solidly inside the opaque half
    PE_CHECK(near(out[31].a, 0.0f));  // solidly inside the transparent half
}

PE_TEST(resample_leaves_hdr_colour_unclamped) {
    // A 32-bit-float layer can hold values above 1. The resampler must not clamp RGB (only
    // alpha), or resizing an HDR document would crush its highlights.
    std::vector<Rgbaf> src(4 * 4, opaque(3.0f, 0.0f, 0.0f));  // super-white red
    const std::vector<Rgbaf> out = resampleImage(src, 4, 4, 8, 8);
    PE_REQUIRE(!out.empty());
    PE_CHECK(out[out.size() / 2].r > 2.5f);  // still well above 1
}

PE_TEST(resample_coverage_stays_in_range_and_interpolates) {
    // Single-channel path for masks/selection: a 0->255 ramp upscaled stays within [0,255] and
    // moves monotonically, and a downscale of the ramp lands mid-scale.
    std::vector<std::uint8_t> ramp;  // 1x4: 0, 85, 170, 255
    for (int i = 0; i < 4; ++i) ramp.push_back(static_cast<std::uint8_t>(i * 85));
    const std::vector<std::uint8_t> up = resampleCoverage(ramp, 1, 4, 1, 16);
    PE_REQUIRE(up.size() == 16);
    PE_CHECK(up.front() <= 5);   // still near 0 at the top (clamp-to-edge)
    PE_CHECK(up.back() >= 250);  // still near 255 at the bottom
    for (std::size_t i = 1; i < up.size(); ++i) PE_CHECK(up[i] >= up[i - 1]);  // monotonic

    const std::vector<std::uint8_t> down = resampleCoverage(ramp, 1, 4, 1, 1);
    PE_REQUIRE(down.size() == 1);
    PE_CHECK(down[0] > 100 && down[0] < 160);  // the average, not an endpoint
}

PE_TEST(resample_downscale_low_pass_does_not_drop_pixels_between_output_centres) {
    // The stretch (h = 1/scale) is what makes this a low-pass rather than a subsampler. At 6:1,
    // an unstretched radius-2 kernel reaches only +-2 source pixels around each output centre
    // (2.5 and 8.5 here), so pixels near the midpoint (5, 6) fall in the gap between them and
    // contribute NOTHING. A stretched kernel (radius 12) reaches them. Two bright pixels sitting
    // exactly in that gap must therefore survive into the output.
    std::vector<std::uint8_t> src(12, 0);
    src[5] = 255;
    src[6] = 255;
    const std::vector<std::uint8_t> out = resampleCoverage(src, 12, 1, 2, 1);
    PE_REQUIRE(out.size() == 2);
    const int energy = static_cast<int>(out[0]) + static_cast<int>(out[1]);
    PE_CHECK(energy > 20);  // 0 if the kernel is not stretched (the pixels are dropped)
}

PE_TEST(resample_coverage_identity_is_exact) {
    std::vector<std::uint8_t> src = {10, 40, 200, 255, 128, 0};
    const std::vector<std::uint8_t> out = resampleCoverage(src, 3, 2, 3, 2);
    PE_REQUIRE(out.size() == src.size());
    for (std::size_t i = 0; i < src.size(); ++i) PE_CHECK_EQ(out[i], src[i]);
}

PE_TEST(resample_rejects_a_degenerate_size) {
    std::vector<Rgbaf> one(1, opaque(1.0f, 1.0f, 1.0f));
    PE_CHECK(resampleImage(one, 1, 1, 0, 4).empty());
    PE_CHECK(resampleImage(one, 1, 1, 4, 0).empty());
    PE_CHECK(resampleImage(one, 0, 1, 4, 4).empty());
    PE_CHECK(resampleImage({}, 2, 2, 4, 4).empty());  // length mismatch
    PE_CHECK(resampleCoverage({}, 2, 2, 4, 4).empty());
}
