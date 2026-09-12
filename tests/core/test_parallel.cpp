// #170: filter kernels and the bake tile-delta loop run across a small thread pool. The
// whole point of the parallelisation is that it changes nothing observable, so these
// tests pin two properties: parallelFor partitions a range into disjoint pieces that
// together cover it exactly (for any lane count), and every parallelised kernel plus the
// end-to-end bake produce BYTE-IDENTICAL output at 1 lane and at many lanes. Bit-exact
// (memcmp) is deliberate: a partition that split a reduction, or a kernel that shared a
// scratch buffer across threads, would drift in the low bits, and a looser compare would
// hide exactly the bug this guards against.

#include "pe/core/Color.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Filter.hpp"
#include "pe/core/Parallel.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe_test.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace pe;

namespace {

// Pin the lane count parallelThreadCount() reports, so a test can compare serial against
// parallel deterministically. Cross-platform: MinGW/MSVC have _putenv_s, POSIX has setenv.
void setThreads(int n) {
    const std::string v = std::to_string(n);
#ifdef _WIN32
    _putenv_s("PHOTOEDIT_THREADS", v.c_str());
#else
    setenv("PHOTOEDIT_THREADS", v.c_str(), 1);
#endif
}

// A varied, fully deterministic source: every channel (alpha included) carries a
// different hashed gradient, so a kernel has real structure to work on and a transparent
// -pixel bug would show. 256 wide x 1024 tall is chosen so rowsPerChunk(256) == 256 and
// the pass actually splits (2*256 <= 1024) at more than one lane.
std::vector<Rgbaf> makeSource(int w, int h) {
    std::vector<Rgbaf> v(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::uint32_t k = static_cast<std::uint32_t>(x) * 2654435761u ^
                                    static_cast<std::uint32_t>(y) * 40503u;
            const auto f = [k](int s) { return static_cast<float>((k >> s) & 0xFFu) / 255.0f; };
            v[static_cast<std::size_t>(y) * w + x] = Rgbaf{f(0), f(8), f(16), f(24)};
        }
    }
    return v;
}

bool bitEqual(const std::vector<Rgbaf>& a, const std::vector<Rgbaf>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(Rgbaf)) == 0;
}

}  // namespace

PE_TEST(parallelfor_covers_the_range_exactly_once_for_any_lane_count) {
    constexpr int N = 5000;
    for (const int lanes : {1, 2, 3, 4, 7, 16}) {
        setThreads(lanes);
        // atomic, so a partition bug that double-visits an index is caught without the
        // test itself being a data race.
        std::vector<std::atomic<int>> hits(N);
        for (auto& a : hits) a.store(0);
        std::atomic<int> outOfRange{0};
        parallelFor(0, N, 17, [&](int lo, int hi) {
            for (int i = lo; i < hi; ++i) {
                if (i < 0 || i >= N) {
                    outOfRange.fetch_add(1);
                } else {
                    hits[static_cast<std::size_t>(i)].fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        PE_CHECK_EQ(outOfRange.load(), 0);
        int visitedExactlyOnce = 0;
        for (auto& a : hits) {
            if (a.load() == 1) ++visitedExactlyOnce;
        }
        PE_CHECK_EQ(visitedExactlyOnce, N);
    }
}

PE_TEST(parallelfor_empty_and_singleton_ranges_are_safe) {
    setThreads(8);
    int calls = 0;
    parallelFor(0, 0, 1, [&](int, int) { ++calls; });
    PE_CHECK_EQ(calls, 0);  // nothing to do -> body never runs
    parallelFor(5, 5, 1, [&](int, int) { ++calls; });
    PE_CHECK_EQ(calls, 0);
    int sum = 0;
    parallelFor(0, 1, 1, [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i) sum += i + 1;
    });
    PE_CHECK_EQ(sum, 1);  // single element still processed exactly once
}

PE_TEST(parallelthreadcount_honours_the_override) {
    setThreads(1);
    PE_CHECK_EQ(parallelThreadCount(), 1);
    setThreads(4);
    PE_CHECK_EQ(parallelThreadCount(), 4);
    setThreads(999);
    PE_CHECK(parallelThreadCount() <= 64);  // clamped
}

PE_TEST(kernels_are_byte_identical_across_lane_counts) {
    const int w = 256;
    const int h = 1024;  // forces a real split: 1024 >= 2 * rowsPerChunk(256)
    const std::vector<Rgbaf> src = makeSource(w, h);
    const std::size_t n = src.size();

    // Each entry runs one kernel into `dst`. Reused for the serial reference and each
    // parallel run; a kernel that read uninitialised dst would show up as a mismatch.
    struct Case {
        const char* name;
        std::function<void(const std::vector<Rgbaf>&, std::vector<Rgbaf>&)> run;
    };
    const std::vector<Case> cases = {
        {"boxBlur", [&](const auto& s, auto& d) { boxBlur(s, d, w, h, 3); }},
        {"gaussianBlur", [&](const auto& s, auto& d) { gaussianBlur(s, d, w, h, 4.0f); }},
        {"unsharpMask",
         [&](const auto& s, auto& d) { unsharpMask(s, d, w, h, 3.0f, 1.5f, 0.05f); }},
        {"mosaic", [&](const auto& s, auto& d) { mosaic(s, d, w, h, 7); }},
        {"medianFilter", [&](const auto& s, auto& d) { medianFilter(s, d, w, h, 2); }},
        {"findEdges", [&](const auto& s, auto& d) { findEdges(s, d, w, h); }},
        {"addNoise_uniform",
         [&](const auto& s, auto& d) { addNoise(s, d, w, h, 0.4f, false, false, 12345u); }},
        {"addNoise_gaussian_mono",
         [&](const auto& s, auto& d) { addNoise(s, d, w, h, 0.4f, true, true, 777u); }},
    };

    for (const Case& c : cases) {
        setThreads(1);
        std::vector<Rgbaf> ref(n);
        c.run(src, ref);

        for (const int lanes : {2, 4, 8}) {
            setThreads(lanes);
            std::vector<Rgbaf> got(n);
            c.run(src, got);
            const bool ok = bitEqual(ref, got);
            PE_CHECK(ok);
            if (!ok) {
                // Name the offender; PE_CHECK alone would not say which kernel drifted.
                std::printf("  kernel '%s' differs at %d lanes\n", c.name, lanes);
            }
        }
        // Re-running at the same (multi-)lane count must also match: catches a race that
        // only sometimes corrupts, not just a systematic partition difference.
        setThreads(8);
        std::vector<Rgbaf> again(n);
        c.run(src, again);
        PE_CHECK(bitEqual(ref, again));
    }
}

PE_TEST(bake_tile_delta_loop_is_identical_across_lane_counts) {
    // End-to-end: a real bake whose transform is itself a parallel kernel (gaussianBlur),
    // over a canvas spanning many tiles, so BOTH the kernel pass and the tile-delta diff
    // run in parallel. The applied pixels must match at 1 lane and at 8.
    const int W = 1024;
    const int H = 768;  // 4 x 3 tiles of content
    const auto blurTransform = [](std::span<Rgbaf> s, int w, int h) {
        std::vector<Rgbaf> tmp(s.begin(), s.end());
        gaussianBlur(tmp, s, w, h, 3.0f);
    };

    const auto run = [&](int lanes) {
        setThreads(lanes);
        auto doc = Document::createBlank(Size{W, H});
        const LayerId base = doc->activeLayer();
        auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
        // Varied content across the whole canvas so every tile actually changes.
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const auto r = static_cast<std::uint8_t>((x * 3 + y) & 0xFF);
                const auto g = static_cast<std::uint8_t>((x ^ (y * 5)) & 0xFF);
                const auto b = static_cast<std::uint8_t>((x + y * 7) & 0xFF);
                pl->tiles().setPixel(x, y, Rgba8{r, g, b, 255});
            }
        }
        auto cmd = bakePixelEdit(*doc, base, "blur", blurTransform);
        PE_CHECK(cmd != nullptr);  // PE_REQUIRE would `return;`, illegal in this value lambda
        if (cmd) doc->history().push(std::move(cmd));  // executes the command

        std::vector<Rgba8> out(static_cast<std::size_t>(W) * static_cast<std::size_t>(H));
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                out[static_cast<std::size_t>(y) * W + x] = pl->tiles().pixel(x, y);
            }
        }
        return out;
    };

    // run() PE_CHECKs a non-null command, and bakePixelEdit returns null when nothing
    // changed, so a no-op blur would fail the test rather than silently compare two
    // untouched images.
    const std::vector<Rgba8> ref = run(1);
    const std::vector<Rgba8> par = run(8);
    PE_REQUIRE(ref.size() == par.size());
    const bool ok = std::memcmp(ref.data(), par.data(), ref.size() * sizeof(Rgba8)) == 0;
    PE_CHECK(ok);
}
