#include "pe/core/ColorOps.hpp"
#include "pe/core/ColorProfile.hpp"
#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe_test.hpp"

#ifdef PHOTOEDIT_HAVE_LCMS2

#include <cstdlib>
#include <memory>

#include "pe/core/PixelLayer.hpp"

using namespace pe;

namespace {
bool near8(Rgba8 a, Rgba8 b, int tol = 1) {
    auto d = [](uint8_t x, uint8_t y) {
        return std::abs(static_cast<int>(x) - static_cast<int>(y));
    };
    return d(a.r, b.r) <= tol && d(a.g, b.g) <= tol && d(a.b, b.b) <= tol && d(a.a, b.a) <= tol;
}
}  // namespace

PE_TEST(assign_profile_tags_and_undoes) {
    auto doc = Document::createBlank(Size{8, 8});
    PE_CHECK(doc->colorProfile() == nullptr);  // a fresh document is untagged

    auto srgb = ColorProfile::builtin(BuiltinSpace::sRGB);
    doc->history().push(std::make_unique<AssignProfileCommand>(srgb));
    PE_CHECK(doc->colorProfile() == srgb);
    PE_CHECK(doc->colorProfile()->mode() == ColorMode::RGB);

    doc->history().undo();
    PE_CHECK(doc->colorProfile() == nullptr);  // restored to untagged

    doc->history().redo();
    PE_CHECK(doc->colorProfile() == srgb);  // re-tagged
}

PE_TEST(assign_profile_replaces_previous) {
    auto doc = Document::createBlank(Size{8, 8});
    auto srgb = ColorProfile::builtin(BuiltinSpace::sRGB);
    auto adobe = ColorProfile::builtin(BuiltinSpace::AdobeRGB1998);

    doc->history().push(std::make_unique<AssignProfileCommand>(srgb));
    doc->history().push(std::make_unique<AssignProfileCommand>(adobe));
    PE_CHECK(doc->colorProfile() == adobe);

    doc->history().undo();
    PE_CHECK(doc->colorProfile() == srgb);  // undo the second assign -> back to sRGB
    doc->history().undo();
    PE_CHECK(doc->colorProfile() == nullptr);  // undo the first -> untagged
}

PE_TEST(convert_profile_transforms_pixels_and_retags_with_undo) {
    auto doc = Document::createBlank(Size{16, 16});
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(Rect{0, 0, 16, 16}, Rgba8{200, 60, 60, 255});  // a saturated red
    const Rgba8 before = pl->tiles().pixel(8, 8);

    auto srgb = ColorProfile::builtin(BuiltinSpace::sRGB);
    auto adobe = ColorProfile::builtin(BuiltinSpace::AdobeRGB1998);
    doc->history().push(std::make_unique<AssignProfileCommand>(srgb));  // tag as sRGB first

    auto cmd = convertToProfile(*doc, adobe);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));
    PE_CHECK(doc->colorProfile() == adobe);          // re-tagged to the destination
    PE_CHECK(!(pl->tiles().pixel(8, 8) == before));  // pixels were transformed

    doc->history().undo();
    PE_CHECK(doc->colorProfile() == srgb);        // profile restored
    PE_CHECK(pl->tiles().pixel(8, 8) == before);  // pixels restored exactly
}

PE_TEST(convert_profile_requires_source_and_target) {
    auto doc = Document::createBlank(Size{8, 8});  // untagged
    auto adobe = ColorProfile::builtin(BuiltinSpace::AdobeRGB1998);
    PE_CHECK(convertToProfile(*doc, adobe) == nullptr);  // no source tag -> null

    doc->history().push(
        std::make_unique<AssignProfileCommand>(ColorProfile::builtin(BuiltinSpace::sRGB)));
    PE_CHECK(convertToProfile(*doc, nullptr) == nullptr);  // no destination -> null
}

PE_TEST(display_convert_identity_when_profiles_match) {
    ColorEngine engine;
    auto srgb = ColorProfile::builtin(BuiltinSpace::sRGB);
    PixelBufferF working(2, 1);
    working.set(0, 0, Rgbaf{0.8f, 0.2f, 0.2f, 1.0f});
    working.set(1, 0, Rgbaf{0.1f, 0.5f, 0.9f, 1.0f});

    // working sRGB -> display sRGB is ~identity: equals a direct quantization.
    PixelBuffer disp = convertForDisplay(working, srgb, srgb, engine);
    PE_CHECK_EQ(disp.width(), 2);
    PE_CHECK(near8(disp.at(0, 0), toRgba8(working.at(0, 0))));
    PE_CHECK(near8(disp.at(1, 0), toRgba8(working.at(1, 0))));
}

PE_TEST(display_convert_changes_under_different_display_profile) {
    ColorEngine engine;
    auto srgb = ColorProfile::builtin(BuiltinSpace::sRGB);
    auto adobe = ColorProfile::builtin(BuiltinSpace::AdobeRGB1998);
    PixelBufferF working(1, 1);
    working.set(0, 0, Rgbaf{0.9f, 0.1f, 0.1f, 1.0f});  // saturated red

    PixelBuffer cm = convertForDisplay(working, srgb, adobe, engine);  // working->Adobe display
    PixelBuffer direct =
        convertForDisplay(working, nullptr, adobe, engine);  // no source -> fallback
    PE_CHECK(!near8(cm.at(0, 0), direct.at(0, 0)));          // CM result differs from raw quantize
    PE_CHECK(near8(direct.at(0, 0), toRgba8(working.at(0, 0))));  // fallback == direct quantize
}

PE_TEST(display_convert_empty_is_empty) {
    ColorEngine engine;
    auto srgb = ColorProfile::builtin(BuiltinSpace::sRGB);
    PixelBuffer disp = convertForDisplay(PixelBufferF{}, srgb, srgb, engine);
    PE_CHECK(disp.isEmpty());
}

PE_TEST(soft_proof_gamut_warning_marks_out_of_gamut) {
    auto prophoto = ColorProfile::builtin(BuiltinSpace::ProPhotoRGB);
    auto srgb = ColorProfile::builtin(BuiltinSpace::sRGB);
    PixelBufferF working(1, 1);
    working.set(0, 0, Rgbaf{0.0f, 1.0f, 0.0f, 1.0f});  // pure ProPhoto green (outside sRGB gamut)

    const Rgbaf alarm{1.0f, 0.0f, 1.0f, 1.0f};  // magenta alarm
    PixelBuffer warned =
        convertForProof(working, prophoto, srgb, srgb, RenderingIntent::RelativeColorimetric,
                        RenderingIntent::RelativeColorimetric, true, /*gamutCheck=*/true, alarm);
    // Out-of-gamut -> painted the configured alarm color (magenta).
    PE_CHECK(near8(warned.at(0, 0), Rgba8{255, 0, 255, 255}, 2));

    // Without the gamut check, the color is proofed normally (not the alarm magenta).
    PixelBuffer plain =
        convertForProof(working, prophoto, srgb, srgb, RenderingIntent::RelativeColorimetric,
                        RenderingIntent::RelativeColorimetric, true, /*gamutCheck=*/false, alarm);
    PE_CHECK(!near8(plain.at(0, 0), Rgba8{255, 0, 255, 255}, 2));
}

PE_TEST(soft_proof_in_gamut_color_not_alarmed) {
    auto srgb = ColorProfile::builtin(BuiltinSpace::sRGB);
    PixelBufferF working(1, 1);
    working.set(0, 0, Rgbaf{0.5f, 0.5f, 0.5f, 1.0f});  // neutral gray, in every gamut
    PixelBuffer p =
        convertForProof(working, srgb, srgb, srgb, RenderingIntent::RelativeColorimetric,
                        RenderingIntent::RelativeColorimetric, true, true,
                        Rgbaf{1.0f, 0.0f, 1.0f, 1.0f});         // magenta alarm
    PE_CHECK(!near8(p.at(0, 0), Rgba8{255, 0, 255, 255}, 4));   // NOT magenta
    PE_CHECK(near8(p.at(0, 0), toRgba8(working.at(0, 0)), 3));  // ~unchanged neutral
}

PE_TEST(soft_proof_fallback_without_profiles) {
    PixelBufferF working(1, 1);
    working.set(0, 0, Rgbaf{0.3f, 0.6f, 0.9f, 1.0f});
    PixelBuffer p = convertForProof(working, nullptr, nullptr, nullptr);  // no profiles -> direct
    PE_CHECK(near8(p.at(0, 0), toRgba8(working.at(0, 0))));
}

#endif  // PHOTOEDIT_HAVE_LCMS2
