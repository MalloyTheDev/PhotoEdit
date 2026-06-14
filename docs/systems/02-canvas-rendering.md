# 02 — Canvas & Rendering

> Milestone: M2 · Status: Spec

## Purpose

The canvas/rendering system puts the [document](01-document-system.md) on screen
and lets the user navigate it. It is the first place the engine's design meets a
hard performance wall: a 30,000 × 30,000 px, many-layer document cannot be
recomposited or repainted in full on every brush dab and still feel live. The
answer — the crux of this whole document — is a **tile-based renderer**: an edit
hits a [`Rect`](../glossary.md), we mark only the overlapping
[tiles](../glossary.md) dirty, recomposite *only those tiles*, and upload *only
the changed tiles* to the GPU. Cost scales with the changed area, not the
document size ([ADR-0003](../adr/0003-tile-based-engine.md)).

This spec defines two things and the seam between them:

1. **Engine responsibilities** (`pe_core`, no Qt): the **view transform**
   (zoom/pan/rotation), the **dirty-tile compositing pipeline**, and the mapping
   from document tiles to a presentable surface via the [RHI](23-gpu-acceleration.md).
2. **App-shell responsibilities** (`src/app`, Qt6): the **canvas viewport
   widget**, input handling, and the **overlay layer** (rulers, guides, grid,
   marching ants, transform handles, brush cursor, color readout) drawn *above*
   the composited image.

The visible image is never stored; it is computed on demand from the layer tree.
Everything here is an elaboration of "one rendering model — the tile compositor
over a uniform [`Layer`](03-layer-system.md) contract."

## Requirements

**Functional**

- Display the document's composited image, honoring layer tree, masks, blend
  modes, opacity, and the working color space, converted through the display
  profile (see [color management](15-color-management.md)).
- Navigation: **zoom** (fit/fill/100%/arbitrary, around a focal point), **pan**,
  and **rotate-view** (rotating the *view*, never the document pixels).
- Show **checkerboard transparency** behind/through transparent areas.
- Overlays above the image: **rulers**, **guides**, **grid**, **marching ants**
  (active selection), **transform handles**, **brush-cursor preview** (tip
  outline at true size), and a **color readout** sampling the composited pixel
  under the cursor.
- Repaint must be **dirty-region driven**: a `DocumentChange{Pixels, region}`
  recomposites and re-uploads only the affected tiles; navigation that changes
  only the view transform re-presents cached tiles without recompositing.
- Reduced-resolution / progressive display while a full-res composite computes in
  the background (large documents, heavy stacks).

**Non-functional**

- Engine layer is pure C++20, **no Qt** ([ADR-0006](../adr/0006-headless-core-separation.md)).
  The viewport widget and input live in the [app shell](24-ui-workspace.md).
- Interactive: pan/zoom of a large document stays smooth; a single brush dab
  repaints in well under a frame because it touches only a few 256×256 tiles.
- The GPU is never *required*: a software fallback presents the same result,
  slower ([ADR-0002](../adr/0002-gpu-abstraction.md)). The CPU path defines
  correctness; the GPU must match within tolerance.
- Display GPU textures are a **cache**, invalidated per dirty tile; the document
  holds no GPU resources.

## Data model

Concrete, illustrative shapes in `namespace pe`. The engine pieces use only core
types (`Point`, `Size`, `Rect`, `kTileSize`, `TileCoord`, `tilesForRect`).

```cpp
namespace pe {

// Maps document pixel space <-> view (widget) space. Pure value type; no Qt.
// Pan is stored as the document point under the view origin; rotation is about
// the focal point. Kept as discrete fields (not a bare matrix) so the UI can
// reason about/clamp each independently, then derive the matrix on demand.
struct ViewTransform {
    double zoom = 1.0;          // 1.0 == 100% (1 doc px -> 1 device px)
    double rotationRad = 0.0;   // view rotation; document is untouched
    Point  focusDocPx{0, 0};    // document point pinned under focusViewPx
    Point  focusViewPx{0, 0};   // its location in the viewport (e.g. cursor)

    // Affine forms (device px == physical pixels after DPR scaling).
    [[nodiscard]] Affine2 docToView() const noexcept;
    [[nodiscard]] Affine2 viewToDoc() const noexcept;

    [[nodiscard]] Point  viewToDoc(Point viewPx)  const noexcept;
    [[nodiscard]] Point  docToView(Point docPx)   const noexcept;
    // The document-space region currently visible in a viewport of size vp.
    [[nodiscard]] Rect   visibleDocRect(Size vp)  const noexcept;
};

// What the renderer is asked to present this frame.
struct ViewportState {
    Size         deviceSize{};   // viewport size in device pixels (post-DPR)
    double       devicePixelRatio = 1.0;
    ViewTransform view{};
    bool         showCheckerboard = true;
    int          previewLevel = 0; // 0 == full res; >0 == 1/2^n mip for progressive
};

// A composited, immutable tile ready to present: the render thread owns these.
// Keyed by document tile coordinate; one entry per resident visible tile.
struct DisplayTile {
    TileCoord coord;             // document tile grid address
    RhiTexture texture;          // RGBA, display-converted; uploaded once per change
    uint64_t   contentVersion;   // bumped when the source tile is recomposited
};

// The engine-side renderer: owns the display-tile cache and the composite path.
// Lives below the Qt widget; the widget calls present() each frame.
class CanvasRenderer {
public:
    explicit CanvasRenderer(const Document&, Rhi&);

    // A document edit reported these dirty tiles: drop their cache entries so the
    // next present() recomposites + re-uploads exactly them.
    void invalidate(Rect dirtyDocRect);

    // Recomposite any dirty visible tiles, (re)upload changed ones, and draw the
    // image (with checkerboard) into the current RHI target. Returns the device
    // rect actually repainted (for the widget to scope its overlay redraw).
    Rect present(const ViewportState&);

private:
    const Document& doc_;
    Rhi& rhi_;
    std::unordered_map<TileKey, DisplayTile> cache_; // visible-tile LRU
    DirtyTileSet dirty_;                              // pending recomposite set
};

} // namespace pe
```

The **overlay** is deliberately *not* in this model. Marching ants, handles, the
brush cursor, and rulers are drawn by the shell on top of the presented image
using the same `ViewTransform`, via the [tool](09-tool-system.md) `drawOverlay`
hook and the selection's ants geometry. They never enter the composited result,
so toggling a guide or moving the cursor costs an overlay redraw, not a
recomposite.

## Behavior & algorithms

**The dirty-tile pipeline (the core loop).** The canonical brush-stroke path from
the [master architecture](../01-master-architecture.md#data-flow-the-life-of-a-brush-stroke)
ends here:

```
on DocumentChange{Pixels, dirtyDocRect}:           # from a committed command
    renderer.invalidate(dirtyDocRect)              # mark overlapping tiles stale
    widget.scheduleRepaint()

present(vp):                                        # once per frame on render thread
    visible   = vp.view.visibleDocRect(vp.deviceSize)
    span      = tilesForRect(visible)               # document tiles on screen
    toRender  = dirty_ ∩ span                       # only dirty AND visible
    for tile in toRender (partitioned across workers):
        rgbaf = compositor.compositeTile(tileBounds(tile), doc)   # working space
        rgba8 = colorPipeline.toDisplay(rgbaf, doc.profile())     # display convert
        cache_[tile] = upload(rgba8)                # one RHI upload per changed tile
        dirty_.erase(tile)
    drawCheckerboard(vp)                            # only where alpha < 1
    for tile in span: blit(cache_[tile], vp.view)   # GPU sampler does zoom/rotate
    return repaintedDeviceRect
```

Three properties fall out: (1) we recomposite the **intersection** of dirty and
visible tiles, so off-screen edits cost nothing until scrolled into view; (2)
zoom/pan/rotate that don't dirty pixels skip compositing entirely and just
re-blit cached textures through the new transform; (3) GPU upload granularity is
exactly one tile, matching [ADR-0003](../adr/0003-tile-based-engine.md).

**Document tiles ↔ screen.** Document tiles are axis-aligned 256×256 cells in
*document* space. The renderer composites them upright, then the **view
transform** (zoom, then rotation about the focal point) maps them to device
pixels at present time. Magnification and view rotation are a property of the
*sampler/blit*, not of stored pixels — so rotate-view is free of resampling cost
on the source and never degrades document data. At fractional zoom the renderer
samples cached tiles with bilinear (zoom-out uses mip levels of the display tiles
to avoid shimmer).

**Zoom about a focal point.** To keep the pixel under the cursor stationary while
zooming, we hold `focusDocPx` pinned under `focusViewPx` and only change `zoom`:

```
zoomAround(viewPx, newZoom):
    focusDocPx  = view.viewToDoc(viewPx)   # before changing zoom
    focusViewPx = viewPx
    view.zoom   = clamp(newZoom, kMinZoom, kMaxZoom)
    # docToView() now keeps focusDocPx exactly under viewPx
```

Pan adjusts `focusViewPx`; rotate-view changes `rotationRad` about the same
focus. All three are *view* operations — not [commands](../glossary.md), not
undoable, not dirtying — emitting only a lightweight "view changed" signal so the
[Navigator](24-ui-workspace.md) and rulers update.

**Checkerboard transparency.** Drawn first, in *device* space (so the squares are
a constant on-screen size independent of zoom), but only where the composited
alpha is below 1 — so it shows through transparent layers and around the canvas.
It is a presentation detail, never part of the document or any export.

**Progressive / reduced-resolution display.** When `previewLevel > 0` (heavy
stack, fast pan, first paint of a huge document), the renderer composites and
presents a coarser mip and schedules the full-res tiles in the background,
refining as worker results arrive. This keeps interaction responsive while
"full-res computes in the background" (master architecture, Memory section).

**Overlay rendering (app shell).** After `present()` returns the repainted device
rect, the widget draws overlays in the same coordinate frame:

```
paintOverlay(painter, view):
    if grid.showRulers:  drawRulers(view, rulerOrigin)
    if grid.showGrid:    drawGrid(view, spacing, subdivisions)
    if grid.showGuides:  for g in guides: drawGuide(g, view)
    if hasSelection:     drawMarchingAnts(selection.boundaryPath(), view, antPhase)
    if activeTransform:  drawHandles(transformBox(), view)
    drawBrushCursor(toolTipOutline(), cursorDocPx, view)   # true-size tip
```

Marching ants animate by advancing a dash phase on a timer (an overlay-only
repaint, no recomposite). The boundary path comes from the
[selection system](07-selection-system.md); handles from the
[transform system](10-transform-system.md); the tip outline from the active
[tool](09-tool-system.md)/[brush engine](08-brush-engine.md).

**Color picking under the cursor.** The readout samples the *composited* pixel
(or the active layer, per tool option) at `viewToDoc(cursor)`. It reads the
engine's composited tile in the document's working space and reports both raw and
display values; the eyedropper [tool](09-tool-system.md) reuses the same sample
to set the foreground color via a command.

## Interactions

- **[Compositor](../01-master-architecture.md#4-the-compositor--tree--tiles) /
  [layer system](03-layer-system.md):** `present()` calls `compositeTile` per
  dirty visible tile; this system is the compositor's primary consumer and the
  reason it is tile-granular.
- **[Document](01-document-system.md):** the renderer is a `DocumentObserver`;
  `DocumentChange{Pixels, region}` is precisely the dirty-tile trigger.
  View state (zoom/pan/rotation) lives in the viewport, *not* the document.
- **[Blend modes](04-blend-modes.md):** applied inside `compositeTile`, upstream
  of display conversion.
- **[Color management](15-color-management.md):** composite happens in the working
  space; `toDisplay` converts each tile through the display profile before upload.
- **[Performance](22-performance.md):** the display-tile cache is LRU and bounded;
  source tiles page to scratch; compositing of dirty tiles is partitioned across
  the worker pool.
- **[GPU / RHI](23-gpu-acceleration.md) + [ADR-0002](../adr/0002-gpu-abstraction.md):**
  textures, uploads, and the present blit go through the RHI; software fallback
  matches.
- **[Tool system](09-tool-system.md) / [selection](07-selection-system.md) /
  [transform](10-transform-system.md):** supply overlay geometry (cursor, ants,
  handles) the shell draws above the image.
- **[UI / workspace](24-ui-workspace.md):** owns the Qt `QWidget` viewport,
  translates Qt/tablet events into engine `PointerEvent`s, and hosts the
  Navigator/rulers/zoom UI.

## Performance, threading & GPU

- **Only dirty ∩ visible tiles recomposite**; only changed tiles re-upload.
  Off-screen edits defer until scrolled into view. This is the whole performance
  thesis ([ADR-0003](../adr/0003-tile-based-engine.md)).
- **Threading:** compositing of the dirty tile set is dispatched to the worker
  pool, partitioned by tile so workers never alias pixels; the **render thread**
  consumes immutable composited tiles and issues RHI uploads/blits; document
  mutation stays on the document thread (master architecture, Threading model).
- **GPU:** the view transform (zoom/rotation/pan) is realized by the present
  blit's sampler — essentially free — so navigation does not touch CPU pixels.
  Mip levels of display tiles prevent shimmer on zoom-out.
- **Cache budget:** the display-tile cache covers the visible set plus a small
  margin for smooth panning; eviction is LRU under the [performance](22-performance.md)
  RAM budget, separate from the source-tile cache.
- **Overlay** repaints (ants animation, cursor move, guide drag) never recomposite
  and never re-upload image tiles — they redraw vector overlays only.

## Edge cases & failure modes

- **Empty document / all-transparent:** `present()` draws only the checkerboard;
  zero tiles to composite.
- **View beyond canvas:** legal — panning past the edge shows checkerboard;
  layers extending off-canvas (negative coords) composite normally because tile
  math uses `floorDiv`.
- **Extreme zoom:** `zoom` is clamped to `[kMinZoom, kMaxZoom]`; at high
  magnification the source tile is point/bilinear sampled and a pixel grid may be
  shown; at tiny zoom, mips avoid aliasing.
- **Rotate-view + pick/paint:** input is mapped through `viewToDoc` (inverse
  including rotation) so cursor, ants, and dabs land on the correct document
  pixels regardless of view rotation.
- **High-DPI / fractional DPR:** all device-space math uses
  `devicePixelRatio`; the checkerboard and overlay line widths are in device
  pixels so they stay crisp.
- **GPU device loss / fallback:** on RHI device-lost, the display-tile cache is
  rebuilt from source tiles; if no GPU, the software backend presents identically.
- **Stale cache vs. edit race:** `contentVersion` per tile guards against
  presenting a texture older than the latest composite of that tile.
- **Huge dirty region** (e.g. a full-canvas adjustment toggled): bounded by the
  *visible* span, so even a document-wide change only recomposites what's on
  screen; the rest is invalidated lazily.

## Testing strategy

Headless `pe_core` unit tests (no widget needed):

- **View transform math:** `docToView`/`viewToDoc` are inverses;
  `zoomAround(p)` leaves the document point under `p` invariant (within ε) across
  zoom and rotation; `visibleDocRect` matches the transformed viewport corners.
- **Dirty-tile selection:** given a `dirtyDocRect` and a `ViewportState`, the set
  passed to the compositor equals `tilesForRect(dirty) ∩ tilesForRect(visible)` —
  asserted directly against `tilesForRect`.
- **Invalidate/recomposite count:** a small edit recomposites exactly the touched
  visible tiles and re-uploads exactly those (instrument the cache); a pure
  pan/zoom recomposites **zero** tiles.
- **Checkerboard/overlay isolation:** advancing the ants phase or moving the
  cursor triggers no `compositeTile` calls.

Golden-image tests (with a software-RHI present target):

- A known document at known zoom/rotation presents pixel-for-pixel to a committed
  reference (CPU path), including checkerboard regions.
- Software vs. D3D12 present of the same scene match within the documented
  tolerance ([ADR-0002](../adr/0002-gpu-abstraction.md)).
- Progressive: the `previewLevel>0` frame and the final full-res frame converge to
  the same reference.

Overlay drawing (ants, handles, ruler ticks) is validated in app-shell tests
against small rendered references; the engine tests own the composited image.

## Phasing

- **M2 (this doc lands):** view transform (zoom/pan/rotate-view), the dirty-tile
  pipeline, checkerboard, the display-tile cache, D3D12 present + software
  fallback, and the Layers-panel-driven repaint. Rulers/guides/grid overlay and
  brush-cursor/color readout ship with the viewport. Marching ants arrive
  fully in M4 with [selections](07-selection-system.md); a stub boundary renderer
  exists earlier.
- **M3:** brush-cursor preview and high-frequency repaint are stressed by the
  [brush engine](08-brush-engine.md); transform handles by the basic
  [transform](10-transform-system.md).
- **M5+:** progressive/reduced-resolution preview matters more as filters and
  adjustment stacks deepen; GPU compositing of more of the stack.
- **M6:** display conversion becomes fully profile-aware (soft-proof view,
  gamut-warning overlay) via [color management](15-color-management.md).

## Open questions

- **Where does rotate-view resampling live** at very high zoom — sample the source
  tile rotated, or pre-rotate a working surface? Leaning sampler-only until
  profiling says otherwise.
- **Display-tile mip generation:** lazily on zoom-out vs. eagerly on upload — a
  memory/latency trade-off to settle with data.
- **Pixel-grid and per-100%-snap behavior:** exact zoom levels that snap to whole
  device pixels for crispness; UX detail deferred to [UI](24-ui-workspace.md).
- **Overlay in engine vs. shell for headless render tests:** keep overlays in the
  shell (current plan) or expose a headless overlay rasterizer for golden tests?

## References

- [01 — Master Architecture](../01-master-architecture.md) — the compositor loop,
  the life of a brush stroke, threading and memory models.
- [00 — Vision & Scope](../00-vision-and-scope.md) — "a tiled engine that only
  recomputes the dirty region."
- [Glossary](../glossary.md) — Tile, Dirty region, Compositor, Canvas, RHI.
- ADRs: [0003 — tile-based engine](../adr/0003-tile-based-engine.md),
  [0002 — GPU abstraction](../adr/0002-gpu-abstraction.md),
  [0006 — headless core](../adr/0006-headless-core-separation.md).
- Sibling systems: [layers](03-layer-system.md), [blend modes](04-blend-modes.md),
  [document](01-document-system.md), [performance](22-performance.md),
  [GPU acceleration](23-gpu-acceleration.md), [color management](15-color-management.md),
  [selection](07-selection-system.md), [transform](10-transform-system.md),
  [tool system](09-tool-system.md), [UI / workspace](24-ui-workspace.md).
