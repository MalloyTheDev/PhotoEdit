#pragma once

// Golden-image comparison for subsystems with visual output.
//
// docs/04-coding-standards.md requires these ("committed reference PNGs compared within
// tolerance") and until now none existed, so no filter, adjustment, brush, gradient or
// composite had regression coverage of what it actually draws. That matters most for the
// optimization work in flight: replacing an exact kernel with a faster equivalent (a
// running-sum box blur, a constant-time median) is precisely the change a golden test
// exists to police, and there was no way to demonstrate the output had not moved.
//
// Two deliberate design points:
//
//   - Tolerance is TWO numbers, not one. A faster algorithm typically shifts many pixels
//     by a least significant bit, which a per-pixel bound alone would either forbid
//     (blocking a legitimate rewrite) or have to loosen so far that real regressions slip
//     through. Bounding the worst single channel AND the mean absolute difference lets an
//     LSB-level rewrite pass while still catching a shifted, scaled or differently-shaped
//     result.
//
//   - A golden on its own can lock in a bug: whatever the code did the day the reference
//     was generated becomes the specification. So every golden case here is paired with a
//     cheap structural assertion in the test itself (a blur reduces variance, a gradient
//     increases monotonically), which a wrong reference cannot satisfy.
//
// Regenerating: run the test binary with PE_GOLDEN_UPDATE=1 in the environment. Each case
// writes its reference and reports it. Review the images before committing them; a
// regenerated golden is a claim that the new output is correct.
//
// Requires PNG support, so these are compiled only where PHOTOEDIT_HAVE_PNG is defined.
// The no-optional-deps CI lane skips them, which is why the structural assertions above
// are not merely belt and braces: they are the part that still runs everywhere.

#include "pe/core/ImageIO.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe_test.hpp"

#ifdef PHOTOEDIT_HAVE_PNG

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

namespace pe_golden {

// How far the actual image may drift from its reference.
struct Tolerance {
    // No single channel of any pixel may differ by more than this. Catches a localized
    // regression that a mean would average away.
    int maxChannelDelta = 0;
    // And the mean absolute difference across every channel must stay under this. Catches
    // a uniform shift that slips under the per-pixel bound.
    double maxMeanDelta = 0.0;
};

// Exact: for anything whose output is defined bit for bit.
inline constexpr Tolerance kExact{0, 0.0};
// For a kernel that may be reimplemented with different rounding, e.g. a running-sum box
// blur replacing the naive one. One LSB per channel, and the average drift must stay well
// below that, so a wholesale shift still fails.
inline constexpr Tolerance kKernelRewrite{1, 0.25};

namespace detail {

inline std::filesystem::path goldenDir() {
    return std::filesystem::path(PE_GOLDEN_DIR);
}

inline std::filesystem::path goldenPath(const std::string& name) {
    return goldenDir() / (name + ".png");
}

inline bool updating() {
    const char* v = std::getenv("PE_GOLDEN_UPDATE");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}

inline bool writePng(const std::filesystem::path& p, const pe::PixelBuffer& img) {
    const std::vector<std::byte> bytes = pe::encodePng(img);
    if (bytes.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

inline bool readPng(const std::filesystem::path& p, pe::PixelBuffer& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    const std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto decoded = pe::decodePng(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(raw.data()), raw.size()));
    if (!decoded.has_value()) return false;
    out = std::move(*decoded);
    return true;
}

}  // namespace detail

// Compare `actual` against the committed reference for `name`. Returns false (and reports
// through the test harness) on any mismatch, so a caller can add its own detail.
//
// On failure the actual image is written next to the reference as <name>.actual.png, so
// the two can be opened side by side rather than reasoning about pixel indices.
inline bool checkGolden(const char* name, const pe::PixelBuffer& actual, Tolerance tol) {
    const std::string id(name);
    const std::filesystem::path ref = detail::goldenPath(id);

    if (detail::updating()) {
        const bool ok = detail::writePng(ref, actual);
        PE_CHECK(ok);
        return ok;
    }

    pe::PixelBuffer expected;
    if (!detail::readPng(ref, expected)) {
        // Missing reference is a failure, not a silent pass: a test that checks nothing is
        // worse than no test, because it reads as coverage.
        pe_test::reportFailure(std::string("golden reference missing: ") + ref.string() +
                               " (regenerate with PE_GOLDEN_UPDATE=1)");
        return false;
    }
    if (expected.width() != actual.width() || expected.height() != actual.height()) {
        pe_test::reportFailure(std::string("golden size mismatch for ") + id);
        (void)detail::writePng(detail::goldenDir() / (id + ".actual.png"), actual);
        return false;
    }

    int worst = 0;
    std::int64_t sum = 0;
    std::int64_t samples = 0;
    for (int y = 0; y < actual.height(); ++y) {
        for (int x = 0; x < actual.width(); ++x) {
            const pe::Rgba8 a = actual.at(x, y);
            const pe::Rgba8 e = expected.at(x, y);
            const int d[4] = {std::abs(static_cast<int>(a.r) - static_cast<int>(e.r)),
                              std::abs(static_cast<int>(a.g) - static_cast<int>(e.g)),
                              std::abs(static_cast<int>(a.b) - static_cast<int>(e.b)),
                              std::abs(static_cast<int>(a.a) - static_cast<int>(e.a))};
            for (const int v : d) {
                worst = worst > v ? worst : v;
                sum += v;
                ++samples;
            }
        }
    }
    const double mean = samples > 0 ? static_cast<double>(sum) / static_cast<double>(samples) : 0.0;

    const bool ok = worst <= tol.maxChannelDelta && mean <= tol.maxMeanDelta;
    if (!ok) {
        (void)detail::writePng(detail::goldenDir() / (id + ".actual.png"), actual);
        pe_test::reportFailure(id + ": worst channel delta " + std::to_string(worst) +
                               " (allowed " + std::to_string(tol.maxChannelDelta) + "), mean " +
                               std::to_string(mean) + " (allowed " +
                               std::to_string(tol.maxMeanDelta) + "); wrote " + id +
                               ".actual.png next to the reference");
    }
    return ok;
}

}  // namespace pe_golden

#endif  // PHOTOEDIT_HAVE_PNG
