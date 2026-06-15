# 03 — Roadmap & Milestones

The build order is **engine-up** and **demoable-at-every-step**. We never want
"everything half-built and broken," so each milestone is independently buildable,
testable, and shows visible progress. Earlier milestones deliberately establish
the substrate (document, tiles, compositor, commands) that everything later
depends on.

> Dependencies, not dates. Each milestone lists its goal, what ships, and the
> exit criteria that let the next one start. Sizes are rough relative effort.

> 📊 **Current progress** is tracked in [STATUS.md](STATUS.md). In short: the
> **engine core for M1, M3–M7 is implemented and tested** — M5 (adjustments +
> filters + analysis), **M6 (the full lcms2 color-management pipeline)**, and
> **M7 (PNG/JPEG/TIFF/WebP + the native `.pedoc` format)** are all complete
> headless. The **application pivot has started**: Open / New / Save and a canvas
> are wired through the engine. The **GPU path and M8+** are not yet started.

---

## M0 — Foundations ✅ (this step)

**Goal:** a professional skeleton everything else is built on.

Ships:
- Repo, build system (CMake + presets), vcpkg manifest, `.gitignore`.
- `pe_core` with foundational types: geometry, color, tiles, blend/composite
  math reference, pixel buffer — all unit-tested headless.
- Dependency-free test harness; CI (headless core lane + Windows app lane).
- Qt6 app shell: main window, menu bar, dockable panel placeholders, status bar.
- The complete documentation set (this `docs/` tree).

**Exit criteria:** core builds `-Werror` and tests pass in CI; app shell builds
on Windows; architecture documented. *(All met.)*

---

## M1 — Document & layers (the spine) — 🟡 engine ✅, UI pending

**Goal:** a real document with a layer tree that composites correctly to a flat
image (headless first).

Ships:
- `Document` model: canvas size, resolution, color mode, bit depth, profile ref,
  metadata, dirty state. ([systems/01](systems/01-document-system.md))
- Tiled pixel storage; reference-counted, copy-on-write tiles.
  ([systems/02](systems/02-canvas-rendering.md), [22](systems/22-performance.md))
- `Layer` contract + pixel layers, group layers, background layer.
  ([systems/03](systems/03-layer-system.md))
- The **compositor**: bottom-to-top blend over tiles, all separable blend modes,
  per-layer opacity/visibility. ([systems/04](systems/04-blend-modes.md))
- Layer opacity, blend mode, ordering, add/delete/duplicate as **commands**.
- Golden-image tests: known layer stacks → known composited output.

**Exit criteria:** build a multi-layer document in code, composite to a buffer,
and match golden images for every blend mode; all mutations undoable.

---

## M2 — Canvas & view (see it) — 🟡 engine + canvas display; GPU/viewport pending

**Goal:** the document is visible and navigable in the app, fast.

Ships:
- Canvas viewport widget: zoom, pan, rotate-view, checkerboard transparency,
  rulers/guides/grid, brush-cursor/color-pick readout.
  ([systems/02](systems/02-canvas-rendering.md), [24](systems/24-ui-workspace.md))
- **RHI** with a Direct3D 12 backend (+ software fallback) for display
  compositing and tile upload. ([systems/23](systems/23-gpu-acceleration.md))
- Dirty-region driven repaint; only changed tiles re-uploaded/redrawn.
- Layers panel reflecting the tree (reorder, visibility, opacity, blend mode).

**Exit criteria:** open/scroll/zoom a large multi-layer document smoothly;
edits in M1 reflect on-screen by recompositing only dirty tiles.

---

## M3 — Painting (make marks) + history — 🟡 brush+history engine ✅

**Goal:** paint naturally, with full undo.

Ships:
- **Tool framework**: `Tool` contract, tool manager, options, cursors/overlays.
  ([systems/09](systems/09-tool-system.md))
- **Brush engine**: tip shape, spacing, size, hardness, opacity, flow,
  smoothing/stabilization, tablet pressure/tilt, blend mode; dab stamping along
  stabilized paths. ([systems/08](systems/08-brush-engine.md))
- Eraser, pencil, eyedropper, paint bucket, gradient, hand, zoom, move.
- **History system**: command stack, tile-delta undo, snapshots, History panel.
  ([systems/21](systems/21-history-undo.md))
- Basic transform (move/scale/rotate of a layer) with resampling.
  ([systems/10](systems/10-transform-system.md))
- Presets scaffolding for brushes. ([systems/25](systems/25-presets-assets.md))

**Exit criteria:** paint pressure-sensitive strokes on a layer; undo/redo any
edit; tile-delta undo verified on large documents.

---

## M4 — Selections & masks (control where) — 🟡 engine ✅, UI pending

**Goal:** constrain edits and hide pixels non-destructively.

Ships:
- **Selection system**: marquee, lasso, polygonal/magnetic lasso, magic wand,
  quick selection; add/subtract/intersect/invert/feather/grow/contract; marching
  ants overlay; save/load to channel. ([systems/07](systems/07-selection-system.md))
- **Masks**: layer masks, vector masks, clipping masks, quick mask; paint masks,
  selection↔mask, feather/density. ([systems/06](systems/06-masks.md))
- Selection gates every painting/fill/filter operation.

**Exit criteria:** make/refine a selection, convert to a layer mask, paint
through it; masks honored by the compositor and undo.

---

## M5 — Adjustments & filters (change the look, non-destructively) — ✅ engine complete

**Goal:** non-destructive color/tonal edits and a real filter engine.

Ships:
- **Adjustment layers**: Brightness/Contrast, Levels, Curves, Exposure, Vibrance,
  Hue/Saturation, Color Balance, Black & White, Photo Filter, Channel Mixer,
  Gradient Map, Selective Color, LUT — live, masked, blended.
  ([systems/05](systems/05-adjustment-layers.md))
- **Filter engine**: blur/sharpen/noise/distort/stylize/render/pixelate families;
  preview vs full-quality; CPU now, GPU where it pays.
  ([systems/12](systems/12-filter-engine.md))
- Histogram/Info panels driven by the engine.

**Exit criteria:** stack adjustment layers above content and see live,
mask-aware results; apply destructive filters with preview + undo.

---

## M6 — Color management (get color right) — ✅ engine complete (lcms2)

**Goal:** correct color across depths, spaces, screen, and proof.

Ships:
- lcms2 integration; ICC profiles; RGB/Gray/Lab (CMYK groundwork).
  ([systems/15](systems/15-color-management.md))
- 16-bit and 32-bit-float pixel paths through storage, compositor, filters.
- Working-space compositing, display conversion, soft proofing, gamut warning,
  rendering intents, black-point compensation.
- **Channels** system surfaced (R/G/B/A, spot, saved selections).
  ([systems/19](systems/19-channels.md))

**Exit criteria:** documents carry and honor profiles; convert between spaces;
soft-proof a CMYK target; 16/32-bit editing verified against references.

---

## M7 — File formats (open & save real files) — ✅ engine complete; app Open/Save wired

**Goal:** interoperate with the world.

Ships:
- **Native document format**: full-fidelity round-trip of the model (layers,
  masks, channels, paths, smart objects, metadata, profile), versioned.
- **PSD/PSB** import/export (faithful on common structure).
- PNG, JPEG, TIFF, WebP, GIF, BMP import/export; export-for-web; quick export.
  ([systems/20](systems/20-file-io.md))
- Format-aware flatten/preserve/convert logic; metadata (EXIF/XMP) preservation.

**Exit criteria:** round-trip a layered document through the native format with
zero loss; import a real-world PSD and composite it correctly; export every
listed raster format correctly.

---

## M8 — Type, vector, smart objects (composition power)

**Goal:** editable text, vector shapes/paths, and embedded smart objects.

Ships:
- **Text/typography**: editable text layers, fonts, size/leading/tracking/kerning,
  paragraphs, warping, anti-aliasing, live rasterization.
  ([systems/17](systems/17-text-typography.md))
- **Vector/paths**: pen tool, Bezier paths, shape/fill layers, vector masks,
  path↔selection, stroke/fill. ([systems/18](systems/18-vector-paths.md))
- **Smart objects** + **smart filters**: embedded/linked sources, stored
  transform, re-editable non-destructive filters; full **transform** suite
  (skew/distort/perspective/warp). ([systems/11](systems/11-smart-objects.md),
  [12](systems/12-filter-engine.md), [10](systems/10-transform-system.md))
- Camera-Raw-as-smart-object groundwork. ([systems/16](systems/16-camera-raw.md))

**Exit criteria:** set editable type; draw and edit vector shapes; place a
smart object, transform it repeatedly without quality loss, and re-edit a smart
filter.

---

## M9 — Retouching & AI (fix & generate)

**Goal:** professional cleanup and generative editing.

Ships:
- **Retouching**: clone stamp, healing/spot-healing, patch, red-eye,
  dodge/burn/sponge, content-aware fill/remove.
  ([systems/13](systems/13-retouching.md))
- **Generative AI** behind a provider interface: generative fill/expand, generate
  image, generative upscale, harmonize; selection+prompt+context → masked
  generative layer; variations; provenance/content-credentials hooks; local or
  cloud execution. ([systems/14](systems/14-generative-ai.md))
- Full **Camera Raw** pipeline (demosaic→white balance→tone→lens→sharpen/NR) as a
  re-editable smart object. ([systems/16](systems/16-camera-raw.md))

**Exit criteria:** remove an object with content-aware/AI tooling onto a
non-destructive layer; run generative fill from a selection+prompt and pick a
variation; edit raw settings after placing.

---

## M10 — Pro & platform (productionize)

**Goal:** the systems that make it a professional product, not just an editor.

Ships:
- **Automation**: actions (record/play), batch, droplets, image processor,
  scripting host. ([systems/26](systems/26-automation.md))
- **Presets & assets** maturity: brushes, gradients, patterns, swatches, styles,
  shapes, LUTs, tool presets, workspaces, import/export/migration.
  ([systems/25](systems/25-presets-assets.md))
- **Printing/prepress**: CMYK conversion, soft proof, gamut, spot colors, print
  scaling, PDF export. ([systems/27](systems/27-printing-prepress.md))
- **Plugin/extension** API: tools, filters, formats, panels; versioned + sandboxed.
  ([systems/29](systems/29-plugin-extension.md))
- **Cloud/account** (optional layer): sign-in, cloud documents, asset libraries,
  AI service routing. ([systems/28](systems/28-cloud-account.md))

**Exit criteria:** record and batch-run an action across a folder; install a
sample plugin that adds a filter; export a print-ready, color-managed PDF.

---

## Cross-cutting tracks (continuous, not a single milestone)

These advance alongside the milestones rather than in one phase:

- **Performance** ([22](systems/22-performance.md)) — RAM budget and tile cache
  land in M2; scratch-disk paging matures through M5–M7; SIMD/multithread filters
  in M5+; profiling is continuous.
- **GPU** ([23](systems/23-gpu-acceleration.md)) — display path in M2; filter
  acceleration in M5+; AI acceleration in M9.
- **UI/workspace** ([24](systems/24-ui-workspace.md)) — grows every milestone;
  dockable workspace, contextual task bar, shortcuts.
- **Color correctness** ([15](systems/15-color-management.md)) — designed in from
  M1 (working space), fully realized in M6.

## Sequencing rationale

- **Document + compositor + commands come first (M1)** because every later
  feature is "a layer type," "a filter," "a tool," or "a command" — all defined
  against those contracts.
- **Visibility (M2) before painting (M3)** so we can *see* what we edit, and so
  the dirty-region/GPU path is proven before high-frequency brush edits stress it.
- **Selections/masks (M4) before adjustments/filters (M5)** because professional
  adjustments and filters are almost always *masked*.
- **Color management (M6) before file formats (M7)** so saved/loaded files carry
  correct, profile-aware color rather than being retrofitted later.
- **Pro/platform (M10) last** because automation, plugins, and batch are
  multipliers on a feature set that must exist first — and they fall out cheaply
  from the command architecture once it does.
