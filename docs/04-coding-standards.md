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
  PNGs compared within tolerance).
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
