#include "pe/core/Parallel.hpp"

#include <cstdlib>
#include <thread>
#include <vector>

namespace pe {

int parallelThreadCount() noexcept {
    // An explicit override wins, so a test can pin the lane count. atoi returns 0 on a
    // non-numeric value, which falls through to the hardware count below.
    int forced = 0;
#if defined(_MSC_VER)
    // The MSVC UCRT marks getenv() deprecated, and clang targeting that CRT enforces it
    // under -Werror; _dupenv_s is the sanctioned replacement (it allocates a copy to free).
    char* env = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&env, &len, "PHOTOEDIT_THREADS") == 0 && env != nullptr) {
        forced = std::atoi(env);
        std::free(env);
    }
#else
    if (const char* env = std::getenv("PHOTOEDIT_THREADS")) forced = std::atoi(env);
#endif
    if (forced >= 1) return forced < 64 ? forced : 64;

    const unsigned hc = std::thread::hardware_concurrency();
    if (hc == 0) return 1;  // unknowable; stay serial rather than guess
    return hc < 64u ? static_cast<int>(hc) : 64;
}

void parallelFor(int begin, int end, int minChunk, const std::function<void(int, int)>& body) {
    if (end <= begin) return;
    if (minChunk < 1) minChunk = 1;
    const int total = end - begin;
    const int lanes = parallelThreadCount();

    // Chunk count: never more than the lanes available, and never so many that a chunk
    // would fall below minChunk. Below 2*minChunk it is not worth a thread at all, so
    // run inline. This is also the whole of the single-thread path (lanes == 1).
    int chunks = total / minChunk;
    if (chunks > lanes) chunks = lanes;
    if (chunks <= 1 || total < 2 * minChunk) {
        body(begin, end);
        return;
    }

    // Contiguous, near-equal chunks whose boundaries are a pure function of
    // (begin, end, chunks): chunk c is [boundary(c), boundary(c+1)). The remainder is
    // spread one row at a time over the first `rem` chunks, so boundary(chunks) == end
    // exactly and no row is dropped or visited twice.
    const int base = total / chunks;
    const int rem = total % chunks;
    const auto boundary = [&](int c) noexcept { return begin + c * base + (c < rem ? c : rem); };

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(chunks - 1));
    for (int c = 1; c < chunks; ++c) {
        workers.emplace_back(body, boundary(c), boundary(c + 1));
    }
    body(boundary(0), boundary(1));  // the calling thread takes chunk 0
    for (std::thread& t : workers) t.join();
}

}  // namespace pe
