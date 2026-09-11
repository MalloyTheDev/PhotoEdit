// resampleMask and resampledSelection: the single-channel side of Image Size. Both round a
// coverage buffer through the same Catmull-Rom resampleCoverage the pixel path uses, so a layer's
// mask and a document's selection scale in lockstep with the pixels. These pin the properties the
// contiguous resampleCoverage cannot see on its own: the canvas sampling domain (absent == opaque
// == revealing), the canonical write-back (kOpaque skipped), and the two selection guards whose
// failure would silently turn "this area" into "everything".

#include "pe/core/Color.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/Resample.hpp"
#include "pe/core/Selection.hpp"
#include "pe_test.hpp"

#include <cstdint>
#include <vector>

using namespace pe;

// ------------------------------------------------------------------- resampleMask

PE_TEST(resamplemask_identity_preserves_coverage) {
    // A half-hidden mask resampled 1:1 is exact (Catmull-Rom interpolates), so every sampled
    // value survives: the left half stays clear, the right half stays revealing.
    MaskBuffer src;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 32; ++x) src.setValue(x, y, MaskBuffer::kClear);
    }
    const MaskBuffer out = resampleMask(src, Rect{0, 0, 64, 64}, Rect{0, 0, 64, 64});
    PE_CHECK_EQ(out.value(5, 5), MaskBuffer::kClear);
    PE_CHECK_EQ(out.value(31, 40), MaskBuffer::kClear);
    PE_CHECK_EQ(out.value(32, 40), MaskBuffer::kOpaque);
    PE_CHECK_EQ(out.value(60, 40), MaskBuffer::kOpaque);
}

PE_TEST(resamplemask_absent_stays_revealing) {
    // An empty mask reads as kOpaque everywhere (no masking). Resampling it must produce another
    // empty, all-revealing mask -- not a materialised slab of 255s, which would defeat the
    // canonical form and inflate serialization.
    MaskBuffer src;
    const MaskBuffer out = resampleMask(src, Rect{0, 0, 100, 80}, Rect{0, 0, 250, 200});
    PE_CHECK(out.contentBounds().isEmpty());
    PE_CHECK_EQ(out.value(10, 10), MaskBuffer::kOpaque);
    PE_CHECK_EQ(out.value(240, 190), MaskBuffer::kOpaque);
}

PE_TEST(resamplemask_upscale_moves_the_boundary_with_the_pixels) {
    // A clear block over the left quarter, upscaled 2x, must clear the left half of the new
    // canvas: the hidden region scales with the image rather than staying put.
    MaskBuffer src;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 16; ++x) src.setValue(x, y, MaskBuffer::kClear);
    }
    const MaskBuffer out = resampleMask(src, Rect{0, 0, 64, 64}, Rect{0, 0, 128, 128});
    PE_CHECK_EQ(out.value(5, 20), MaskBuffer::kClear);    // was clear, still clear
    PE_CHECK_EQ(out.value(28, 20), MaskBuffer::kClear);   // 16*2 boundary moved out to ~32
    PE_CHECK_EQ(out.value(64, 20), MaskBuffer::kOpaque);  // right half revealed
}

PE_TEST(resamplemask_downscale_matches_the_coverage_oracle) {
    // The load-bearing agreement: resampleMask over the canvas must equal resampleCoverage run on
    // the same canvas-sampled buffer, pixel for pixel (bar the kOpaque canonicalisation). A soft
    // vertical ramp gives every downscaled column a distinct value to disagree on.
    MaskBuffer src;
    for (int y = 0; y < 90; ++y) {
        for (int x = 0; x < 120; ++x) {
            src.setValue(x, y, static_cast<uint8_t>(x * 255 / 119));
        }
    }
    std::vector<uint8_t> cov(static_cast<std::size_t>(120) * 90);
    for (int y = 0; y < 90; ++y) {
        for (int x = 0; x < 120; ++x) {
            cov[static_cast<std::size_t>(y) * 120 + x] = src.value(x, y);
        }
    }
    const std::vector<uint8_t> oracle = resampleCoverage(cov, 120, 90, 40, 30);
    PE_REQUIRE(!oracle.empty());

    const MaskBuffer out = resampleMask(src, Rect{0, 0, 120, 90}, Rect{0, 0, 40, 30});
    int mismatches = 0;
    for (int y = 0; y < 30; ++y) {
        for (int x = 0; x < 40; ++x) {
            if (out.value(x, y) != oracle[static_cast<std::size_t>(y) * 40 + x]) ++mismatches;
        }
    }
    PE_CHECK_EQ(mismatches, 0);
}

PE_TEST(resamplemask_degenerate_canvas_is_empty) {
    MaskBuffer src;
    src.setValue(1, 1, MaskBuffer::kClear);
    PE_CHECK(resampleMask(src, Rect{}, Rect{0, 0, 8, 8}).contentBounds().isEmpty());
    PE_CHECK(resampleMask(src, Rect{0, 0, 8, 8}, Rect{}).contentBounds().isEmpty());
}

PE_TEST(resamplemask_offset_canvas_writes_at_destination_origin) {
    // srcCanvas and dstCanvas need not start at (0,0): a mask sampled over an offset source canvas
    // must land at the destination canvas origin, not the source's.
    MaskBuffer src;
    for (int y = 100; y < 120; ++y) {
        for (int x = 100; x < 110; ++x) src.setValue(x, y, MaskBuffer::kClear);
    }
    const MaskBuffer out = resampleMask(src, Rect{100, 100, 20, 20}, Rect{0, 0, 40, 40});
    PE_CHECK_EQ(out.value(5, 10), MaskBuffer::kClear);    // (100..110)->(0..20) at 2x
    PE_CHECK_EQ(out.value(30, 10), MaskBuffer::kOpaque);  // beyond the scaled block
}

// ------------------------------------------------------------------- resampledSelection

namespace {
// A rectangular selection at (x,y,w,h): loadMask a fully-covered buffer at that origin.
Selection rectSelection(int x, int y, int w, int h) {
    Selection sel;
    PixelBuffer mask(w, h, Rgba8{255, 255, 255, 255});
    sel.loadMask(mask, x, y);
    return sel;
}
}  // namespace

PE_TEST(resampledselection_inactive_stays_inactive) {
    Selection sel;  // default: inactive == everything selected
    const Selection out = resampledSelection(sel, Rect{0, 0, 100, 100}, Rect{0, 0, 50, 50});
    PE_CHECK(!out.active());
}

PE_TEST(resampledselection_scales_a_rectangle_with_the_canvas) {
    Selection sel = rectSelection(20, 20, 40, 40);  // in a 100x100 canvas
    PE_REQUIRE(sel.active());
    const Selection out = resampledSelection(sel, Rect{0, 0, 100, 100}, Rect{0, 0, 200, 200});
    PE_REQUIRE(out.active());
    const Rect tb = out.tightBounds();
    PE_CHECK_EQ(tb.x, 40);
    PE_CHECK_EQ(tb.y, 40);
    PE_CHECK_EQ(tb.width, 80);
    PE_CHECK_EQ(tb.height, 80);
    PE_CHECK(out.coverage(80, 80) > 0.5);  // interior selected
    PE_CHECK(out.coverage(10, 10) < 0.5);  // outside not selected (still active)
}

PE_TEST(resampledselection_downscale_that_rounds_away_keeps_the_selection) {
    // A one-pixel-wide selection shrunk hard rounds its width to zero. Loading that would
    // deactivate the selection ("select everything"); the guard must keep the original instead.
    Selection sel = rectSelection(10, 10, 1, 40);
    PE_REQUIRE(sel.active());
    const Selection out = resampledSelection(sel, Rect{0, 0, 100, 100}, Rect{0, 0, 30, 30});
    PE_REQUIRE(out.active());                 // NOT silently turned into select-all
    PE_CHECK_EQ(out.tightBounds().width, 1);  // unchanged
}

PE_TEST(resampledselection_downscale_shrinks_bounds) {
    Selection sel = rectSelection(0, 0, 80, 80);  // in a 160x160 canvas
    const Selection out = resampledSelection(sel, Rect{0, 0, 160, 160}, Rect{0, 0, 80, 80});
    PE_REQUIRE(out.active());
    const Rect tb = out.tightBounds();
    PE_CHECK_EQ(tb.width, 40);
    PE_CHECK_EQ(tb.height, 40);
}

PE_TEST(resampledselection_degenerate_canvas_is_a_no_op) {
    Selection sel = rectSelection(5, 5, 10, 10);
    PE_CHECK(resampledSelection(sel, Rect{}, Rect{0, 0, 8, 8}).tightBounds() == sel.tightBounds());
    PE_CHECK(resampledSelection(sel, Rect{0, 0, 8, 8}, Rect{}).tightBounds() == sel.tightBounds());
}
