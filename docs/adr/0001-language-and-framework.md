# ADR-0001 — Language & application framework: C++20 + Qt 6

**Status:** Accepted

## Context

PhotoEdit is a flagship-level, Photoshop-class editor, Windows-first, that must:

- do enormous amounts of per-pixel work on large buffers, on multiple cores and
  the GPU, with tight control over memory layout, SIMD, and lifetimes;
- present a complex desktop UI (dockable panels, menus, tablet input, dialogs);
- integrate a mature ecosystem of imaging libraries (color/ICC, PSD, raw, codecs);
- remain portable underneath even while shipping Windows first.

The language and UI framework are the most expensive decisions to reverse.

## Decision

Use **C++20** for everything, with **Qt 6 (Widgets)** as the application
framework, and keep the image engine in a **separate Qt-free core library**
(`pe_core`).

## Consequences

**Positive**
- Highest performance ceiling for the pixel engine; direct access to SIMD,
  threads, GPU APIs, and custom memory/tile management.
- Qt provides the proven desktop-editor infrastructure: dockable panels, tablet
  events, high-DPI, cross-platform windowing.
- Validated by **Krita**, an open-source Photoshop competitor built on C++/Qt with
  a tile engine and lcms color management — strong evidence the stack carries this
  class of app.
- Rich C++ ecosystem for imaging (lcms2, libpng/jpeg/tiff/webp, LibRaw, HarfBuzz).

**Negative / costs**
- C++ is the most demanding option: manual care around memory safety, longer
  build times, more boilerplate. Mitigated by strict standards, sanitizers,
  `-Werror` core lane, and heavy unit testing.
- Qt is a large dependency with licensing considerations (we use it within its
  open-source terms; revisit if distribution model changes).

## Alternatives considered

- **Rust + wgpu (+ Slint/egui).** Excellent memory safety and concurrency, and
  `wgpu` is a clean GPU abstraction. Rejected as primary because the desktop GUI
  ecosystem is less mature for a complex dockable editor, and the imaging library
  ecosystem (PSD, ICC, raw, text shaping) is thinner / requires more bridging.
  Kept in mind as the strongest alternative; the headless-core split means engine
  ideas remain portable.
- **C#/.NET + WinUI 3.** Fastest Windows UI development and great tooling, but a
  lower ceiling on pixel-engine throughput and a harder cross-platform story later
  (MAUI/Avalonia). Native interop for hot paths would reintroduce C++ anyway.
- **TypeScript + WebGPU (Electron).** Fastest path to a UI (the Photopea model)
  and trivially cross-platform, but real memory and performance ceilings for a
  true tile + scratch-disk engine on multi-gigabyte documents. Not "flagship
  engine" enough for the stated goal.

## Notes

The decision is reinforced by [ADR-0006](0006-headless-core-separation.md): Qt is
confined to `src/app`; the engine is Qt-free and unit-tested headless, which both
de-risks the Qt dependency and preserves a future port path.
