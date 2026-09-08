# Implementation Status

A living snapshot of what is actually built in the codebase, versus the
[roadmap](03-roadmap-and-milestones.md) and [system specs](systems/). The specs
describe the **intended** design; this file records the **current** reality.

> Convention: ✅ implemented & tested · 🟡 partially implemented · ⬜ not started.
> "Engine core" means the headless `pe_core` library (no Qt); "App" means the Qt6
> shell. The engine is built bottom-up first, so engine pieces land ahead of their UI.

Last updated after an audit-driven hardening pass on top of the Photoshop-style UI
(#61): a document-owned selection with an undoable `SetSelectionCommand`, a Marquee
selection tool with marching ants and Shift/Alt modifiers, a Select menu
(All/Deselect/Invert), an eyedropper that sets the paint colour, tablet-pressure
painting, Image-menu adjustment actions, golden compositor tests, and an enforced
clang-format CI gate plus a real ASan/UBSan CI step. Built `-Werror` clean on gcc and
clang (headless no-deps included), ASan/UBSan-clean, clang-format-clean.
(Removed from the prior WIP as premature/unsafe: a non-compiling PSD decoder, an
RHI/GPU skeleton, and a scratch-disk cache — to be done properly with tests later.)
Test suite: **659 engine cases + 114 shell cases, 0 failed**. The engine tests
(`pe_core_tests`) run in every lane. The shell tests (`pe_app_tests`, added with
[ADR-0008](adr/0008-app-shell-as-a-library.md)) link `pe_app` and run wherever the
app is built, and under ASan/UBSan on Linux; they pin the theme contrast ratios, the stylesheet token
substitution, the icon-resource linkage, the menu surface (no top-level menu may be
empty; one Window toggle per dock), the keyboard-shortcut set including collision
detection, the unsaved-changes guard, and that long operations (save, export, the Magic
Wand) run off the GUI thread. Save and Export serialize an immutable `Document::snapshot()`,
so the canvas stays live and painting continues through the write; the engine tests pin the
copy-on-write barrier that makes that safe, including a worker reading a snapshot while the
document is painted underneath it.

## Milestones

| Milestone | Engine core | App / GPU | Notes |
|-----------|:-----------:|:---------:|-------|
| **M0** Foundations | ✅ | ✅ | Build system, vcpkg, CI, Qt6 shell, full docs. |
| **M1** Document & layers | ✅ | 🟡 | Document, layer tree (pixel/group/solid/adjustment), CoW tiled storage, compositor (all separable blend modes), commands/undo. Layers panel UI pending. |
| **M2** Canvas & view | 🟡 | 🟡 | Engine `ViewTransform` + `CanvasRenderer` (dirty-tile cache); the app now shows the flattened composite on a `CanvasView`. Zoom/scroll viewport and the **RHI / Direct3D 12** GPU path are not started. |
| **M3** Painting & history | ✅ | 🟡 | Brush engine (tile-delta paint commands) and the history/undo stack are implemented and tested, with an incremental live stroke so per-sample cost does not grow with stroke length. The app has brush, eraser, clone, dodge/burn, blur/sharpen, spot heal, bucket, gradient, move, marquee, lasso, magic wand, crop, type, free transform and eyedropper, with tablet pressure and stabilization. Brush presets and the remaining dynamics are pending. |
| **M4** Selections & masks | ✅ | 🟡 | Engine complete (rect, ops, masks, gating). Basic Marquee tool + marching ants + modifiers (Shift/Alt) wired in UI. Select All/Deselect/Invert menu added. More tools pending. |
| **M5** Adjustments & filters | ✅ | 🟡 | **Complete in the engine** (see below). The app has adjustment layers with interactive editors for Curves, Levels, Photo Filter, Gradient Map, Channel Mixer and Selective Color, plus the filter dialogs. A unified filter gallery is pending. |
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
- **`CanvasView`** — paints `Document::compositeImage()` (→ `QImage` RGBA8888),
  observes the document (auto-refresh on commit/undo/redo/load), and routes mouse
  input to the brush tool.
- **`PaintToolController`** (engine, headless, tested) — the interactive
  brush/eraser: `begin`/`extend`/`end`/`cancel` turn pointer samples into a live
  preview and commit exactly one undoable `PaintCommand` per stroke, gated by the
  active selection. `MainWindow` wires Edit ▸ Undo/Redo (Ctrl+Z / Ctrl+Shift+Z).
- **Viewport** — `CanvasView` shows the composite through the engine
  `ViewTransform`: wheel-zoom about the cursor, middle-drag pan, fit-to-window /
  actual-pixels (Ctrl+0/1), and a transparency checkerboard. Painting maps the
  pointer back through `viewToDoc`, so it tracks the cursor under any zoom/pan.
- **Move tool options**: Auto-Select starts the drag on the layer under the cursor
  rather than on the active one, at either Layer or Group granularity; it rests on
  the engine's `layerAt()`, which walks the stack top-down and falls through anything
  hidden, cleared, masked out or adjustment. Show Transform Controls draws the active
  layer's box and handles, and grabbing a corner or the rotate knob enters Free
  Transform without Ctrl+T. Both default off. Group granularity selects the group,
  which the Move command then declines to move: relocating a group's pixels needs an
  engine command that does not exist yet.
- **Layers panel** — top-first stack with visibility / opacity / blend mode and
  add / duplicate / delete / reorder, all as undoable commands. Rows rename in place
  (double-click), delete asks before discarding a layer that holds content, every
  button carries a tooltip and an accessible name, and top-level rows can be dragged
  to reorder. A drag that would change nesting is refused out loud rather than
  ignored: `ReorderLayerCommand` takes a top-level index, so moving a layer into or
  out of a group needs an engine command that does not exist yet (#131). The **Layer
  menu** is a second door onto the same operations, so none of them depends on the
  dock being open: New Layer (Ctrl+Shift+N), Duplicate Layer (Ctrl+J), Delete Layer,
  and an Arrange submenu (Bring to Front / Forward / Backward / Send to Back, bound to
  Ctrl+Shift+] / Ctrl+] / Ctrl+[ / Ctrl+Shift+[). Reaching an end of the stack
  reports a refusal instead of doing nothing quietly, which is what the dock's two
  arrows used to do.
- **History panel** — a state timeline (uses `History::undoNames/redoNames`) with
  click-to-seek; the current state is highlighted, redoable states dimmed.

### Visual design system (dark pro theme)

The app has a committed visual identity: a Fusion-based dark theme applied as a
`QPalette` + generated QSS. The accent is a light hue used as a line, not a flood: it
carries keyboard focus, the active tool, the selected row and the active panel tab as
an outline or underline, because a lightness step alone is not resolvable at the
contrast the chrome runs at. **Three switchable variants** (View ▸ Theme), persisted
via `QSettings`: *Nocturne* (the flagship blue-grey ground, the default), *Graphite*
(medium neutral grey) and *Charcoal* (a darker neutral grey). Every theme is held to
WCAG 2.1: text and secondary text clear 4.5:1 on each surface, and the accent and the
control-outline role clear 3:1. The chrome is the scaffold the rest of the toolset
wires into: a left **tool strip** (Move / Marquee / Lasso / Wand / Crop / Eyedropper /
Brush / Eraser / Bucket / Type / Hand / Zoom) with bundled Lucide SVG icons
(`icons.qrc`, ISC, re-tinted via `IconUtil`) — Brush/Eraser/Hand/Zoom are wired, the
rest are scaffolded; custom panel headers (`PanelHeader`: glyph + Title-Case label),
per-layer thumbnails in the Layers panel, styled menus/lists/controls/scrollbars, and
a status bar with the active tool + live zoom %. `CanvasView::Tool` gates input per
the selected tool.

Next app slice: functional Move + selection tools (lasso/wand) routed through proper
undoable commands, adjustment/filter dialogs (the M5/M6 engine is ready), then the
GPU display path (M2) — each landed with tests, not as a stub.
(Recent: Marquee selection + marching ants + Select menu, eyedropper sets the paint
colour, tablet-pressure painting, Image-menu adjustment actions, golden compositor
tests, and an enforced clang-format CI gate + real ASan/UBSan CI step.)

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
- **Native `.pedoc`** — a self-contained, bounds-checked binary format that
  preserves the layer **tree**: canvas metadata, recursive groups, per-layer
  properties, nested active layer, **layer masks**, and **zlib-compressed** pixel
  blocks. The writer emits v6, or v7 when a layer holds content outside the canvas;
  the reader accepts v4 and up. Blocks are written at DEFLATE level 1 and stored raw
  when compression would not shrink them. The reader is fuzz-tested against every
  prefix truncation.
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
- **Threat model.** File bytes are untrusted; see the summary in
  [systems/20-file-io.md](systems/20-file-io.md#threat-model--untrusted-file-handling-summary).
  Atomic saves prevent partial-file corruption on crash/disk-full.
- **Graceful optional dependencies** — every external library is detected at
  configure time; absent libraries disable their feature, never break the build.
- **Determinism** where it matters (seeded noise).
- **Headless core / Qt app** one-way dependency
  ([ADR-0006](adr/0006-headless-core-separation.md)).
