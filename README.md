# PhotoEdit

A flagship-level, **Photoshop-class image editor**. Windows first; other
platforms later. Built in deliberate, shippable steps rather than all-at-once.

> Status: **Step 1 — foundations.** The architecture is fully documented and a
> buildable skeleton (engine core + tests + Qt6 app shell + CI) is in place.
> No image-editing features are implemented yet; see the
> [roadmap](docs/03-roadmap-and-milestones.md).

## What this is

PhotoEdit is being built as a professional 2D image "operating system": a real
document engine, layer compositor, selection/mask system, brush engine, filter
engine, color-managed pipeline, file-format support, automation, a
performance/scratch-disk system, and AI generation tools. The full system design
lives in [`docs/`](docs/README.md) — start with the
[master architecture](docs/01-master-architecture.md).

## Architecture at a glance

The codebase is split so the entire image engine is testable without a GUI:

```
photoedit (Qt6 app shell)     src/app   ── depends on ──┐
                                                        ▼
pe_core   (headless engine)   src/core  ◄─── NO Qt, NO UI, unit-tested
```

- **`src/core` — `pe_core`**: pure C++20, zero UI/Qt dependencies. Document
  model, tiles, color, blend/composite math, and (over time) every engine
  system. Fully unit-tested headless.
- **`src/app` — `photoedit`**: the Qt6 desktop application. The *only* place Qt
  is allowed. Hosts the dockable-panel workspace and canvas viewport.
- **`tests/`**: dependency-free unit tests for `pe_core`.

See [docs/02-technology-stack.md](docs/02-technology-stack.md) for why C++/Qt6,
and [ADR-0001](docs/adr/0001-language-and-framework.md).

## Building

### Prerequisites

- CMake ≥ 3.24, Ninja
- A C++20 compiler (MSVC 2022 on Windows; GCC 13+/Clang 16+ elsewhere)
- Qt 6.5+ (only needed to build the app, not the engine core)
- vcpkg (for native image libraries as they come online)

### Windows (primary target — full app)

```powershell
# with VCPKG_ROOT set and Qt discoverable by CMake
cmake --preset windows-msvc
cmake --build --preset windows-msvc
ctest --preset windows-msvc
```

### Engine core only (any platform, no Qt)

This is the lane CI gates on and the fastest inner loop for engine work:

```bash
cmake --preset linux-core-dev
cmake --build build/linux-core-dev
ctest --preset linux-core-dev
```

## Repository layout

```
docs/            Full system design: architecture, per-system specs, ADRs, roadmap
src/core/        pe_core — headless C++ engine (no Qt)
src/app/         photoedit — Qt6 application shell
tests/           Unit tests for the engine core
cmake/           Build helpers
.github/         CI workflows
```

## Roadmap (short form)

The build order is engine-up, each milestone independently demoable:

1. **M0 Foundations** *(this step)* — repo, build, core types, CI, docs
2. **M1 Document & layers** — document model, tiled layer storage, compositor
3. **M2 Canvas & view** — tile renderer, zoom/pan, GPU display path
4. **M3 Painting** — brush engine, eraser, tools framework, history/undo
5. **M4 Selections & masks** — selection masks, layer masks, marching ants
6. **M5 Adjustments & filters** — non-destructive adjustment layers, filter engine
7. **M6 Color management** — ICC, 16/32-bit, working spaces, soft proofing
8. **M7 File formats** — native doc, PSD, PNG/JPEG/TIFF/WebP import/export
9. **M8 Type, vector, smart objects** — text, paths/shapes, smart objects/filters
10. **M9 Retouching & AI** — clone/heal, generative fill/expand
11. **M10 Pro & platform** — automation, presets, print/prepress, plugins, cloud

Full detail: [docs/03-roadmap-and-milestones.md](docs/03-roadmap-and-milestones.md).

## License

TBD.
