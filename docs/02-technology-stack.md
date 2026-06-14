# 02 — Technology Stack

This document records the stack and the reasoning. Where a choice is significant
it is also captured as an [ADR](adr/README.md).

## Summary

| Layer | Choice | Why |
| --- | --- | --- |
| Language | **C++20** | Performance, control over memory/SIMD, mature ecosystem for image tooling. |
| App framework | **Qt 6 (Widgets)** | The proven toolkit for flagship desktop editors (Krita); dockable panels, tablet input, cross-platform. |
| Engine | **`pe_core`, pure C++** | No UI dependency → testable, scriptable, portable. |
| GPU | **Abstraction over D3D12 / Vulkan** (Windows: D3D12 first) | Compositing, display, and heavy filters on the GPU. See [ADR-0002](adr/0002-gpu-abstraction.md). |
| Build | **CMake ≥ 3.24 + Ninja** | Standard, preset-driven, IDE-agnostic. |
| Packages | **vcpkg (manifest mode)** | Reproducible native deps (Qt, lcms2, libpng, …). |
| Color | **Little-CMS 2 (lcms2)** | Battle-tested ICC engine. See [ADR-0004](adr/0004-color-management.md). |
| Tests | **dependency-free harness now; doctest/Catch2 later** | Headless, runs in every CI lane. |

## Why C++ / Qt 6 (the primary decision)

A professional image editor is dominated by per-pixel work over very large
buffers, on the GPU and on multiple CPU cores, with tight control over memory
layout and lifetime. That is C++'s home turf, and it is what every serious
editor (Photoshop, Krita, Affinity) is written in.

**Krita** — the open-source Photoshop competitor — is C++/Qt, which de-risks the
choice enormously: it proves the toolkit can carry a dockable, tablet-driven,
tile-based, color-managed painting/editing application. We lean on the same
foundations (Qt Widgets for the shell, a tile engine, lcms2 for color).

Alternatives considered (full rationale in
[ADR-0001](adr/0001-language-and-framework.md)):

- **Rust + wgpu** — excellent safety and concurrency story; the GUI ecosystem is
  still less mature than Qt for a complex dockable desktop app, and the image
  tooling/library ecosystem (PSD, color, raw) is thinner. A strong second choice.
- **C#/.NET + WinUI 3** — fastest Windows UI development, but a lower ceiling on
  pixel-engine performance and a harder cross-platform path later.
- **TypeScript/WebGPU (Electron)** — fastest to a UI (this is the Photopea
  approach) but real memory/perf ceilings for a true tile + scratch-disk engine
  on huge documents.

We chose C++/Qt6 for the highest performance ceiling and the most proven path to
a flagship desktop editor, accepting that it is the most demanding to write.

### Qt usage rules

- Qt is allowed **only** in `src/app`. The engine (`src/core`) must never include
  or link Qt. This keeps the engine headless and testable and prevents UI
  concerns from leaking into image logic.
- We use **Qt Widgets** (not QML) for the main shell: it has the mature
  dockable-panel, menu, and tablet-event infrastructure a desktop editor needs.
- Qt types (`QImage`, `QColor`, `QPainter`) are used for UI display and simple
  I/O glue, **not** as the engine's pixel representation. Conversions happen at
  the boundary.

## C++ standard and toolchains

- **C++20**, no compiler extensions (`CMAKE_CXX_EXTENSIONS OFF`). We use concepts,
  `<span>`, designated initializers, `constexpr` math, and `<bit>` where helpful.
- Primary compiler: **MSVC 2022** (Windows). Also kept warning-clean on
  **GCC 13+** and **Clang 16+** (the Linux core lane builds with `-Werror`).
- We avoid bleeding-edge C++23 features until all three toolchains support them.

## GPU strategy (short version)

- The engine defines a small **render hardware interface (RHI)**: textures,
  buffers, command submission, and compute/shader dispatch. See
  [systems/23-gpu-acceleration.md](systems/23-gpu-acceleration.md).
- **Windows-first backend: Direct3D 12.** A **Vulkan** backend follows for
  portability. (Qt's own RHI is a fallback option if we choose not to own this.)
- The GPU path is for display compositing, zoom/pan, transform previews, and
  data-parallel filters. The CPU reference path always exists and defines
  correctness; the GPU must match it within tolerance.
- We never *require* a GPU: a software fallback keeps the editor usable on weak
  hardware, just slower.

## Dependencies (initial set, via vcpkg)

| Library | Role | Milestone |
| --- | --- | --- |
| Qt 6 (Core/Gui/Widgets) | App shell, windowing, tablet input | M0 |
| Little-CMS 2 (lcms2) | ICC color management | M6 |
| libpng, libjpeg-turbo, libtiff, libwebp | Raster import/export | M7 |
| zlib | Compression (PNG, PSD, native format) | M7 |
| (later) a raw library (e.g. LibRaw) | Camera Raw decode | M8+ |
| (later) HarfBuzz + FreeType, or Qt text | Typography shaping | M8 |

Dependencies are added **when the code that needs them lands**, not before, so CI
stays fast and the dependency graph reflects reality. The `vcpkg.json` manifest
lists the near-term set.

## Testing & quality tooling

- **Unit tests**: headless `pe_core` tests run in every CI lane (the dependency-
  free harness today; we may adopt doctest/Catch2 as suites grow).
- **Sanitizers**: ASan/UBSan builds of the core in CI on Linux.
- **Formatting/static analysis**: `clang-format` (enforced) and `clang-tidy`
  (advisory → enforced). See [05-build-and-tooling.md](05-build-and-tooling.md).
- **Golden-image tests**: compositor/filter outputs compared against committed
  reference images with a tolerance, added as those systems land.

## Platform policy

- **Primary**: Windows 10/11 x64, MSVC, D3D12.
- **Engine portability**: `pe_core` builds and is tested on Linux today; this
  guards portability continuously even though the shipping target is Windows.
- macOS/Linux app shells are a later effort; no decision here should foreclose
  them (hence the GPU abstraction and the Qt/engine split).
