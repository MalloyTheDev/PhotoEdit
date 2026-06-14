# 06 — Masks

> Milestone: M4 · Status: Spec

## Purpose

A **mask** is a grayscale buffer that controls *where* something applies. White
means full, black means none, and every gray in between is a partial weight. For
a layer with a mask, the rule the compositor enforces is simply:

```
visible_coverage = layer_coverage * mask_value
```

Masks are the project's universal "where" control. They show up everywhere
because almost every feature needs to localize its effect without destroying
pixels: a [brush](08-brush-engine.md) paints into a mask, a
[selection](07-selection-system.md) is converted to a mask, an
[adjustment](05-adjustment-layers.md) or [filter](12-filter-engine.md) is
confined by a mask, and the [generative AI](14-generative-ai.md) system reads and
writes masks. Because they carry real editorial intent, masks are first-class,
[non-destructive](../glossary.md) document data that [file I/O](20-file-io.md)
must preserve.

This document specifies the mask data model, the mask *types* (layer, vector,
clipping, filter, quick), the editing operations, and exactly how the compositor
multiplies a mask into a layer's coverage. The closely related
[selection system](07-selection-system.md) is "a document-wide mask of where
edits are allowed"; this doc owns the per-layer/per-effect masks and the shared
grayscale buffer type they both build on.

## Requirements (Functional + Non-functional)

**Functional**

- Represent a mask as a tiled grayscale buffer at 8- or 16-bit precision,
  defaulting to "fully visible" where no tile is allocated.
- Support these mask kinds:
  - **Layer mask** — a raster mask attached to any [layer](03-layer-system.md).
  - **Vector mask** — a [path](18-vector-paths.md)-backed mask, rasterized to
    coverage; resolution-independent and re-editable as Bézier geometry.
  - **Clipping mask** — a layer whose own alpha confines the layers clipped to
    it (the base layer's coverage is the mask for the group above it).
  - **Filter mask** — the mask that scopes a smart filter on a
    [smart object](11-smart-objects.md) (delegated here for the buffer/ops).
  - **Quick mask** — a transient, full-document mask used to *paint a selection*;
    it round-trips to the [selection](07-selection-system.md) on exit.
- Derive a mask **from a selection** and **from a channel** (and the reverse,
  selection/channel from a mask), losslessly within precision.
- Support **object-aware masking** ("Mask All Objects") that produces one layer
  mask per detected object region (segmentation supplied by the AI/auto path;
  the result is ordinary mask data).
- Mask editing operations: **paint** (via the brush engine), **fill/clear**,
  **invert**, **feather** (Gaussian softening of the edge), **density** (a global
  ceiling on the mask's effect), **refine edge** (edge detection + smoothing +
  shift), and **apply** (bake the mask into the layer, destructive).
- A mask has independent state: **enabled/disabled**, **linked** to the layer's
  transform or not, and (for layer masks) a default **color↔gray meaning** of
  white = reveal.
- Every change to a mask is a reversible [Command](../glossary.md) using the same
  tile-delta mechanism as pixel edits.

**Non-functional**

- Masks honored by the compositor add cost proportional to the *dirty* tiles
  only, never the whole canvas.
- Mask buffers participate in the [performance](22-performance.md) substrate:
  reference-counted, copy-on-write tiles, RAM-budgeted, scratch-pageable.
- A disabled or absent mask costs nothing in the composite (no per-pixel work).
- CPU reference defines correctness; SIMD/GPU mask multiply must match within the
  golden-image tolerance.

## Data model (concrete C++ in `namespace pe`)

Masks reuse the engine's tile machinery (`kTileSize`, `Rect`, tile coords from
`pe/core/Tile.hpp`). A mask stores **single-channel coverage**, not RGBA.

```cpp
namespace pe {

// One 256x256 grayscale coverage tile. 8-bit today; 16-bit (Mask16) arrives
// with color management (M6). Absent tiles read as kOpaque (fully revealing),
// so an empty MaskBuffer means "no masking".
struct MaskTile {
    static constexpr uint8_t kOpaque = 255; // reveals
    static constexpr uint8_t kClear  = 0;   // hides
    std::array<uint8_t, kTileSize * kTileSize> coverage;
};

// Sparse, tiled, copy-on-write grayscale buffer. Shares the eviction/paging
// policy of layer pixel storage (see systems/22-performance.md).
class MaskBuffer {
public:
    [[nodiscard]] uint8_t sample(Point p) const noexcept;   // kOpaque if no tile
    [[nodiscard]] bool    hasTileAt(TileCoord c) const noexcept;
    [[nodiscard]] Rect    contentBounds() const noexcept;   // union of non-default tiles
    void writeTile(TileCoord c, const MaskTile&);           // via commands only
};

// A raster mask attached to a layer / filter / quick-mask session.
class Mask {
public:
    enum class Kind : uint8_t { Layer, Filter, Quick };

    [[nodiscard]] Kind  kind() const noexcept;
    [[nodiscard]] bool  enabled() const noexcept;   // disabled => ignored by compositor
    [[nodiscard]] bool  linked() const noexcept;    // moves with the layer transform
    [[nodiscard]] float density() const noexcept;   // [0,1] global ceiling on effect
    [[nodiscard]] float feather() const noexcept;   // Gaussian radius in px, live
    [[nodiscard]] const MaskBuffer& buffer() const noexcept;

    // Effective mask value at p in [0,1], after density and live feather:
    //   m = feathered(sample(p)/255) * density
    [[nodiscard]] float evaluate(Point p) const noexcept;
};

// A vector mask: the source of truth is geometry; coverage is rasterized on
// demand and cached as a MaskBuffer (regenerated when the path or transform
// changes). See systems/18-vector-paths.md.
class VectorMask {
public:
    [[nodiscard]] const Path&  path() const noexcept;
    [[nodiscard]] FillRule     fillRule() const noexcept;   // NonZero / EvenOdd
    [[nodiscard]] bool         enabled() const noexcept;
    // Rasterize the path's coverage for a tile region into a MaskBuffer cache.
    void rasterizeInto(TileRequest, MaskSink&) const;
};

} // namespace pe
```

A layer may carry **both** a raster `Mask` and a `VectorMask`; their effects
multiply (vector coverage × raster coverage), matching the Photoshop model. The
[Layer](03-layer-system.md) contract already exposes `const Mask* mask()`; the
vector mask hangs off the same layer slot.

## Behavior & algorithms

**Compositor integration.** The single canonical hook is in the per-tile loop
from the [master architecture](../01-master-architecture.md): after a layer
renders its pixels and effects, the compositor multiplies the layer's alpha by
the mask coverage *before* blending.

```
applyMask(src_tile, layer, region):           # src_tile is straight-alpha Rgbaf
    if no enabled mask on layer: return src_tile
    for each pixel p in region:
        m = 1.0
        if layer.vectorMask.enabled: m *= vectorCoverage(p)   # cached raster
        if layer.mask.enabled:       m *= layer.mask.evaluate(p)
        src_tile[p].a *= m            # scale coverage only; color is unchanged
    return src_tile
```

Because masks scale **alpha (coverage)** and never the color channels, a 50% gray
mask makes the layer 50% transparent at that pixel — it does not darken it. This
keeps masks orthogonal to blend mode and opacity (which the compositor applies
afterward).

**Clipping masks** are handled one level up, in group/stack traversal rather than
per layer: a run of layers clipped to a base layer is composited into a scratch
buffer, then that buffer's coverage is multiplied by the **base layer's rendered
alpha**, then the whole group is blended down. So a clipping mask is "use the
layer below me as my mask" expressed through stack structure, reusing the same
coverage-multiply kernel.

**Feather (live).** `feather` is a non-destructive Gaussian blur of the mask edge
evaluated at composite time. For performance the compositor feathers the *sampled
coverage* per dirty tile with an apron (halo) of `ceil(3·sigma)` pixels pulled
from neighboring mask tiles, so the blur is seamless across tile boundaries (the
standard wide-kernel tile handling from [ADR-0003](../adr/0003-tile-based-engine.md)).
A baked feather (destructive) is also offered for very large radii.

**Density** is a trivial post-multiply: `m_effective = m * density`. It lets a
black region become "80% hidden" without repainting the mask.

**Refine edge** runs as a command that, given the current mask: detects the soft
edge band, optionally classifies foreground/background (hair/fur cases use the
auto/AI path), then applies *smooth*, *feather*, *contrast*, and *shift edge*
adjustments, writing a new mask. It shares its core with the selection system's
"Select and Mask" workspace — both produce a refined grayscale coverage.

**Editing via the brush engine.** When the user paints on a mask, the active
[tool](09-tool-system.md) targets the mask buffer instead of a color layer; the
[brush engine](08-brush-engine.md) stamps grayscale dabs (painting with white
reveals, black conceals), producing a `PaintMaskCommand` that records tile
deltas. Fill, gradient, and bucket tools likewise write grayscale.

**Selection ↔ mask.** "Add layer mask from selection" copies the
[selection](07-selection-system.md) `SelectionMask` coverage straight into a new
layer `Mask` (white where selected). "Load mask as selection" does the reverse.
Both are lossless because both are the same grayscale tile format.

## Interactions

- **[Layer system](03-layer-system.md)** — a `Mask`/`VectorMask` is owned by a
  `Layer`; the compositor's `applyMask` step sits between layer rendering and
  blending. Clipping masks are expressed via stack structure.
- **[Compositor](02-canvas-rendering.md) / [blend modes](04-blend-modes.md)** —
  masks scale coverage *before* blend/opacity; disabled/absent masks are skipped.
- **[Selection system](07-selection-system.md)** — selections are the
  document-wide sibling; quick mask and selection↔mask conversions bridge them on
  the shared grayscale buffer.
- **[Brush engine](08-brush-engine.md)** — paints grayscale into a mask buffer;
  same dab/stabilization path as color painting.
- **[Vector & paths](18-vector-paths.md)** — backs vector masks; the path is the
  source, the `MaskBuffer` is a regenerated cache.
- **[Adjustment layers](05-adjustment-layers.md) & [filters](12-filter-engine.md)** —
  every adjustment layer carries its own `Mask`; smart filters carry a filter
  mask. Both use this buffer and the `evaluate`/`applyMask` machinery.
- **[Channels](19-channels.md)** — a mask is channel-like grayscale data; masks
  convert to/from alpha/saved-selection channels.
- **[File I/O](20-file-io.md)** — layer masks, vector masks, density, feather,
  and enabled state are persisted in the native format and mapped to PSD where
  the structure exists.
- **[Command/history](21-history-undo.md)** — every mask mutation is a
  tile-delta command.
- **UI touchpoints (app shell)** — the mask thumbnail in the Layers panel,
  Masks/Properties panel (density/feather sliders, invert, refine), and the
  Alt-click "view mask" overlay are Qt UI; the engine only exposes the data and
  commands.

## Performance, threading & GPU

- Mask tiles are stored, reference-counted, paged, and threaded exactly like
  layer pixel tiles ([performance](22-performance.md)); a mask costs memory only
  where it deviates from fully-revealing.
- The coverage multiply is a per-tile, per-pixel scalar op — trivially SIMD on
  CPU and a one-line shader on the [GPU/RHI](23-gpu-acceleration.md) path; it runs
  in the same tile job that composites the layer, partitioned so workers never
  alias.
- Live feather needs neighbor-tile aprons; the compositor expands the read region
  by the feather halo. Large radii should be baked to avoid re-blurring every
  frame.
- Vector-mask rasterization is cached per tile and invalidated only when the path
  or layer transform changes (dirty-region driven).

## Edge cases & failure modes

- **No mask / disabled mask** — `applyMask` returns the source unchanged; zero
  cost. This is the default and the common case.
- **Mask vs. layer bounds mismatch** — masks are document-space and sparse;
  regions with no mask tile read `kOpaque` (reveal), so a small mask does not clip
  a large layer unless explicitly painted.
- **Linked vs. unlinked transform** — a linked mask transforms with the layer; an
  unlinked mask stays put while the layer moves (the engine tracks two transforms;
  the compositor samples the mask in its own space).
- **Density 0 with content** — layer fully hidden but still present and editable;
  must not be confused with `visible() == false`.
- **Vector + raster mask both present** — coverage multiplies; either being
  disabled drops it from the product.
- **Apply mask (bake)** — destructive: composites coverage into the layer's alpha
  and discards the mask; recorded as an undoable command that snapshots both.
- **16-bit mask in an 8-bit document** — allowed; the compositor reads at the
  higher precision and the mask is preserved on save (precision is a property of
  the mask buffer, decoupled from the document depth).
- **Refine edge with no detectable edge** — degrades to a plain feather; never
  errors.

## Testing strategy

- **Unit** — `MaskBuffer` sparse semantics (absent tile = `kOpaque`),
  `evaluate()` with density/feather, invert idempotency, selection↔mask
  round-trip equality.
- **Golden image** — known layer + known mask → known composite for: hard mask,
  gray (partial) mask, feathered edge across a tile boundary, vector mask,
  clipping mask, vector×raster product, density scaling.
- **Property** — `mask = white` ⇒ composite identical to no mask; `mask = black`
  ⇒ layer contributes nothing; `invert(invert(m)) == m`.
- **Cross-tile** — feather and refine produce seamless results across the
  256-px tile seams (no visible halo discontinuity).
- **Undo** — paint/feather/invert/apply commands restore prior tiles exactly.

## Phasing

- **M4 (this doc lands)** — 8-bit raster layer masks, quick mask, clipping masks,
  selection↔mask, paint/fill/invert/feather/density, compositor coverage multiply,
  tile-delta undo.
- **M6** — 16-bit mask buffers riding in with [color management](15-color-management.md)
  and the [channels](19-channels.md) system (mask↔channel).
- **M8** — vector masks land with [vector & paths](18-vector-paths.md) and filter
  masks with [smart objects/smart filters](11-smart-objects.md).
- **M9** — object-aware "Mask All Objects" and AI-assisted refine edge.

## Open questions

- Should live feather have a per-document max radius beyond which it auto-bakes,
  and is that a setting or a fixed heuristic?
- Default mask precision: follow the document bit depth, or always 8-bit unless
  the user opts into 16-bit to save memory?
- Do we expose maskable *layer effects* individually, or is one mask per layer
  (plus the filter mask) sufficient for parity?

## References

- [03 — Layer system](03-layer-system.md)
- [07 — Selection system](07-selection-system.md)
- [08 — Brush engine](08-brush-engine.md)
- [05 — Adjustment layers](05-adjustment-layers.md)
- [18 — Vector & paths](18-vector-paths.md)
- [19 — Channels](19-channels.md)
- [20 — File I/O](20-file-io.md)
- [22 — Performance](22-performance.md)
- [ADR-0003 — Tile-based engine](../adr/0003-tile-based-engine.md)
- [Master architecture](../01-master-architecture.md) · [Glossary](../glossary.md)
