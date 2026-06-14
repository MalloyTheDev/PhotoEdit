# Implementation Status

A living snapshot of what is actually built in the codebase, versus the
[roadmap](03-roadmap-and-milestones.md) and [system specs](systems/). The specs
describe the **intended** design; this file records the **current** reality.

> Convention: ✅ implemented & tested · 🟡 partially implemented · ⬜ not started.
> "Engine core" means the headless `pe_core` library (no Qt); "App" means the Qt6
> shell. The engine is built bottom-up first, so engine pieces land ahead of their
> UI.

Last updated against `main` after the M5 engine completion and the M6 high-bit-
depth (8/16/32-float) pixel pipeline. Test suite: **208 headless cases, 0 failed**,
built `-Werror`, run under ASan/UBSan, clang-format clean, on every merge (Linux
core + Windows MSVC/Qt6 CI).

## Milestones

| Milestone | Engine core | App / GPU | Notes |
|-----------|:-----------:|:---------:|-------|
| **M0** Foundations | ✅ | ✅ | Build system, vcpkg, CI, Qt6 shell, full docs. |
| **M1** Document & layers | ✅ | 🟡 | Document, layer tree (pixel/group/solid/adjustment), CoW tiled storage, compositor (all separable blend modes), commands/undo. Layers panel UI pending. |
| **M2** Canvas & view | 🟡 | ⬜ | CPU `ViewTransform` + `CanvasRenderer` (dirty-tile cache) exist in the engine; the Qt viewport widget and the **RHI / Direct3D 12** GPU path are not started. |
| **M3** Painting & history | 🟡 | ⬜ | Brush engine (tile-delta paint commands) and the history/undo stack are implemented and tested; the full tool framework and remaining tools (eraser, bucket, gradient, …) are pending. |
| **M4** Selections & masks | ✅ | 🟡 | Selection (marquee/coverage, boolean ops, feather) and masks (layer/clipping, density, invert) implemented and honored by the compositor; selection-tool UI and marching-ants overlay pending. |
| **M5** Adjustments & filters | ✅ | ⬜ | **Complete in the engine** — see below. Adjustment-layer dialogs / filter-gallery UI pending. |
| **M6** Color management | 🟡 | ⬜ | **High-bit-depth pixel pipeline complete end-to-end** (see below): 8/16/32-float storage, rendering, flatten, and destructive editing with exact undo. sRGB⇄linear transfer functions in place. **lcms2/ICC** transforms, soft-proofing, channels, and depth-aware **brush dabs** are the remaining pieces. |
| **M7**–**M10** | ⬜ | ⬜ | Not started (file formats, type/vector/smart objects, retouching/AI, automation/print/plugins). |

## M5 detail — adjustments, filters, analysis (engine complete)

**Adjustment operators** (`Adjustment.hpp`, non-destructive, mask-aware, applied as
adjustment layers or baked destructively via the shared tile-delta machinery):

- Brightness/Contrast, Levels (1D-LUT), Curves (1D-LUT), Invert, Exposure,
  Hue/Saturation, Channel Mixer, Gradient Map, Vibrance, Color Balance,
  Black & White, Photo Filter, Posterize, Threshold, **Selective Color**.
- Deferred: **Color Lookup** (`.cube` 1D/3D LUT) — waits on the asset pipeline.

**Filter engine** (`Filter.hpp`, edge-clamped reference kernels + `Filter`
subclasses, runnable as reversible, selection-gated commands):

- Blur: Gaussian, Box (separable, premultiplied — no transparent-color bleed).
- Sharpen: Unsharp Mask. · Pixelate: Mosaic. · Noise: Median (reduce), Add Noise
  (deterministic, seeded). · Stylize: Find Edges (Sobel).

**Analysis:**

- `Histogram` — R/G/B/A + Rec.601 luma bins (uint64) with channel statistics
  (mean, stddev, median, min/max, mode) and percentile queries.
- `AutoTone` — Auto Contrast (luma-based) and Auto Levels (per-channel), with
  clip-fraction outlier trimming.

## M6 detail — high-bit-depth pixel pipeline (complete end-to-end)

8-bit, 16-bit, and 32-bit-float documents are carried at native precision through
every engine stage; the proven 8-bit path is byte-for-byte unchanged (each step
was audited for zero regression):

- **Pixel types & conversions** — `Rgba16` with exact `8↔16↔float` conversions
  (lossless 8→16→8); sRGB⇄linear transfer functions (`ColorSpace`).
- **Storage** — `TileData`/`TileStore` templatized on pixel type
  (`TileStoreT<Pixel>`), aliased so all callers are unchanged; `TileStore16`,
  `TileStoreF`.
- **Layers** — `PixelLayer` carries a `BitDepth` and the matching sparse store;
  `renderInto`/`clone`/`contentBounds` dispatch on depth.
- **Documents** — `createBlank` seeds the base layer at the chosen depth;
  `compositeImage` / `compositeImage16` / `compositeImageF` flatten at 8/16/32f.
- **Editing** — filters and adjustment-bakes edit at native depth with exact undo
  (`bakePixelEdit` + a depth-generic `PixelLayer`). Brush dabs are 8-bit for now
  and fail safe on high-depth layers (depth-aware brushing is pending).

What's **not** done in M6: lcms2/ICC profiles & transforms, display conversion,
soft-proofing, gamut warning, the channels system, and CMYK/Lab/Gray modes.

## Cross-cutting engineering invariants

These hold across the engine and are enforced by tests + per-change audits:

- **Float working space.** All blend/filter/adjustment math is `Rgbaf`;
  `clamp01` is the NaN sink on every channel write.
- **Copy-on-write tiles** (`shared_ptr<TileData>`) with dirty-region tracking;
  tile-delta undo.
- **DoS caps** on every whole-image/destructive path (megapixel and radius
  bounds) so untrusted dimensions can't exhaust memory.
- **Determinism** where it matters (seeded noise) for reproducibility and future
  tiling.
- **Headless core / Qt app** one-way dependency ([ADR-0006](adr/0006-headless-core-separation.md)).
