#pragma once

#include <functional>

namespace pe {

// The number of worker lanes parallelFor divides work across (always >= 1).
//
// Defaults to std::thread::hardware_concurrency(), clamped to [1, 64]. The environment
// variable PHOTOEDIT_THREADS overrides it when set to a positive integer: the filter
// tests use that to force a specific lane count (including 1, the serial reference) and
// prove the output is byte-for-byte identical across counts.
[[nodiscard]] int parallelThreadCount() noexcept;

// Data-parallel for-loop over the half-open range [begin, end).
//
// The range is split into contiguous, disjoint sub-ranges and `body(lo, hi)` runs once
// per sub-range, some on worker threads and one on the calling thread; the call returns
// only after every sub-range has finished. `body` MUST confine its writes to outputs
// indexed by [lo, hi) and share nothing mutable with the other sub-ranges. Given that,
// the observable result does not depend on how the range was partitioned, so it is
// bit-identical for any lane count, including 1. That is the entire determinism
// contract: there is no reduction, no accumulation, and no ordering to preserve inside
// the loop, because each output element is produced by exactly one invocation with the
// same arithmetic the serial loop would use.
//
// `minChunk` is the smallest sub-range worth handing to another thread; when the range
// is shorter than 2*minChunk, or only one lane is available, the call runs `body` once
// inline on the calling thread with no threading overhead. `body` must not itself call
// parallelFor: the implementation gives each lane one sub-range and does not support
// nested parallelism.
void parallelFor(int begin, int end, int minChunk, const std::function<void(int lo, int hi)>& body);

}  // namespace pe
