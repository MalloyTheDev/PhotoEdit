# Implementation Status

A living snapshot of what is actually built in the codebase, versus the
[roadmap](03-roadmap-and-milestones.md) and [system specs](systems/). The specs
describe the **intended** design; this file records the **current** reality.

> Convention: ✅ implemented & tested · 🟡 partially implemented · ⬜ not started.
> "Engine core" means the headless `pe_core` library (no Qt); "App" means the Qt6
> shell. The engine is built bottom-up first, so engine pieces land ahead of their UI.

Last updated against `main` after the **M5–M7 engine completion** (adjustments &
filters, the full lcms2 color-management pipeline, and the file-format suite) and
the **start of the application pivot** (a working Open / New / Save + canvas). Test
suite: **277 headless cases, 0 failed**, built `-Werror`, run under ASan/UBSan,
clang-format clean, on every merge — **Linux core + Windows MSVC/Qt6** CI, with the
optional native libraries (lcms2, libpng, libjpeg-turbo, libtiff, libwebp, zlib)
provisioned and exercised on **both** lanes. The whole engine has had a five-part
parallel correctness/security audit.

## Milestones

| Milestone | Engine core | App / GPU | Notes |
|-----------|:-----------:|:---------:|-------|
| **M0** Foundations | ✅ | ✅ | Build system, vcpkg, CI, Qt6 shell, full docs. |
| **M1** Document & layers | ✅ | 🟡 | Document, layer tree (pixel/group/solid/adjustment), CoW tiled storage, compositor (all separable blend modes), commands/undo. Layers panel UI pending. |
| **M2** Canvas & view | 🟡 | 🟡 | Engine `ViewTransform` + `CanvasRenderer` (dirty-tile cache); the app now shows the flattened composite on a `CanvasView`. Zoom/scroll viewport and the **RHI / Direct3D 12** GPU path are not started. |
| **M3** Painting & history | 🟡 | ⬜ | Brush engine (tile-delta paint commands) and the history/undo stack are implemented and tested; the interactive paint tool and the rest of the tool framework are pending. |
| **M4** Selections & masks | ✅ | 🟡 | Selection (marquee/coverage, boolean ops, feather) and masks (layer/clipping, density, invert) implemented and honoured by the compositor; saved-selection ↔ alpha-channel round-trip done. Selection-tool UI / marching ants pending. |
| **M5** Adjustments & filters | ✅ | ⬜ | **Complete in the engine** (see below). Adjustment-layer dialogs / filter-gallery UI pending. |
| **M6** Color management | ✅ | ⬜ | **Complete in the engine** (see below): lcms2/ICC profiles, working spaces, transforms (4 intents + BPC), a thread-safe transform cache, document assign/convert, display conversion, soft-proofing + gamut warning, and the channels system, on the 8/16/32-float pixel pipeline. Color-settings UI pending. |
| **M7** File formats | ✅ | 🟡 | **Engine complete** (see below): PNG, JPEG, TIFF, WebP, and the native layered **`.pedoc`** format, all hardened against untrusted input. The app's **Open / New / Save / Save As** are wired through `DocumentIO`. |
| **M8**–**M10** | ⬜ | ⬜ | Not started (type/vector/smart objects, retouching/AI, automation/print/plugins). |

## App — the interactive shell (pivot started)

The engine is no longer headless-only; the Qt6 app provides a real
**open → see it on a canvas → save** loop:

- **`DocumentIO`** (engine, tested) — `formatFromExtension`, `importDocument`/
  `exportDocument` dispatching to every codec (each guarded so a missing codec
  degrades gracefully), and `loadDocument`/`saveDocument` path helpers with a 512 MB
  read cap on untrusted files.
- **`MainWindow`** — File ▸ New (blank 800×600), Open…, Save, Save As… wired to
  `DocumentIO`; window title and canvas track the active document.
- **`CanvasView`** — paints `Document::compositeImage()` (→ `QImage` RGBA8888).

Next app slice: the interactive paint tool (mouse → `PaintCommand`).

## M5 detail — adjustments, filters, analysis (engine complete)

**Adjustment operators** (`Adjustment.hpp`, non-destructive, mask-aware, applied as
adjustment layers or baked destructively via the shared tile-delta machinery):
Brightness/Contrast, Levels, Curves, Invert, Exposure, Hue/Saturation, Channel
Mixer, Gradient Map, Vibrance, Color Balance, Black & White, Photo Filter,
Posterize, Threshold, Selective Color. (Deferred: Color Lookup `.cube` — asset
pipeline.)

**Filter engine** (`Filter.hpp`, reversible selection-gated commands): Gaussian/Box
blur (separable, premultiplied), Unsharp Mask, Mosaic, Median, Add Noise (seeded),
Find Edges (Sobel).

**Analysis:** `Histogram` (R/G/B/A + Rec.601 luma, statistics + percentiles) and
`AutoTone` (Auto Contrast / Auto Levels with clip trimming).

## M6 detail — color management (engine complete)

Built on **Little-CMS 2** (graceful optional dependency), on top of the 8/16/32-float
pixel pipeline:

- **`ColorProfile`** — ICC load/export plus five built-in working spaces (sRGB,
  linear sRGB, Display P3, Adobe RGB, ProPhoto).
- **`ColorTransform`** — RGB transforms with the four rendering intents and
  black-point compensation; **`ColorEngine`** caches them (thread-safe build-or-fetch).
- **Document operations** — assign vs. convert, working→display conversion, and
  soft-proofing with a configurable out-of-gamut alarm.
- **Channels** — split/merge and saved-selection ↔ alpha-channel round-trips.
- **High-bit-depth storage** — `Rgba16`/float tile stores, depth-aware layers,
  `compositeImage`/`16`/`F`, and destructive editing at native depth with exact undo.

Pending in color: 16-bit mask storage, CMYK/Lab/Gray document modes, depth-aware
brush dabs.

## M7 detail — file formats (engine complete)

All decoders cap dimensions (uint64, pre-allocation) and free native resources on
every path; verified against truncation / garbage / oversized / extreme-aspect input.

- **PNG** (libpng simplified API), **JPEG** (libjpeg-turbo / TurboJPEG), **TIFF**
  (libtiff, in-memory client + magic-validated), **WebP** (libwebp, lossless).
- **Native `.pedoc`** (v4) — a self-contained, bounds-checked binary format that
  preserves the layer **tree**: canvas metadata, recursive groups, per-layer
  properties, nested active layer, **layer masks**, and **zlib-compressed** pixel
  blocks. The reader is fuzz-tested against every prefix truncation.
- **Document I/O** — `importDocument`/`exportDocument` + path-level
  `loadDocument`/`saveDocument` (see App above).

## Cross-cutting engineering invariants

Enforced by tests + a per-change correctness/security audit:

- **Float working space.** Blend/filter/adjustment math is `Rgbaf`; `clamp01` is the
  NaN sink on every channel write.
- **Copy-on-write tiles** (`shared_ptr<TileData>`) with dirty-region tracking;
  tile-delta undo.
- **DoS caps** on every whole-image / destructive / decode path (megapixel, radius,
  coordinate-magnitude, and file-size bounds) so untrusted input can't exhaust memory.
- **Graceful optional dependencies** — every external library is detected at
  configure time; absent libraries disable their feature, never break the build.
- **Determinism** where it matters (seeded noise).
- **Headless core / Qt app** one-way dependency
  ([ADR-0006](adr/0006-headless-core-separation.md)).
</content>
