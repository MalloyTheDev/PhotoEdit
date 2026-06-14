#include "pe/core/ColorProfile.hpp"
#include "pe_test.hpp"

// The color module is only built when lcms2 is available; the macro is exported by
// pe_core. Where it's absent (e.g. Windows CI until vcpkg provisions lcms2), this
// file compiles to nothing so the link stays clean.
#ifdef PHOTOEDIT_HAVE_LCMS2

#include <cstddef>
#include <span>

using namespace pe;

PE_TEST(colorprofile_builtin_srgb) {
    auto p = ColorProfile::sRGB();
    PE_CHECK(p != nullptr);
    PE_CHECK(p->valid());
    PE_CHECK(p->nativeHandle() != nullptr);
    PE_CHECK(p->mode() == ColorMode::RGB);
    PE_CHECK(!p->description().empty());  // lcms labels it (e.g. "sRGB built-in")
}

PE_TEST(colorprofile_from_empty_is_null) {
    PE_CHECK(ColorProfile::fromIccData(std::span<const std::byte>{}) == nullptr);
}

PE_TEST(colorprofile_from_garbage_is_null) {
    const std::byte junk[8]{};  // not a valid ICC header
    PE_CHECK(ColorProfile::fromIccData(std::span<const std::byte>(junk, 8)) == nullptr);
}

#endif  // PHOTOEDIT_HAVE_LCMS2
