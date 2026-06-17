#include "pe/core/ColorProfile.hpp"
#include "pe_test.hpp"

// The color module is only built when lcms2 is available; the macro is exported by
// pe_core. Where it's absent (e.g. Windows CI until vcpkg provisions lcms2), this
// file compiles to nothing so the link stays clean.
#ifdef PHOTOEDIT_HAVE_LCMS2

#include <cstddef>
#include <span>
#include <vector>

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

PE_TEST(colorprofile_from_truncated_icc_is_null) {
    // An ICC-shaped header (the 'acsp' signature at offset 36) with an inconsistent body
    // must be rejected, not mis-parsed — the bytes are untrusted (embedded in opened image
    // files). Here the header is well-formed but the tag-table count (offset 128) is absurd
    // and the directory it claims runs off the end of the buffer; lcms2 must reject it.
    std::vector<std::byte> icc(132, std::byte{0});
    // Bytes 0..3: declared total profile size, big-endian = 0x00040000 (256 KiB) — but only
    // 132 bytes are actually provided (a truncated body).
    icc[0] = std::byte{0x00};
    icc[1] = std::byte{0x04};
    icc[2] = std::byte{0x00};
    icc[3] = std::byte{0x00};
    // Bytes 36..39: the ICC profile-file signature 'acsp'.
    icc[36] = std::byte{'a'};
    icc[37] = std::byte{'c'};
    icc[38] = std::byte{'s'};
    icc[39] = std::byte{'p'};
    // Bytes 128..131: tag count = 0xFFFFFFFF — far past any real profile, and a directory of
    // that many 12-byte entries cannot fit, so the parse must fail rather than over-read.
    icc[128] = std::byte{0xFF};
    icc[129] = std::byte{0xFF};
    icc[130] = std::byte{0xFF};
    icc[131] = std::byte{0xFF};
    PE_CHECK(ColorProfile::fromIccData(icc) == nullptr);  // inconsistent -> rejected, no crash
}

#endif  // PHOTOEDIT_HAVE_LCMS2
