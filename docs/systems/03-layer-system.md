# 03 — Layer System

> Milestone: M1 · Status: Spec

## Purpose

Layers are the heart of PhotoEdit. A [`Layer`](../glossary.md) is a node in the
[document](01-document-system.md)'s tree, and the
[compositor](../01-master-architecture.md#4-the-compositor--tree--tiles) walks
that tree bottom-to-top to produce the visible image. The defining property of a
flagship editor — *non-destructive, composable editing* — is realized here: pixel
content, text, shapes, fills, color/tonal adjustments, smart objects, and groups
all live in one ordered stack and all satisfy a **single polymorphic contract**,
so the compositor treats every kind uniformly.

This document specifies the `Layer` base contract, the layer **kinds**, the
per-layer **properties** every layer carries, how each kind satisfies
`renderInto`, the **tiled, copy-on-write** pixel storage that pixel layers use,
and the compositor's recursive **tree walk** (groups, clipping, and the
adjustment-layer twist). The per-pixel blend math itself lives in
[blend modes](04-blend-modes.md); this doc owns the *structure* the compositor
walks and the *kinds* of node it walks over.

## Requirements

**Functional**

- A `Layer` base type exposing identity (id, name, kind) and the universal
  properties: **visible/hidden**, **locked** (with **lock transparency** as a
  distinct sub-lock), **opacity** `[0,1]`, **fill opacity** `[0,1]`,
  **blend mode**, optional **mask** (raster and/or vector — see
  [masks](06-masks.md)), **transform**, **clipping** state (clipped to the layer
  below), layer **effects/styles**, and per-layer **metadata**.
- Support these layer **kinds**: **pixel**, **background**, **text**, **shape**,
  **fill** (solid/gradient/pattern), **adjustment**, **smart object**, **group**,
  **artboard**, and (later) **video** and **generative**.
- A uniform render contract, `renderInto(TileRequest, TileSink&)`: produce this
  layer's pixel contribution for a tile region in the working color space at the
  document bit depth, honoring the layer's **own effects** but **not** its blend
  mode/opacity/mask (the compositor applies those).
- **Pixel** layers store pixels in 256×256 [tiles](../glossary.md); absent tiles
  read as transparent; tiles are **reference-counted and copy-on-write**.
- **Groups** are layers that contain a sub-stack and composite their children
  into an isolated buffer, then blend that buffer down as a unit.
- **Adjustment** layers contribute no pixels; they **transform the accumulated
  result** beneath them at composite time.
- **Clipping masks**: a run of layers clipped to a base layer is confined by that
  base layer's coverage (delegated to [masks](06-masks.md) for the mechanism).
- Every structural and property change is a reversible [Command](../glossary.md)
  (add/delete/duplicate/reorder/group/ungroup; set opacity/blend/visibility/lock/
  name), notifying observers via a `DocumentChange`.

**Non-functional**

- Pure C++20 in `pe_core`; **no Qt** ([ADR-0006](../adr/0006-headless-core-separation.md)).
  The Layers panel UI is in the [app shell](24-ui-workspace.md).
- Pixel storage is sparse, RAM-budgeted, and scratch-pageable
  ([ADR-0003](../adr/0003-tile-based-engine.md), [performance](22-performance.md));
  a layer costs memory only where it has content.
- `renderInto` is **side-effect free and reentrant**: the compositor may call it
  for many tiles concurrently across the worker pool without aliasing.
- The CPU reference defines correctness; SIMD/GPU compositing must match within
  the golden-image tolerance.

## Data model

Concrete, illustrative shapes in `namespace pe`, building on the `Layer` sketch in
[01-master-architecture.md](../01-master-architecture.md#3-layer--a-node-in-the-tree).
Real headers may differ in detail, not in concept.

```cpp
namespace pe {

using LayerId = uint64_t;

enum class LayerKind : uint8_t {
    Pixel = 0,
    Background,    // opaque base; no transparency, sits at the bottom
    Text,          // editable type (M8)
    Shape,         // vector shape/fill (M8)
    Fill,          // solid / gradient / pattern fill
    Adjustment,    // transforms the result below it (M5)
    SmartObject,   // embeds source + stored transform + smart filters (M8)
    Group,         // a nested sub-stack
    Artboard,      // a group with its own bounds/clip (M8 first-class)
    Video,         // later
    Generative,    // later (AI-produced, re-generable)
};

// Per-layer locks (independent toggles; lockTransparency is the common one).
struct LayerLocks {
    bool transparency = false;  // can't change a pixel's alpha (paint only opaque)
    bool pixels       = false;  // can't paint/edit color
    bool position     = false;  // can't move/transform
    bool all          = false;  // fully locked
};

// What renderInto is asked for: the tile region (document space) and the target
// bit depth / working space. The compositor batches these per visible dirty tile.
struct TileRequest {
    Rect      region;       // document-space pixel rect (usually a tileBounds())
    BitDepth  depth;        // document bit depth (U8 in M1)
    // working color space is implied by the document profile
};

// The base contract. Every kind below satisfies it so the compositor is uniform.
class Layer {
public:
    virtual ~Layer() = default;

    [[nodiscard]] LayerId     id() const noexcept;
    [[nodiscard]] LayerKind   kind() const noexcept;
    [[nodiscard]] std::string name() const;

    // Universal compositing properties (mutated only via commands).
    [[nodiscard]] bool        visible() const noexcept;
    [[nodiscard]] LayerLocks  locks() const noexcept;
    [[nodiscard]] float       opacity() const noexcept;      // [0,1] whole layer
    [[nodiscard]] float       fillOpacity() const noexcept;  // [0,1] content only
    [[nodiscard]] BlendMode   blendMode() const noexcept;
    [[nodiscard]] const Mask* mask() const noexcept;         // optional, see 06
    [[nodiscard]] const Transform& transform() const noexcept;
    [[nodiscard]] bool        clipped() const noexcept;      // clipped to layer below
    [[nodiscard]] const LayerEffects& effects() const noexcept; // styles

    // Tightest document-space rect this layer can affect (for dirty/visible cull).
    [[nodiscard]] virtual Rect contentBounds() const noexcept = 0;

    // Render this layer's pixel contribution for the request into the working
    // space at the document depth, applying its OWN effects but NOT its blend
    // mode / opacity / mask. Must be reentrant and side-effect free.
    virtual void renderInto(const TileRequest&, TileSink&) const = 0;

    // Adjustment-kind layers override this instead: transform the accumulated
    // backdrop in-place rather than contributing pixels. Default: no-op.
    [[nodiscard]] virtual bool isAdjustment() const noexcept { return false; }
    virtual void applyTo(TileBuffer& backdrop, const TileRequest&) const {}
};

// A pixel layer: sparse, copy-on-write tiled storage in the document depth.
class PixelLayer final : public Layer {
public:
    [[nodiscard]] Rect contentBounds() const noexcept override;     // union of tiles
    void renderInto(const TileRequest&, TileSink&) const override;  // copy/convert tiles

    [[nodiscard]] bool hasTileAt(TileCoord) const noexcept;         // false => transparent
private:
    TileStore tiles_;   // reference-counted, COW; absent tile == transparent
};

// A group: composites its children into an isolated buffer, then is blended down
// as one layer. 'isolated'/'knockout' control how its backdrop is seen.
class GroupLayer final : public Layer {
public:
    [[nodiscard]] std::span<const std::unique_ptr<Layer>> children() const noexcept;
    [[nodiscard]] bool isolated() const noexcept;   // children blend over transparent
    [[nodiscard]] Rect contentBounds() const noexcept override; // union of children
    void renderInto(const TileRequest&, TileSink&) const override; // recurse + flatten
};

} // namespace pe
```

`Document` owns the root `GroupLayer` (the layer tree) and exposes it read-only;
all mutation is via commands routed through [history](21-history-undo.md), per the
[document spec](01-document-system.md).

## Behavior & algorithms

**The compositor tree walk.** The loop from the
[master architecture](../01-master-architecture.md#4-the-compositor--tree--tiles),
made concrete per tile. `result` accumulates straight-alpha working-space pixels;
`compositeOver` is the [blend-mode](04-blend-modes.md) kernel:

```
compositeTile(region, stack):                  # stack = a GroupLayer's children
    result = transparent(region)               # straight-alpha working buffer
    i = 0
    while i < stack.size:
        layer = stack[i]
        if not layer.visible or layer.opacity == 0:
            i += 1; continue

        if layer.isAdjustment():               # transforms result, no pixels
            scoped = result
            if layer.mask: scoped = applyMaskCoverage(scoped, layer, region)
            layer.applyTo(scoped, request(region))     # Curves/Levels/…
            result = blendCoverage(result, scoped, layer)  # masked + opacity blend in
            i += 1; continue

        # Gather a clipping run: this layer + any layers clipped to it.
        base, clipped = takeClippingRun(stack, i)       # see masks: clipping
        src = render(base, region)                      # base's own pixels+effects
        for c in clipped:                               # clip group draws onto base alpha
            csrc = render(c, region)
            src  = compositeOver(c.blendMode, src, csrc, effOpacity(c))
        src = confineToCoverage(src, base.alpha)        # clipping mask = base coverage

        src = applyLayerEffects(src, base)              # drop shadow/stroke/…
        src = applyMask(src, base, region)              # raster×vector mask (06)
        result = compositeOver(base.blendMode, result, src, effOpacity(base))
        i += 1 + clipped.size
    return result

render(layer, region):                          # the per-layer entry the walk uses
    if layer.kind == Group:  return compositeTile(region, layer.children)  # recurse
    sink = TileBuffer(region)
    layer.renderInto(request(region), sink)     # pixel/text/shape/fill/SO produce here
    return sink
```

Two subtleties the contract encodes:

- **Opacity vs. fill opacity.** `opacity()` scales the whole layer *including its
  effects*; `fillOpacity()` scales only the rendered content, leaving layer
  effects at full strength (matching Photoshop). `effOpacity()` and effect
  application account for both. Certain "special-8" blend modes also respond
  differently to fill opacity — captured in [blend modes](04-blend-modes.md).
- **Adjustment layers are the twist in the loop.** They do not add a `src`; their
  `applyTo` runs the color/tonal op (Curves, Levels, Hue/Sat…) over the
  *accumulated `result`* beneath them, scoped by their mask and blended back by
  their opacity. See [adjustment layers](05-adjustment-layers.md) for the ops.

**How each kind satisfies the contract:**

| Kind | `renderInto` produces |
| --- | --- |
| **Pixel** | Its tiles, converted to working space/depth; absent tiles → transparent. Copy-on-write, so reads are zero-copy until written. |
| **Background** | Opaque pixels with no alpha; always at the stack bottom; locked from transparency by default. |
| **Fill** | A procedural solid/gradient/pattern evaluated per pixel in `region` — no stored raster (resolution-independent). |
| **Text** | Rasterizes shaped glyph runs for `region` on demand from the editable text model ([text](17-text-typography.md)); cached, regenerated on edit. |
| **Shape** | Rasterizes vector geometry (fill + stroke) for `region` ([vector & paths](18-vector-paths.md)); resolution-independent. |
| **Adjustment** | *Nothing* — overrides `applyTo` instead (transforms the backdrop). |
| **Smart Object** | Renders embedded/linked source through its stored transform + smart filters ([smart objects](11-smart-objects.md)); never resamples the source destructively. |
| **Group** | Recurses: composites children into an isolated buffer (above). |
| **Artboard** | A group clipped to its bounds with an optional backing fill. |

**Tiled pixel storage (copy-on-write).** A `PixelLayer`'s `TileStore` is a sparse
map from `TileCoord` to a reference-counted tile. Editing a tile that is shared
(refcount > 1 — e.g. captured by an undo snapshot or a duplicated layer) triggers
a **copy** before write; tiles touched by no edit stay shared. This makes
duplicate-layer and undo snapshots cheap (share unchanged tiles) and is the same
mechanism [history](21-history-undo.md) uses for tile-delta undo. Absent tiles are
transparent, so an empty or mostly-empty layer costs almost nothing.

**Groups, isolation, and knockout.** A non-isolated group lets its children blend
as if they were inlined into the parent stack; an **isolated** group composites
its children over transparency first, then blends the flattened result down — so
blend modes inside the group don't "see" the backdrop. Knockout (punching through
to a lower layer) is expressed as a group/clip property. Groups recurse to
arbitrary depth; the walk above calls itself for `Group` children.

**Clipping masks.** `takeClippingRun` collects the base layer and the contiguous
run of layers above it whose `clipped()` is true. They are composited together,
then confined to the base layer's rendered alpha — "use the layer below me as my
mask." The coverage multiply reuses the [masks](06-masks.md) kernel.

**Mutation as commands.** Structural edits (`AddLayerCommand`,
`DeleteLayerCommand`, `DuplicateLayerCommand`, `ReorderLayerCommand`,
`GroupLayersCommand`) and property edits (`SetOpacityCommand`,
`SetBlendModeCommand`, `SetVisibilityCommand`, `SetLockCommand`,
`RenameLayerCommand`) all run through [history](21-history-undo.md), emit the
right `DocumentChange` kind (`LayerStructure` or `LayerProps`, with the affected
`LayerId` and dirty region), and are undoable. Reorder/visibility/opacity dirty
the layer's `contentBounds`; structure changes may dirty the whole composited
region of the affected subtree.

## Interactions

- **[Document](01-document-system.md):** owns the root group; exposes the tree
  read-only; all layer mutation is a command it routes through history.
- **[Compositor / canvas](02-canvas-rendering.md):** the sole consumer of
  `renderInto`/`applyTo`; calls them per dirty visible tile and blends the result.
- **[Blend modes](04-blend-modes.md):** `compositeOver` provides the per-pixel
  math (`blendMode`, opacity); non-separable modes operate on whole pixels.
- **[Masks](06-masks.md):** each layer's optional raster/vector mask scales
  coverage before blending; clipping masks are expressed via the clipping run.
- **[Adjustment layers](05-adjustment-layers.md):** the `Adjustment` kind; its
  `applyTo` transforms the backdrop — the doc that owns the actual ops.
- **[Smart objects](11-smart-objects.md):** the `SmartObject` kind; renders source
  through a stored transform + smart filters non-destructively.
- **[Transform system](10-transform-system.md):** each layer's `Transform`
  positions/scales/rotates its content; pixel layers resample on bake, smart
  objects keep the transform live.
- **[Channels](19-channels.md):** a pixel layer's per-channel data and saved
  selections surface here; layer alpha is a channel.
- **[Text](17-text-typography.md) / [vector & paths](18-vector-paths.md):** back
  the Text/Shape kinds' `renderInto`.
- **[History](21-history-undo.md) + [ADR-0005](../adr/0005-command-history-model.md):**
  every structural/property edit is a reversible command; tile-delta undo reuses
  the COW tiles.
- **[Performance](22-performance.md) + [ADR-0003](../adr/0003-tile-based-engine.md):**
  the tile store's refcounting, paging, and worker partitioning.
- **App shell:** the Layers panel (thumbnails, drag-reorder, opacity/blend
  controls, lock/visibility toggles) observes `DocumentChange` and issues
  commands; the engine knows nothing of Qt.

## Performance, threading & GPU

- **Sparse, COW tiles:** memory tracks content, not canvas area; duplicate and
  undo snapshots share unchanged tiles; a copy happens only on first write to a
  shared tile.
- **Reentrant `renderInto`:** the compositor partitions the dirty visible tile set
  across the [worker pool](../01-master-architecture.md#threading-model); each
  worker renders/composites disjoint tiles, so there is no aliasing and no lock on
  the tree (which is immutable during a composite pass).
- **`contentBounds` culling:** a layer whose bounds don't intersect a tile is
  skipped entirely; groups cull whole subtrees.
- **Isolated groups** add one scratch buffer per group per tile; the compositor
  pools these. Deeply nested groups are flattened tile-by-tile, not whole-canvas.
- **GPU:** `compositeOver` and mask/coverage multiplies map to RHI shaders;
  pixel-layer tiles upload once per change. The CPU reference defines correctness;
  GPU matches within tolerance ([ADR-0002](../adr/0002-gpu-abstraction.md)).

## Edge cases & failure modes

- **Empty layer / all transparent:** no tiles; `renderInto` yields transparent;
  contributes nothing. The model tolerates an empty stack (composites to
  transparent; canvas shows checkerboard).
- **Layers beyond the canvas:** legal; `contentBounds` and tiles may use negative
  coordinates (tile math uses `floorDiv`). Crop/canvas-resize retains off-canvas
  pixels unless flattened.
- **Background layer rules:** opaque, transparency-locked, pinned to the bottom;
  "convert to normal layer" is a command that unlocks alpha.
- **Lock transparency while painting:** the [brush engine](08-brush-engine.md)
  must honor `locks().transparency` — paint modifies color but not alpha where set.
- **Opacity 0 vs. hidden:** `opacity()==0` still participates structurally (and
  costs a cull check) but contributes nothing; `visible()==false` is skipped
  earlier — they are distinct, both undoable.
- **Clipping run with hidden base:** a hidden base layer hides the clipped run
  with it (nothing to clip onto).
- **Adjustment layer at the very bottom:** transforms only transparency → no-op;
  legal, harmless.
- **Cyclic/linked smart objects:** the tree is acyclic by construction; linked
  smart objects that would recurse are detected and broken by
  [smart objects](11-smart-objects.md), not here.

## Testing strategy

Headless `pe_core` unit tests (the dependency-free harness):

- **Contract conformance:** each kind's `renderInto`/`applyTo` produces expected
  pixels for a small fixture; `contentBounds` equals the true affected rect
  (asserted against `tilesForRect`).
- **COW semantics:** writing a shared tile copies (refcount drops on the original);
  duplicating a layer shares all tiles until one is edited; undo restores the
  shared tile byte-exact.
- **Command discipline:** add/delete/duplicate/reorder/group and each property
  setter push history, emit one `DocumentChange` of the right kind with the
  correct dirty region, and undo to byte-identical state.
- **Culling:** a layer outside a tile is skipped; a hidden/zero-opacity layer
  contributes nothing.

Golden-image tests (compositor output, the M1 exit gate):

- Known multi-layer stacks → known composited buffers for: stacked opacities,
  every separable [blend mode](04-blend-modes.md), groups (isolated and not),
  nested groups, clipping masks, fill-opacity vs. opacity, and an adjustment layer
  over content.
- Software vs. SIMD/GPU composite of the same stack match within tolerance.

## Phasing

- **M1 (this doc lands):** `Layer` contract; **pixel**, **background**, and
  **group** kinds; tiled COW pixel storage; the compositor tree walk with all
  separable blend modes, opacity/fill-opacity/visibility; add/delete/duplicate/
  reorder/group as commands; golden-image gate. **Fill** kind (solid) is cheap to
  add here. Other kinds are modeled in `LayerKind` but inert.
- **M4:** masks (raster, clipping) integrate into the walk
  ([masks](06-masks.md)); lock-transparency honored by painting.
- **M5:** **adjustment** kind activated with the real ops
  ([adjustment layers](05-adjustment-layers.md)); fill (gradient/pattern).
- **M8:** **text**, **shape**, **smart object**, first-class **artboard** kinds
  via [text](17-text-typography.md), [vector & paths](18-vector-paths.md),
  [smart objects](11-smart-objects.md).
- **Later:** **video** and **generative** kinds.

## Open questions

- **Effects/styles ownership:** model `LayerEffects` here as a fixed set, or as a
  list of effect nodes (more like smart filters) for extensibility? Leaning
  toward a small structured set in M1, generalized later.
- **Knockout/isolation surface:** expose full Photoshop knockout semantics, or the
  common subset? Defer the exotic cases until groups are stressed.
- **Layer-level color space override:** could a layer carry its own working space
  (e.g. a linear group)? Likely unnecessary; revisit with
  [color management](15-color-management.md).
- **Very deep trees:** is recursion depth ever a concern, or do we iterate with an
  explicit stack? Iterative flatten if profiling shows deep nesting in practice.

## References

- [01 — Master Architecture](../01-master-architecture.md) — the `Layer` contract
  and the compositor loop this elaborates.
- [00 — Vision & Scope](../00-vision-and-scope.md) — "non-destructive everything."
- [Glossary](../glossary.md) — Layer, Layer stack/tree, Compositor, Tile,
  Adjustment layer, Smart object, Non-destructive.
- ADRs: [0003 — tile-based engine](../adr/0003-tile-based-engine.md),
  [0005 — command/history model](../adr/0005-command-history-model.md),
  [0006 — headless core](../adr/0006-headless-core-separation.md),
  [0002 — GPU abstraction](../adr/0002-gpu-abstraction.md).
- Sibling systems: [document](01-document-system.md),
  [canvas & rendering](02-canvas-rendering.md), [blend modes](04-blend-modes.md),
  [masks](06-masks.md), [adjustment layers](05-adjustment-layers.md),
  [smart objects](11-smart-objects.md), [transform](10-transform-system.md),
  [channels](19-channels.md), [text](17-text-typography.md),
  [vector & paths](18-vector-paths.md), [history](21-history-undo.md),
  [performance](22-performance.md).
