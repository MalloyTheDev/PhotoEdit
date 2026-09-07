# 04 — Coding Standards

These rules keep a large C++ codebase consistent, safe, and reviewable. They are
enforced by `clang-format`, compiler warnings (`-Werror` / `/WX` in the core
lane), and code review.

## Language & structure

- **C++20**, no compiler extensions. Prefer standard-library facilities over
  hand-rolled equivalents.
- **Namespaces**: everything in `pe`. Subsystems nest: `pe::core` concepts are in
  `pe` for short names today; UI lives in `pe::app`. Avoid `using namespace` in
  headers, ever.
- **Headers**: `#pragma once`. Include what you use. Public engine headers live
  under `src/core/include/pe/core/...` and must not include Qt or app headers.
- **One-way dependencies**: `app → core`. The core never depends on the app, on
  Qt, or on any UI/windowing/network library. CI's core lane enforces this by
  building the core with no Qt present.

## Naming

| Kind | Style | Example |
| --- | --- | --- |
| Types | `PascalCase` | `LayerStack`, `BlendMode` |
| Functions / methods | `camelCase` | `compositeOver`, `tilesForRect` |
| Variables | `camelCase` | `tileSize`, `dirtyRegion` |
| Member variables | trailing underscore | `width_`, `pixels_` |
| Constants / `constexpr` | `kPascalCase` | `kTileSize` |
| Macros | `PE_UPPER_SNAKE` (avoid macros otherwise) | `PE_CHECK` |
| Files | match the primary type | `BlendMode.hpp` / `BlendMode.cpp` |

## Types & memory

- Prefer **value types** and clear ownership. Use `std::unique_ptr` for owning
  pointers, raw pointers/references for non-owning access, `std::shared_ptr` only
  where shared ownership is genuinely required (e.g. shared immutable tiles).
- Use `std::span`, `std::string_view`, and references to pass through data without
  copying. Be explicit when a copy is intended.
- **No naked `new`/`delete`** outside of low-level allocators/pools.
- Integer types: use `int` for small in-bounds quantities (coordinates), and
  fixed-width types (`int64_t`, `uint8_t`) where size/representation matters
  (pixel components, file formats, buffer offsets). Pixel/byte counts that can
  exceed 2³¹ use 64-bit.
- Mark functions `noexcept` when they cannot throw, `[[nodiscard]]` when ignoring
  the result is a bug, and `constexpr` when feasible.

## Error handling

- **Programming errors** (precondition violations) are asserts; they indicate
  bugs to be fixed, not handled.
- **Expected, recoverable failures** (file not found, decode error, out of
  scratch space) are returned as values: `std::expected<T, Error>` (or a
  `Result<T>` alias) — not exceptions, in the engine's hot and I/O paths.
- Exceptions may cross the Qt/app boundary for truly exceptional UI flows, but the
  engine's public API does not throw for ordinary failures.
- Never silently swallow errors. Either handle, propagate, or log+degrade
  explicitly.

## Const-correctness & immutability

- `const` by default. A method that does not mutate is `const`.
- Favor immutable data for anything shared across threads (e.g. composited tiles
  handed to the renderer). Mutation happens through commands on the owning thread.

## Concurrency

- Shared mutable state is the exception, not the rule. The document is mutated on
  a single logical owner; heavy work (filters, compositing, I/O) is dispatched to
  worker threads operating on **tiles** that don't alias.
- Any cross-thread sharing must have an explicit, documented synchronization
  story. Prefer message/task passing over shared locks.
- **The engine is single threaded for READS as well as writes.** `TileStoreT`
  caches its content bounds lazily, `CanvasRenderer` owns a mutable LRU and one
  shared scratch buffer, and `Document::notify` dispatches observers on the calling
  thread, so two threads merely *reading* the same document race.
- **A second thread sees document state through `Document::snapshot()` and nothing
  else.** It shares tile buffers copy-on-write and costs pointer copies, not pixels
  (0.06 ms on a 24 MP document). Take it on the owning thread; never from a worker.
- **The copy-on-write fork trigger is a shared flag, not `use_count()`.** A refcount
  answers "how many owners exist at this instant", which another thread can
  invalidate between the test and the write. `TileStoreT::Entry::shared` is set when
  a buffer escapes, by the thread that owns the store, before any worker exists. If
  you add a way for a `shared_ptr<TileData>` to leave a store, it must set that flag.
- Moving work off the GUI thread means deciding what the GUI thread may still touch,
  not merely adding a worker. `pe::app::runDocumentTask` is the one place that does
  it, and its `TaskAccess` argument is the decision: `Snapshot` leaves the canvas and
  input fully live, `LiveDocument` blocks input and freezes the canvas,
  `Detached` blocks input but keeps painting. A new background operation goes through
  that helper rather than growing its own policy.
- A blocked window is a correctness problem, not a polish problem. Seconds of
  synchronous work on the GUI thread reads to the user as a crash, and on Windows the
  OS escalates it to the not-responding state; force-quitting there loses the
  document. Anything that can take longer than a frame belongs on a worker.

## Comments

- Explain **why**, not **what**. Assume the reader knows C++.
- Public engine APIs get a short doc comment: purpose, units, ownership,
  threading, and any invariant the caller must uphold.
- Keep comment density similar to the surrounding code; match the house style
  visible in `src/core`.

## Testing bar

- **Every engine system ships with headless unit tests.** No image-logic PR lands
  without tests for the new behavior.
- Math kernels (blend, color conversion, transforms, filters) are validated
  against a simple reference implementation and/or known values.
- Subsystems with visual output add **golden-image** tests (committed reference
  PNGs compared within tolerance). The harness is `tests/golden.hpp` and the
  references live in `tests/golden/`; regenerate with `PE_GOLDEN_UPDATE=1` and
  review the images before committing them. Pair every golden with a structural
  assertion, so a reference captured from a bug cannot silently become the spec.
- **Tiled execution must be tested across a tile boundary.** Storage is tiled at
  `kTileSize`, so a test on a canvas that fits in one tile validates the arithmetic
  and nothing else: not tile adjacency, halo sourcing, cache identity, eviction, or
  cross-tile spatial ownership. Any test intended to validate tiled semantics must
  force at least one operation to cross a tile boundary, and any cache test must
  exercise more than one distinct cache key. This is not hypothetical: forcing every
  tile lookup in `Brush.cpp` to resolve tile (0,0) once left a 512-case suite green,
  and a byte-identical parity test for the incremental blur passed on a single-tile
  canvas while both its halo source and its cache key were broken.
- **Report where an image changed, not only how much.** `tests/pixeldiff.hpp` gives
  the changed-pixel bounds; a bounded operation should assert they sit inside its
  mathematically permitted influence box. An implementation can be numerically
  plausible and still touch pixels it had no business touching.
- **A test must fail by reporting, never by crashing.** Guard the step after a failed
  assertion: indexing a container the assertion just proved empty aborts the binary,
  so every later test's result is lost and the suite reads as green. Found the hard
  way: a mutation that made refusals silent was detected by the right tests, and the
  run still reported zero failures because the first of them dereferenced an empty
  vector. A test that fails in the wrong way is indistinguishable from one that passes.
- Tests must be deterministic and not depend on a GPU, display, or network.

## Formatting

- `clang-format` (config in repo) is the single source of truth; do not hand-format
  against it. CI checks formatting.
- 4-space indent, 100-column soft limit (see `.clang-format`).

## Review checklist (what a reviewer looks for)

1. Does it keep `core` free of UI/Qt?
2. Is every mutation expressible/undoable as a command (where applicable)?
3. Are big-buffer operations tile-aware and dirty-region-correct?
4. Color space and bit depth handled explicitly?
5. Tests: reference-validated, deterministic, covering edge cases?
6. Ownership and threading clear?
