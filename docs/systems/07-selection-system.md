# 07 — Selection System

> Milestone: M4 · Status: Spec

## Purpose

A **selection** is the document-wide answer to *"where am I allowed to edit?"*. It
is a single, temporary [mask](06-masks.md) carried on the [Document](01-document-system.md):
white where editing is permitted, black where it is locked out, and gray for
partial (anti-aliased or feathered) coverage. Visually it reads as the familiar
crawling **marching ants**, but that outline is only a UI rendering — the truth is
a grayscale alpha buffer, the `SelectionMask`.

The selection's job is to **gate every spatial edit**. A paint stroke, a fill, a
filter, a transform, a generative fill — each intersects its dirty region with the
selection so that work outside the selected area is masked away. This is the same
"where" primitive as a [mask](06-masks.md); the difference is scope and lifetime:
a mask belongs to one layer or effect and persists, while *the* selection is
document-wide, transient, and consulted by every tool. This document specifies the
selection buffer, the boolean algebra over it, the tools that produce it, the
operations that refine it, and how the engine enforces it during edits.

## Requirements (Functional + Non-functional)

**Functional**

- Represent the selection as a tiled grayscale coverage buffer over document space
  (8-bit at M4, 16-bit with [color management](15-color-management.md) at M6),
  defaulting to "nothing selected" where no tile is allocated.
- Treat a document with **no active selection** as "edit everywhere" — the absence
  of a selection must not block edits (the common case).
- Support **selection tools** that *produce or modify* the selection (each is a
  [Tool](09-tool-system.md) that commits a selection-changing command):
  rectangular/elliptical **marquee**, **lasso**, **polygonal lasso**, **magnetic
  lasso**, **magic wand** (contiguous/global color tolerance), **quick selection**
  (brush-driven region grow), **object selection** and **select subject**
  (segmentation), **color range**, **focus area**, **path→selection**, and
  **channel→selection**.
- Support **combine modes** on every tool: **replace**, **add** (union),
  **subtract** (difference), **intersect**, applied between the existing selection
  and the tool's new region.
- Support **operations** on the current selection: **invert** (select inverse),
  **select all** / **deselect** / **reselect**, **feather** (Gaussian edge
  softening), **smooth**, **expand/grow**, **contract**, **border**, **transform
  selection** (move/scale/rotate the mask without touching pixels), **save
  selection** (to an alpha [channel](19-channels.md)), and **load selection**.
- Carry **anti-aliased and feathered partial coverage** end to end: a 50%-covered
  pixel receives 50% of an edit's effect.
- Every selection mutation is a reversible [Command](21-history-undo.md) using the
  same tile-delta mechanism as pixel and mask edits.
- Expose a stable **boundary contour** (cached) for the marching-ants overlay so
  the app shell never re-derives it from scratch each frame.

**Non-functional**

- `pe_core`, pure C++20: no Qt. Marching-ants animation, the live drag preview,
  and tool option bars live in the [app shell](24-ui-workspace.md).
- Selection storage and ops are tile-sparse and dirty-region driven: feather,
  grow, and boolean ops touch only the affected tiles plus a wide-kernel apron.
- A selection that covers the whole canvas, or none of it, costs (near) zero in the
  edit-gating path — both collapse to "no clipping".
- CPU reference defines correctness; SIMD/GPU selection multiply and morphology
  must match within the golden-image tolerance.

## Data model (concrete C++ in `namespace pe`)

The selection reuses the engine's tile machinery (`kTileSize`, `Rect`, `TileCoord`
from `pe/core/Tile.hpp`) and the same sparse grayscale buffer the
[mask system](06-masks.md) defines (`MaskBuffer`/`MaskTile`). A selection *is* a
grayscale coverage buffer with a document-wide role and a cached outline.

```cpp
namespace pe {

// How a tool's new region combines with the existing selection.
enum class SelectionOp : uint8_t {
    Replace = 0,   // new region becomes the selection
    Add,           // union:        out = max(cur, src)        (or cur + src clamped)
    Subtract,      // difference:   out = cur * (1 - src)
    Intersect,     // intersection: out = min(cur, src)
};

// A polyline boundary derived from the selection's iso-coverage edge (e.g. the
// 50% contour). Cached so the marching-ants overlay is cheap to draw; rebuilt
// only when the mask changes. Pure geometry — the shell animates the dashes.
struct AntsOutline {
    std::vector<std::vector<Point>> loops;  // closed contours in document space
    Rect bounds;                            // bounding box of all loops
    uint64_t revision = 0;                  // bumped on every selection edit
};

// The document-wide selection: a grayscale coverage buffer plus a cached outline.
// "Empty" (no allocated tiles) means *nothing selected* — note this is the
// opposite default from a layer Mask, which reads as fully-revealing when absent.
class SelectionMask {
public:
    // Coverage at p in [0,1]; 0.0 where no tile is allocated (unselected).
    [[nodiscard]] float coverage(Point p) const noexcept;
    [[nodiscard]] bool  hasTileAt(TileCoord c) const noexcept;

    [[nodiscard]] bool  isEmpty() const noexcept;        // nothing selected
    [[nodiscard]] bool  selectsEverything() const noexcept; // fully opaque, no AA edge
    [[nodiscard]] Rect  contentBounds() const noexcept;  // union of non-zero tiles

    [[nodiscard]] const MaskBuffer&  buffer() const noexcept;
    [[nodiscard]] const AntsOutline& outline() const noexcept; // lazily rebuilt

private:
    MaskBuffer  coverage_;        // sparse, COW, paged — shared with mask storage
    AntsOutline outline_;         // cached boundary contour
    bool        outlineDirty_ = true;
};

// The Selection lives on the Document (Document::selection()). It owns the mask
// and the boolean algebra; it never mutates itself except through commands.
class Selection {
public:
    [[nodiscard]] const SelectionMask& mask() const noexcept;
    [[nodiscard]] bool active() const noexcept;   // false => edit everywhere

    // Pure-function combiners used by selection commands (not public mutators):
    // produce a new mask = combine(current, region, op). 'region' is itself a
    // grayscale coverage buffer (anti-aliased), so partial edges compose.
    static MaskBuffer combine(const MaskBuffer& current,
                              const MaskBuffer& region, SelectionOp op);
};

} // namespace pe
```

The `combine` formulas are the boolean algebra of fuzzy coverage: union is `max`
(or clamped add), intersection is `min`, subtraction is `cur·(1−src)`, replace is
`src`. Because operands are anti-aliased coverage in `[0,1]`, soft edges survive
boolean ops without aliasing.

## Behavior & algorithms

**Edit gating — the core contract.** Every spatial command consults the active
selection and intersects with it. Concretely, a [PaintCommand](08-brush-engine.md),
fill, filter, or transform that wants to write coverage `srcCov` at a pixel
multiplies it by the selection coverage there:

```
writeGated(layerTile, edit, region, selection):
    if not selection.active(): write edit over region (no gating)  # fast path
    else:
        for pixel p in region ∩ selection.contentBounds():
            s = selection.mask().coverage(p)         # [0,1]
            if s == 0: continue                      # locked out, skip
            applyEditAt(layerTile, p, edit, weight = s)   # partial at soft edges
```

This single rule gives anti-aliased, feathered editing for free: a brush dab over a
feathered selection edge fades exactly as the selection fades, and a filter applied
"inside the selection" blends out across the feather band. Tools also pre-clip
their **dirty region** to `selection.contentBounds()` so untouched tiles are never
even loaded.

**Selection tools build a region, then combine.** Each tool produces a candidate
region (a coverage buffer) and a `SelectionOp` from its modifier state, then issues
a `ModifySelectionCommand{op, region}`:

- **Marquee** — rasterizes a rectangle/ellipse to anti-aliased coverage; live drag
  shows a preview outline (shell), commit on release.
- **Lasso / polygonal lasso** — accumulate a freehand or vertex polygon; rasterize
  with the even-odd or non-zero rule; anti-alias the boundary.
- **Magnetic lasso** — snaps the polyline to the nearest high-contrast edge between
  clicks (cost = gradient magnitude); the path is then rasterized like a lasso.
- **Magic wand** — flood-fills from the clicked pixel over the composited (or
  active-layer) color within a **tolerance**, optionally contiguous; the filled
  region becomes the candidate (anti-aliased at the threshold boundary).
- **Quick selection** — a brush whose dabs grow the region into locally similar
  pixels (graph-cut / region-grow seeded by the stroke); paints the selection like
  a mask.
- **Object selection / select subject / focus area** — segmentation produces a
  coverage map (auto/AI path at M9; bounded box or whole-image subject); the result
  is ordinary selection coverage.
- **Color range** — selects by similarity to sampled colors with a fuzziness
  ramp, producing graded (not binary) coverage — naturally feathered.
- **Path→selection** — rasterizes a [vector path](18-vector-paths.md) to coverage
  (with a feather option). **Channel→selection** copies a saved
  [channel](19-channels.md)'s grayscale straight in.

**Refinement operations** rewrite the mask through commands:

- **Invert** — `out = 1 − coverage` over the canvas (selecting the inverse,
  bounded to the document; off-canvas stays unselected unless the document grows).
- **Feather(radius)** — Gaussian blur of the coverage edge with a neighbor-tile
  apron of `ceil(3·sigma)` so the softening is seamless across the 256-px tile
  seams (the wide-kernel tile handling from [ADR-0003](../adr/0003-tile-based-engine.md)).
- **Smooth(radius)** — median/morphological cleanup that removes stray speckles and
  rounds jagged edges.
- **Expand / Contract(n)** — binary morphological **dilate**/**erode** by `n`
  pixels on the thresholded mask (then re-anti-aliased), i.e. grow/shrink.
- **Border(n)** — `dilate(n) − erode(n)`, yielding a band selection around the
  current edge.
- **Transform selection** — applies an affine [transform](10-transform-system.md)
  to the mask only (resampled), so the marquee can be moved/scaled/rotated without
  disturbing pixels; previews live, commits as a command.

**Save / load selection.** "Save selection" writes the `SelectionMask` coverage
into a named alpha [channel](19-channels.md) (a `SelectionSave` channel); "load
selection" copies a channel back and may combine it with the current selection via
a `SelectionOp`. Both are lossless — selection, mask, and saved channel are the
same grayscale tile format, so the whole `mask ↔ selection ↔ channel` triangle is
a copy, not a conversion.

**Marching-ants derivation.** The outline is a UI concern: the engine exposes
`SelectionMask::outline()` (the iso-coverage contour as polylines, rebuilt only
when the mask's revision changes), and the [app shell](24-ui-workspace.md) animates
the dash phase on top of it. The engine never renders the ants; it only supplies
the geometry and a revision counter so the shell knows when to refetch.

## Interactions

- **[Masks](06-masks.md)** — the selection is the document-wide sibling of the
  per-layer mask and shares the `MaskBuffer`/`MaskTile` storage and the coverage
  algebra. "Make layer mask from selection" copies the `SelectionMask` into a layer
  `Mask`; "load mask as selection" reverses it; the **quick mask** workflow is
  literally painting the selection as a mask.
- **[Document](01-document-system.md)** — the active `Selection` lives on the
  document; a `DocumentChange{Selection}` notifies panels and the canvas overlay.
- **[Brush engine](08-brush-engine.md) / [tool system](09-tool-system.md)** —
  painting and every tool consult the selection to gate writes; selection tools are
  themselves `Tool`s that emit selection commands.
- **[Filter engine](12-filter-engine.md)** — a filter runs only inside the
  selection: its dirty region is intersected with the selection, and its result is
  blended by the selection coverage at the edge band.
- **[Transform](10-transform-system.md)** — "transform selection" reuses the
  transform/resample machinery on the mask alone.
- **[Channels](19-channels.md)** — save/load selection moves coverage to/from alpha
  channels; channel→selection seeds a selection from any channel.
- **[Vector & paths](18-vector-paths.md)** — path↔selection rasterizes a path to
  coverage and (via channels) recovers a path from a selection edge later.
- **[Adjustment layers](05-adjustment-layers.md)** — a selection active when an
  adjustment layer is created seeds the adjustment's own mask (Photoshop behavior).
- **[Command/history](21-history-undo.md)** — every selection change is a
  tile-delta command; deselect/reselect are themselves undoable steps.
- **UI touchpoints (app shell)** — marching-ants animation, the live tool drag
  preview, the Select menu, the "Select and Mask" workspace, and tool option bars
  (tolerance, feather, combine-mode buttons) are Qt UI; the engine exposes the
  buffer, the boolean ops, and the outline geometry only.

## Performance, threading & GPU

- Selection tiles are stored, reference-counted, paged, and threaded exactly like
  layer and mask tiles ([performance](22-performance.md)); a selection costs memory
  only where it deviates from fully-unselected.
- The edit-gating multiply is a per-tile scalar op — SIMD on CPU, a one-line shader
  on the [GPU/RHI](23-gpu-acceleration.md) path — folded into the same tile job
  that performs the edit, so gating adds no extra pass.
- Morphology (grow/contract/border/smooth) and feather are separable, tile-parallel
  passes with neighbor aprons; they run on the worker pool partitioned by tile.
- Magic wand / quick-selection flood and region-grow are bounded by the working set
  and run off the document thread; results merge back as a command.
- Outline extraction runs once per selection edit (not per frame) and is cached;
  the shell redraws ants by animating dash phase over the cached polylines.

## Edge cases & failure modes

- **No active selection** — gating is bypassed (`active()==false`); edits apply
  everywhere. This is the default and must be the fast path.
- **Empty selection after an op** — subtracting everything, or a wand with zero
  matches, yields an empty mask; the engine reports "nothing selected" and edits are
  effectively no-ops inside it (UI may warn "no pixels selected").
- **Select-all vs. no-selection** — semantically both edit everywhere, but they
  differ for ops like invert (invert of all = none); the model tracks `active()`
  distinctly from coverage so invert behaves correctly.
- **Feather radius ≥ region size** — a small selection feathered hard may drop below
  50% everywhere (no ants contour); coverage is still valid and edits fade — never
  an error.
- **Selection outside the canvas / after crop** — selection is document-space and
  sparse; cropping intersects it with the new bounds. Off-canvas coverage is
  retained until the canvas shrinks past it.
- **Anti-aliased boundary precision** — boolean ops on soft edges use float
  coverage to avoid dark/light seams where add/subtract regions meet.
- **Transform-selection resampling** — rotating a hard-edged selection introduces a
  soft edge (resample); this is expected and matches pixel transforms.
- **Hidden/locked target layer** — the selection still exists, but a gated edit on a
  locked layer is rejected upstream by the layer, not by the selection.

## Testing strategy

Headless `pe_core` unit + golden tests:

- **Boolean algebra** — `replace/add/subtract/intersect` produce the expected
  coverage on overlapping hard and soft regions; `combine` is commutative for
  add/intersect and correct (non-commutative) for subtract.
- **Gating** — a paint/fill/filter restricted by a selection writes only inside it;
  a 50% feathered edge yields 50% application (pixel-probe asserts).
- **Morphology** — grow/contract by `n` change the thresholded area as expected;
  `border(n) == dilate(n) − erode(n)`; smooth removes single-pixel speckles.
- **Feather across tiles** — feathering across a 256-px seam is seamless (no halo
  discontinuity), matching a golden render.
- **Round-trips** — `selection → channel → selection` and
  `selection ↔ layer mask` are byte-exact at equal precision.
- **Invert / select-all semantics** — `invert(all) == none`,
  `invert(none) == all`, `invert(invert(s)) == s`.
- **Outline** — the cached `AntsOutline` revision bumps on every edit and matches
  the iso-coverage boundary of a known mask.
- **Undo** — every selection command restores the prior mask (and outline cache)
  exactly.

## Phasing

- **M4 (this doc lands)** — 8-bit selection mask; marquee, lasso, polygonal/magnetic
  lasso, magic wand, quick selection; replace/add/subtract/intersect; invert,
  feather, smooth, grow, contract, border, transform selection; save/load to
  channel; edit-gating for paint/fill; marching-ants outline geometry; tile-delta
  undo. (Filter gating arrives with the filter engine in M5.)
- **M5** — selection gates the [filter engine](12-filter-engine.md) and adjustment
  previews; "Select and Mask" refine workspace shares the mask refine core.
- **M6** — 16-bit selection coverage riding in with [color management](15-color-management.md)
  and the [channels](19-channels.md) panel.
- **M8** — path↔selection matures with [vector & paths](18-vector-paths.md).
- **M9** — object selection, select subject, focus area, and color-range upgrades
  via the segmentation/[AI](14-generative-ai.md) path.

## Open questions

- Should `active()` (a live selection) and "select all" share storage, or should
  select-all be represented implicitly (a flag) to avoid allocating a full-canvas
  opaque mask?
- Default feather/anti-alias behavior per tool — global preference, per-tool option,
  or remembered last-used? Photoshop uses per-tool option-bar state.
- Do we cache one outline (50% contour) or expose multiple iso-contours for richer
  overlays (e.g. feather visualization)?
- Quick-selection and magic-wand source: sample the composited result, the active
  layer, or a user choice (sampled "all layers")? Leaning toward a tool option.

## References

- [06 — Masks](06-masks.md) — shared grayscale buffer, mask↔selection, quick mask.
- [08 — Brush engine](08-brush-engine.md) · [09 — Tool system](09-tool-system.md) —
  selection tools and gated painting.
- [12 — Filter engine](12-filter-engine.md) — selection-scoped filters.
- [10 — Transform system](10-transform-system.md) — transform selection.
- [19 — Channels](19-channels.md) — save/load selection, channel↔selection.
- [18 — Vector & paths](18-vector-paths.md) — path↔selection.
- [01 — Document system](01-document-system.md) — the active selection on the
  document.
- [ADR-0003 — Tile-based engine](../adr/0003-tile-based-engine.md) ·
  [ADR-0005 — Command/history model](../adr/0005-command-history-model.md).
- [Master architecture](../01-master-architecture.md) · [Glossary](../glossary.md)
  (Selection, Mask, Channel, Non-destructive).
