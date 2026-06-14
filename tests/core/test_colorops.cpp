#include "pe/core/ColorOps.hpp"
#include "pe/core/ColorProfile.hpp"
#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe_test.hpp"

#ifdef PHOTOEDIT_HAVE_LCMS2

#include <memory>

#include "pe/core/PixelLayer.hpp"

using namespace pe;

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

#endif  // PHOTOEDIT_HAVE_LCMS2
