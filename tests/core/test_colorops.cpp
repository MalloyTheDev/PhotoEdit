#include "pe/core/ColorProfile.hpp"
#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe_test.hpp"

#ifdef PHOTOEDIT_HAVE_LCMS2

#include <memory>

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

#endif  // PHOTOEDIT_HAVE_LCMS2
