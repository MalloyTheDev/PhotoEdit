// The gradient ramp: stops, ordering, and the two stop colours that follow the loaded
// foreground/background rather than carrying their own.

#include "pe/core/Color.hpp"
#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Filter.hpp"
#include "pe/core/Gradient.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Tile.hpp"
#include "pe_test.hpp"

#include <cmath>
#include <memory>
#include <vector>

using namespace pe;

namespace {

constexpr Rgbaf kBlack{0.0f, 0.0f, 0.0f, 1.0f};
constexpr Rgbaf kWhite{1.0f, 1.0f, 1.0f, 1.0f};
constexpr Rgbaf kRed{1.0f, 0.0f, 0.0f, 1.0f};
constexpr Rgbaf kBlue{0.0f, 0.0f, 1.0f, 1.0f};

bool near(float a, float b) {
    return std::fabs(a - b) < 0.002f;
}

bool sameColor(Rgbaf a, Rgbaf b) {
    return near(a.r, b.r) && near(a.g, b.g) && near(a.b, b.b) && near(a.a, b.a);
}

}  // namespace

PE_TEST(gradient_defaults_to_black_to_white) {
    const Gradient g;
    PE_CHECK(sameColor(g.sample(0.0f, kRed, kBlue), kBlack));
    PE_CHECK(sameColor(g.sample(1.0f, kRed, kBlue), kWhite));
    PE_CHECK(near(g.sample(0.5f, kRed, kBlue).r, 0.5f));
    PE_CHECK(g.isFixed());  // nothing in it follows the loaded colours
}

PE_TEST(gradient_interpolates_between_the_stops_it_is_given) {
    const Gradient g(
        {GradientStop{0.0f, kRed, StopColor::Fixed}, GradientStop{1.0f, kBlue, StopColor::Fixed}});
    PE_CHECK(sameColor(g.sample(0.0f, kWhite, kWhite), kRed));
    PE_CHECK(sameColor(g.sample(1.0f, kWhite, kWhite), kBlue));
    const Rgbaf mid = g.sample(0.5f, kWhite, kWhite);
    PE_CHECK(near(mid.r, 0.5f));
    PE_CHECK(near(mid.b, 0.5f));
    PE_CHECK(near(mid.g, 0.0f));
}

PE_TEST(gradient_a_middle_stop_is_actually_reached) {
    // The whole reason the ramp is not two colours: a stop between the ends has to bend the
    // ramp, or every preset collapses to a straight fade from first to last.
    const Gradient g({GradientStop{0.0f, kBlack, StopColor::Fixed},
                      GradientStop{0.5f, kRed, StopColor::Fixed},
                      GradientStop{1.0f, kWhite, StopColor::Fixed}});
    PE_CHECK(sameColor(g.sample(0.5f, kBlue, kBlue), kRed));
    // A quarter of the way in is halfway from black to red, not a quarter of black to white.
    const Rgbaf quarter = g.sample(0.25f, kBlue, kBlue);
    PE_CHECK(near(quarter.r, 0.5f));
    PE_CHECK(near(quarter.g, 0.0f));
}

PE_TEST(gradient_stops_are_sorted_and_clamped_on_the_way_in) {
    // sample() walks the stops in order, so it can only assume order if setStops enforces it.
    const Gradient g({GradientStop{1.5f, kWhite, StopColor::Fixed},   // past the end
                      GradientStop{-0.5f, kBlack, StopColor::Fixed},  // before the start
                      GradientStop{0.5f, kRed, StopColor::Fixed}});
    PE_REQUIRE(g.stops().size() == 3);
    PE_CHECK(near(g.stops()[0].position, 0.0f));
    PE_CHECK(near(g.stops()[1].position, 0.5f));
    PE_CHECK(near(g.stops()[2].position, 1.0f));
    PE_CHECK(sameColor(g.sample(0.0f, kBlue, kBlue), kBlack));
    PE_CHECK(sameColor(g.sample(0.5f, kBlue, kBlue), kRed));
    PE_CHECK(sameColor(g.sample(1.0f, kBlue, kBlue), kWhite));
}

PE_TEST(gradient_fewer_than_two_stops_is_not_a_ramp) {
    // A one-stop "gradient" has no axis to interpolate along. Falling back to black-to-white
    // keeps every caller (the fill, the preview swatches) working on a real ramp instead of
    // guarding for a shape that cannot be drawn.
    const Gradient one({GradientStop{0.3f, kRed, StopColor::Fixed}});
    PE_CHECK(one.stops().size() == 2);
    PE_CHECK(sameColor(one.sample(0.0f, kBlue, kBlue), kBlack));
    PE_CHECK(sameColor(one.sample(1.0f, kBlue, kBlue), kWhite));

    const Gradient none{std::vector<GradientStop>{}};
    PE_CHECK(none.stops().size() == 2);
}

PE_TEST(gradient_holds_the_end_colours_outside_the_stop_range) {
    // Stops that do not reach the ends must not fade to transparent there: the ramp holds.
    const Gradient g({GradientStop{0.25f, kRed, StopColor::Fixed},
                      GradientStop{0.75f, kBlue, StopColor::Fixed}});
    PE_CHECK(sameColor(g.sample(0.0f, kWhite, kWhite), kRed));
    PE_CHECK(sameColor(g.sample(0.1f, kWhite, kWhite), kRed));
    PE_CHECK(sameColor(g.sample(1.0f, kWhite, kWhite), kBlue));
    // And out-of-range t is clamped rather than extrapolated.
    PE_CHECK(sameColor(g.sample(-3.0f, kWhite, kWhite), kRed));
    PE_CHECK(sameColor(g.sample(9.0f, kWhite, kWhite), kBlue));
}

PE_TEST(gradient_two_stops_at_one_position_are_a_hard_edge) {
    const Gradient g(
        {GradientStop{0.0f, kRed, StopColor::Fixed}, GradientStop{0.5f, kRed, StopColor::Fixed},
         GradientStop{0.5f, kBlue, StopColor::Fixed}, GradientStop{1.0f, kBlue, StopColor::Fixed}});
    PE_CHECK(sameColor(g.sample(0.49f, kWhite, kWhite), kRed));
    PE_CHECK(sameColor(g.sample(0.51f, kWhite, kWhite), kBlue));
    PE_CHECK(sameColor(g.sample(0.5f, kWhite, kWhite), kRed));  // the edge itself is the low side
}

PE_TEST(gradient_foreground_and_background_stops_follow_the_loaded_colours) {
    // The point of the stop sources: one preset, whatever colours are loaded.
    const Gradient g = Gradient::foregroundToBackground();
    PE_CHECK(!g.isFixed());
    PE_CHECK(sameColor(g.sample(0.0f, kRed, kBlue), kRed));
    PE_CHECK(sameColor(g.sample(1.0f, kRed, kBlue), kBlue));
    // Load different colours and the same gradient draws differently.
    PE_CHECK(sameColor(g.sample(0.0f, kWhite, kBlack), kWhite));
    PE_CHECK(sameColor(g.sample(1.0f, kWhite, kBlack), kBlack));
}

PE_TEST(gradient_foreground_to_transparent_keeps_the_stop_s_own_alpha) {
    // A Foreground stop takes the live RGB and its OWN alpha; if it took the live alpha too,
    // this preset would be a flat wash instead of a fade.
    const Gradient g = Gradient::foregroundToTransparent();
    const Rgbaf fg{0.2f, 0.4f, 0.6f, 1.0f};
    const Rgbaf start = g.sample(0.0f, fg, kWhite);
    const Rgbaf end = g.sample(1.0f, fg, kWhite);
    PE_CHECK(near(start.r, 0.2f));
    PE_CHECK(near(start.a, 1.0f));
    PE_CHECK(near(end.r, 0.2f));  // same colour
    PE_CHECK(near(end.a, 0.0f));  // gone
    PE_CHECK(near(g.sample(0.5f, fg, kWhite).a, 0.5f));
}

PE_TEST(gradient_fill_draws_the_middle_stop_onto_the_canvas) {
    // The ramp reaching the pixels, not just sample() being right: a three-stop gradient drawn
    // across the canvas must show its middle colour in the middle.
    auto doc = Document::createBlank(Size{64, 1});
    PE_REQUIRE(doc != nullptr);
    const LayerId id = doc->activeLayer();

    const Gradient g({GradientStop{0.0f, kBlack, StopColor::Fixed},
                      GradientStop{0.5f, kRed, StopColor::Fixed},
                      GradientStop{1.0f, kWhite, StopColor::Fixed}});
    auto cmd = gradientFill(*doc, id, Point{0, 0}, Point{64, 0}, g, kBlue, kBlue);
    PE_REQUIRE(cmd != nullptr);
    doc->history().push(std::move(cmd));

    const auto* pl = static_cast<const PixelLayer*>(doc->findLayer(id));
    PE_REQUIRE(pl != nullptr);
    const Rgba8 mid = pl->tiles().pixel(32, 0);
    PE_CHECK(mid.r > 230);  // red at the middle stop
    PE_CHECK(mid.g < 25);
    PE_CHECK(mid.b < 25);
    const Rgba8 left = pl->tiles().pixel(0, 0);
    PE_CHECK(left.r < 25);  // black at the start
    const Rgba8 right = pl->tiles().pixel(63, 0);
    PE_CHECK(right.r > 230 && right.g > 230 && right.b > 230);  // white at the end
}

PE_TEST(gradient_fill_substitutes_the_colours_it_is_handed) {
    auto doc = Document::createBlank(Size{64, 1});
    const LayerId id = doc->activeLayer();
    auto cmd = gradientFill(*doc, id, Point{0, 0}, Point{64, 0}, Gradient::foregroundToBackground(),
                            kRed, kBlue);
    PE_REQUIRE(cmd != nullptr);
    doc->history().push(std::move(cmd));

    const auto* pl = static_cast<const PixelLayer*>(doc->findLayer(id));
    PE_CHECK(pl->tiles().pixel(0, 0).r > 230);   // foreground at the start
    PE_CHECK(pl->tiles().pixel(63, 0).b > 230);  // background at the end
}

PE_TEST(gradient_fill_two_colour_overload_matches_the_ramp_overload) {
    // The old signature is the new one with a two-stop ramp; if they drifted, every existing
    // caller would quietly draw something else.
    auto a = Document::createBlank(Size{32, 1});
    auto b = Document::createBlank(Size{32, 1});
    const LayerId ida = a->activeLayer();
    const LayerId idb = b->activeLayer();

    auto ca = gradientFill(*a, ida, Point{0, 0}, Point{32, 0}, kRed, kBlue);
    auto cb = gradientFill(*b, idb, Point{0, 0}, Point{32, 0}, Gradient::twoStop(kRed, kBlue),
                           kWhite, kWhite);
    PE_REQUIRE(ca != nullptr && cb != nullptr);
    a->history().push(std::move(ca));
    b->history().push(std::move(cb));

    const auto* pa = static_cast<const PixelLayer*>(a->findLayer(ida));
    const auto* pb = static_cast<const PixelLayer*>(b->findLayer(idb));
    for (int x = 0; x < 32; ++x) {
        PE_CHECK(pa->tiles().pixel(x, 0) == pb->tiles().pixel(x, 0));
    }
}
