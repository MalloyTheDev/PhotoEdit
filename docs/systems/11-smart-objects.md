# 11 — Smart Objects

> Milestone: M8 · Status: Spec

## Purpose

A **smart object** is a layer that *contains* source content instead of *being*
pixels. It renders that source through a stored transform and an ordered stack of
[smart filters](12-filter-engine.md), caching a preview — but it never touches the
original. This is the structural heart of the
[vision](../00-vision-and-scope.md)'s "non-destructive everything": a smart object
is how a 4000×4000 photo can be scaled down to 500×500 to fit a layout, nudged,
filtered, and then scaled back to 100% **with no quality loss**, because the only
thing that was ever resampled is the *preview*, not the data.

The mental model: a smart object is a tiny embedded (or linked)
[document](01-document-system.md) plus a recipe — `transform · filters · mask` —
and a cached rasterization of that recipe at the resolution the
[compositor](02-canvas-rendering.md) currently needs. Change any input (the
source, the transform, a filter, the requested resolution) and the smart object
**re-renders its preview on demand** from the pristine source. "Edit Contents"
opens the source as its own editable document; committing it re-renders every
place that source is used. Because the source is a real sub-document or an
external file reference, smart objects also carry the
[camera-raw](16-camera-raw.md), [vector](18-vector-paths.md), and PSD/PSB/TIFF
"placed file" workflows.

This document owns the `SmartObjectLayer` data model, the re-render-on-demand
pipeline, the embedded-vs-linked source distinction with update propagation, and
how all of this stays one `Layer` the compositor treats like any other. The
filter *math* and the smart-filter stack mechanics live in the
[filter engine](12-filter-engine.md); the transform *math* lives in the
[transform system](10-transform-system.md). This spec is how they compose into a
re-editable container.

## Requirements (Functional + Non-functional)

**Functional**

- Wrap source content as a [`Layer`](03-layer-system.md) kind
  (`LayerKind::SmartObject`) that satisfies the standard `renderInto` contract, so
  the compositor blends it identically to a pixel layer.
- Support these **source kinds**:
  - **Embedded** — the source bytes live inside the document (a sub-`Document` or
    an embedded encoded file). Self-contained; travels with the file.
  - **Linked** — a reference (path/URI + content hash + mtime) to an *external*
    file; the source is loaded on demand and updates propagate when it changes.
  - **Camera-raw** — a raw file decoded through the
    [camera-raw](16-camera-raw.md) pipeline whose settings are re-editable.
  - **Vector** — placed [vector/path](18-vector-paths.md) art (or PDF/AI), kept as
    geometry and rasterized to the needed resolution (resolution-independent).
  - **File-based raster** — a placed PSD/PSB, TIFF, PNG, JPEG, etc., either
    embedded or linked.
- Store a **non-destructive transform** (`Mat3` affine + a warp mesh for
  distort/perspective/warp) applied to the source at render time; re-editing the
  transform never compounds resampling.
- Carry an **ordered list of smart filters** (each a
  [`Filter`](12-filter-engine.md) + params + its own filter
  [mask](06-masks.md)), re-editable, reorderable, toggleable, deletable.
- Carry an optional **layer mask** and a **filter mask** (the white box on the
  smart-filter row), both ordinary [mask](06-masks.md) data.
- **Cache preview tiles** of the rendered result at the resolution the compositor
  asks for, and **re-render on demand** when transform, source, filters, or the
  requested scale change — at the needed resolution only.
- **Edit Contents**: open the source as its own document; on commit, re-render the
  smart object and every other instance that shares the source.
- Support **multiple instances** of one embedded source (duplicating a smart
  object shares the source by default — edit once, update all) and **"New Smart
  Object via Copy"** to deliberately break the link.
- Convert a selection of layers **into** a smart object (embedding them as a
  sub-document) and **rasterize** a smart object back to pixels (destructive,
  undoable).
- **Replace Contents**: swap the source while keeping transform, filters, and mask
  (the template/mockup workflow).

**Non-functional**

- `pe_core`, pure C++20 — **no Qt**. "Place file" dialogs, the badge on the layer
  thumbnail, and the linked-source "relink/update" UI are
  [app-shell](24-ui-workspace.md) concerns
  ([ADR-0006](../adr/0006-headless-core-separation.md)).
- A smart object whose inputs are unchanged costs **nothing to re-render**: the
  cached preview tiles are served straight to the compositor like any layer tiles.
- Re-render cost scales with the **requested region and resolution**, not the
  source's native size; a 30k×30k linked source displayed at fit-on-screen
  rasterizes only the visible tiles at screen scale.
- The pristine source is **never mutated** by transforms or filters; rasterizing
  to pixels is the *only* operation that discards it, and only on explicit
  request.
- CPU reference defines the rasterization; SIMD/GPU resampling and filtering match
  within the golden-image tolerance.

## Data model (concrete C++ in `namespace pe`)

Builds on the engine foundation (`Rect`/`Size` from `pe/core/Geometry.hpp`,
`kTileSize`/`TileCoord` from `pe/core/Tile.hpp`) and the `Layer`, `Document`,
`Mask`, `Transform`, and `Filter` contracts.

```cpp
namespace pe {

// Where a smart object's pixels come from.
enum class SourceKind : uint8_t {
    Embedded,     // bytes owned by this document (sub-Document or encoded blob)
    Linked,       // external file referenced by path/URI
    CameraRaw,    // raw file decoded via the camera-raw pipeline
    Vector,       // placed vector/PDF art, kept as geometry
    FileRaster    // placed PSD/PSB/TIFF/PNG/... (embedded or linked)
};

// A reference to an external linked file plus the state needed to detect change.
struct LinkedSourceRef {
    std::string uri;          // absolute or document-relative path/URL
    uint64_t    contentHash = 0;   // hash of last-loaded bytes
    int64_t     modifiedTimeMs = 0;
    bool        missing = false;   // resolved-but-not-found at last check
};

// The pristine source. Exactly one variant is active per smart object.
class SmartSource {
public:
    [[nodiscard]] SourceKind kind() const noexcept;
    [[nodiscard]] Size       nativeSize() const noexcept;  // source pixel/doc size
    [[nodiscard]] int        nativePpi() const noexcept;

    // Embedded sub-document (Embedded/converted-layers/FileRaster-embedded), or
    // null for purely external links until loaded.
    [[nodiscard]] const Document* embeddedDoc() const noexcept;

    // For Linked sources: the reference and a (possibly evicted) cached document.
    [[nodiscard]] const LinkedSourceRef* link() const noexcept;

    // CameraRaw / Vector keep their own re-editable parameter state:
    [[nodiscard]] const RawSettings*  rawSettings() const noexcept;  // 16-camera-raw
    [[nodiscard]] const VectorArt*    vectorArt()   const noexcept;  // 18-vector-paths

    // Render the *source* (no SO transform/filters) for a region at a given
    // output scale into working-space pixels. Sub-documents recurse the compositor.
    void renderSource(TileRequest, float outputScale, TileSink&) const;
};

// One entry in the smart-filter stack. The Filter + params are owned here; the
// mask scopes it. See systems/12-filter-engine.md.
struct SmartFilterEntry {
    std::unique_ptr<Filter> filter;   // the effect (blur, sharpen, camera-raw filter, ...)
    FilterParams            params;   // re-editable parameters
    BlendMode               blendMode = BlendMode::Normal;  // filter blend
    float                   opacity   = 1.0f;
    bool                    enabled   = true;
    Mask                    mask;     // per-filter mask (optional; default reveal)
};

// The smart object layer itself.
class SmartObjectLayer final : public Layer {
public:
    [[nodiscard]] LayerKind kind() const noexcept { return LayerKind::SmartObject; }

    [[nodiscard]] const SmartSource& source() const noexcept;
    [[nodiscard]] const Transform&   transform() const noexcept;  // SO->canvas, non-destructive
    [[nodiscard]] const std::vector<SmartFilterEntry>& filters() const noexcept;
    [[nodiscard]] const Mask* mask() const noexcept;              // layer mask (from Layer)
    [[nodiscard]] uint64_t    sourceId() const noexcept;          // shared-instance identity

    // Compositor entry point: serve cached preview tiles for the region, rendering
    // (and caching) any that are dirty at the requested resolution.
    void renderInto(TileRequest, TileSink&) const override;

    // Invalidate the preview cache for a region (or all) — called when source,
    // transform, filters, or requested scale change.
    void invalidate(Rect regionInCanvas) noexcept;

private:
    mutable TileCache previewCache_;   // rendered result, keyed by tile + scale level
    float cachedScale_ = 0.0f;         // resolution the cache was rendered at
};

} // namespace pe
```

The cache is keyed by `(TileCoord, scaleLevel)`: zooming to a finer level than the
cache holds triggers a higher-resolution re-render of the visible tiles only,
while the pristine `SmartSource` is untouched. `sourceId` ties instances that
share an embedded source so one edit invalidates all of them.

## Behavior & algorithms

### Re-render on demand (the core loop)

```
renderInto(region, sink):                  # called by the compositor per tile
    scale = region.requestedScale          # screen scale, or 1.0 for full-res bake
    for tile in tilesForRect(region):
        if previewCache.valid(tile, scale): sink.emit(previewCache.get(tile, scale)); continue
        rendered = renderPreviewTile(tile, scale)
        previewCache.put(tile, scale, rendered)   # CoW, RAM-budgeted, scratch-pageable
        sink.emit(rendered)

renderPreviewTile(tile, scale):
    # 1. Figure out which source region this canvas tile maps back to.
    srcRegion = transform.inverse().mapRect(tileBounds(tile)).expandedForResampling()
    # 2. Pull pristine source pixels at the resolution we actually need.
    srcPixels = source.renderSource(srcRegion, scale)        # NEVER modifies the source
    # 3. Apply the non-destructive transform (resample once, here only).
    warped = transform.resample(srcPixels, into=tileBounds(tile), scale)
    # 4. Run the smart-filter stack (each filter is re-editable; see 12-filter-engine).
    px = warped
    for f in filters where f.enabled:
        out = f.filter.apply(px, ctx{depth, colorspace, region})
        px  = blendWithMask(px, out, f.mask, f.blendMode, f.opacity)
    # 5. Layer mask is applied by the compositor afterward, like any layer.
    return px
```

The single discipline that makes quality loss impossible: **the source is sampled
fresh every render**, and the transform is applied *once* on the way to the
preview. Scaling to 5% then back to 100% re-runs step 2–3 from the pristine
source at 100%; there is no chain of resamples to accumulate softness, unlike a
destructive transform which would resample the *already-resampled* pixels.

### Transform without compounding

The smart object stores its transform as a `Transform` (affine `Mat3` plus an
optional warp mesh from the [transform system](10-transform-system.md)).
Re-entering Free Transform edits *that stored transform*, it does not transform
the current preview. So a sequence of edits — scale, rotate, scale back —
collapses to a single net `Transform` that is applied once to the pristine
source. (Contrast a pixel layer, where each transform commit resamples the prior
result; that is exactly the loss smart objects avoid.) Distort/perspective/warp
are the warp-mesh path; the resample in step 3 uses the same high-quality
(bicubic/Lanczos) sampler the transform system defines.

### Edit Contents and propagation

```
editContents(so):
    doc = so.source.openAsDocument()       # embedded sub-doc, or load the linked file
    present doc in a new editing context    # app-shell opens a document tab
    on commit(doc):                         # user saves the inner document
        so.source.replaceEmbedded(doc)      # or write back the linked file
        for inst in instancesSharing(so.sourceId()):
            inst.invalidate(ALL)            # every shared instance re-renders
```

Editing contents of an **embedded** source mutates the sub-document and
invalidates every instance that shares `sourceId`. Editing a **linked** source
edits the external file; on commit (or on the next change-check), all linked
smart objects that reference it re-render.

### Linked-source update propagation

A linked smart object watches its `LinkedSourceRef`. On document open, on explicit
"Update Modified Content," or on a (shell-driven) file-watch notification, the
engine compares the stored `contentHash`/`modifiedTimeMs` against the file:

```
checkLinked(so):
    cur = stat(so.source.link().uri)
    if not cur.exists:        mark so.source.link().missing = true; keep last cache; warn
    else if cur.hash != stored.contentHash:
        reload source bytes; update ref; so.invalidate(ALL)   # re-render from new source
```

Relinking (pointing at a new path) and "embed linked" (convert a link into an
embedded source) are commands. A missing link keeps the last cached preview so the
document still composites, and surfaces a warning the shell can show as a badge.

### Smart filters live here, but are owned by the engine

A **smart filter** is just a `SmartFilterEntry` in the stack: the same
[`Filter`](12-filter-engine.md) a destructive filter would use, but stored as
re-editable params on the smart object rather than baked into pixels. Reordering
or deleting an entry, or editing its params/mask, calls `invalidate` and
re-renders. This is why every smart-object mutation routes through a
[command](21-history-undo.md): adding/removing/reordering filters, changing the
transform, replacing contents — all undoable.

## Interactions (Document/Command/Layer/compositor/Tool/RHI + sibling links)

- **[Layer system](03-layer-system.md)** — `SmartObjectLayer` is a `LayerKind`;
  the compositor calls `renderInto` and applies blend/opacity/mask exactly as for
  a pixel layer. Groups can contain smart objects and vice-versa (an embedded
  source *is* a document with its own tree).
- **[Document](01-document-system.md)** — an embedded source is a sub-`Document`;
  the compositor recurses into it. The native format embeds or references sources.
- **[Transform system](10-transform-system.md)** — owns the `Transform`/warp-mesh
  math and the resampler; the smart object stores the transform non-destructively
  and re-applies it from source each render.
- **[Filter engine](12-filter-engine.md)** — supplies the `Filter` interface and
  filter math; smart filters are stored filter entries re-rendered on demand. The
  filter mask reuses [masks](06-masks.md).
- **[Camera Raw](16-camera-raw.md)** — a camera-raw smart object holds
  re-editable `RawSettings`; "Edit Contents" opens the raw editor. The raw filter
  is also available as a smart filter.
- **[Vector & paths](18-vector-paths.md)** — vector smart objects keep geometry
  and rasterize per requested resolution (resolution-independent placement).
- **[File I/O](20-file-io.md)** — the native format round-trips embedded sources,
  linked references (path + hash), the stored transform, the filter stack, and
  masks; PSD maps smart objects to its placed-layer records where structure allows
  ([vision](../00-vision-and-scope.md) targets faithful interchange, not parity).
- **[Masks](06-masks.md)** — both the layer mask and each filter's mask are
  ordinary mask data and machinery.
- **[Command/history](21-history-undo.md)** — every edit (place, transform, add/
  reorder/delete filter, replace/relink/embed, rasterize) is a reversible command;
  `invalidate` + re-render is how the visual result updates.
- **[Performance](22-performance.md)** — the preview cache is reference-counted,
  CoW, RAM-budgeted, and scratch-pageable like layer tiles; cold previews evict
  and re-render on demand.
- **[GPU/RHI](23-gpu-acceleration.md)** — transform resampling and filter passes
  run on the GPU where it pays; the CPU path defines correctness.
- **App shell** — place-file/replace/relink dialogs, the smart-object badge,
  Edit-Contents tab management, and the smart-filter list UI are Qt; the engine
  exposes only data and commands.

## Performance, threading & GPU

- **Lazy, resolution-aware caching.** The preview is rendered only for visible
  tiles at the current scale; zoom-in triggers a higher-res re-render of just
  those tiles. The pristine source can be far larger than RAM and is itself tiled.
- **Background re-render.** When inputs change, the smart object can serve the
  stale-but-valid lower-res cache immediately and re-render the full-res tiles on
  the [worker pool](../01-master-architecture.md#threading-model), swapping them in
  when ready — the canvas stays responsive.
- **Recursion through sub-documents** reuses the same tile compositor and the same
  thread pool; nesting depth is bounded to prevent runaway recursion (see edge
  cases).
- **Shared instances** share both the source and, where the transform matches,
  cache entries; differing transforms render independently but read one source.
- **GPU** handles the resample (one textured draw per tile) and the filter passes;
  the cache stores either CPU tiles or GPU textures depending on the active path.

## Edge cases & failure modes

- **Missing linked file** — keep the last cached preview, set `missing`, composite
  normally, surface a relink prompt. Never block the document on an absent link.
- **Linked file changed underfoot** — detected by hash/mtime; re-renders on next
  check or explicit "Update Modified Content." Concurrent external edits are
  last-writer-wins on the file; the smart object only reads.
- **Recursive placement** (a smart object whose source contains itself) — detect
  via `sourceId` on the placement path and refuse, with a clear error; cap nesting
  depth as a backstop.
- **Enormous source at tiny display scale** — only visible tiles at screen scale
  are rasterized; the native source stays tiled/paged. Conversely, zooming to 100%
  on a huge source rasterizes only the visible window.
- **Transform that collapses to sub-pixel** (scale → 0) — clamp to a minimum
  preview footprint; the *source* is untouched, so scaling back is lossless.
- **Replace Contents with a different aspect/size** — transform and filters are
  retained; the new source is fit per the placement policy (the mockup workflow);
  masks stay in canvas space.
- **Rasterize** — the only lossy operation: bakes the current preview to a pixel
  layer, discards source/transform/filters; recorded as an undoable command that
  snapshots the smart object so undo restores full re-editability.
- **Vector source rasterized too coarse** — re-render at the needed scale on
  zoom; vector stays crisp because geometry, not pixels, is the source of truth.
- **16/32-bit and color space** — the source renders into the document's working
  space at its bit depth (converting on import); filters run at that depth.

## Testing strategy

Headless `pe_core` tests plus golden images:

- **Lossless round-trip transform** — place a source, scale to 5% then back to
  100%; the result must match a fresh render of the source at 100% within
  tolerance (the defining property; contrast a pixel-layer transform which
  degrades). Numeric PSNR assertion + golden.
- **Re-render on demand** — changing transform / a filter param / requested scale
  invalidates and re-renders the expected tiles only; unchanged tiles stay cached
  (assert cache hits).
- **Smart-filter stack** — reorder/disable/delete entries; output matches a golden
  for each permutation; filter mask scopes correctly.
- **Shared instances** — duplicate a smart object, Edit Contents once; all shared
  instances re-render; "via Copy" breaks the link (only one updates).
- **Linked propagation** — mutate the external file; hash/mtime change triggers
  re-render; missing file keeps the last preview and flags `missing`.
- **Replace/Relink/Embed** — transform + filters + mask preserved across Replace;
  relink and embed commands round-trip.
- **Recursion guard** — self-placement is refused; deep nesting is bounded.
- **Undo** — every command (place, transform, filter add/reorder/delete, replace,
  rasterize) restores prior state exactly; rasterize-then-undo restores full
  re-editability.
- **File round-trip** — native format preserves embedded + linked sources,
  transform, filter stack, and masks losslessly ([file I/O](20-file-io.md)).

## Phasing

- **M8 (this doc lands)** — `SmartObjectLayer`; embedded and linked sources;
  file-based raster placement; non-destructive affine + warp transform; the
  smart-filter stack with re-edit/reorder/delete and filter masks; Edit Contents;
  Replace/Relink/Embed; rasterize; convert-layers-to-smart-object; preview cache
  with resolution-aware re-render; native-format round-trip.
- **M8 (with siblings)** — [vector](18-vector-paths.md) smart objects and the full
  [transform](10-transform-system.md) suite (skew/distort/perspective/warp).
- **M9** — [camera-raw](16-camera-raw.md) smart objects (re-editable raw settings,
  raw as a smart filter) land with the full raw pipeline.
- **Continuous** — [GPU](23-gpu-acceleration.md) resample/filter acceleration and
  background re-render tuning; PSD smart-object interchange fidelity improvements.

## Open questions

- **Embedded source dedup** — when many instances share an embedded source, do we
  store one copy keyed by `sourceId`, and how does "New Smart Object via Copy"
  cleanly fork it without bloating the file?
- **Cache scale levels** — a small fixed pyramid of scale levels vs. caching at
  exactly the requested scale: how many levels balance memory against re-render
  churn during smooth zoom?
- **Linked-file watching** — does the engine expose a poll API the shell drives, or
  define an abstract file-watch hook? (Engine stays headless; the shell owns OS
  file-system notifications.)
- **Auto-rasterize guardrails** — should pathological nesting/huge-source cases
  warn the user before a re-render that would page heavily, or just degrade to a
  lower scale silently?
- **Per-instance vs. shared filters** — Photoshop's smart filters are per-instance
  while the *source* is shared; we follow that, but is a "shared filter stack"
  option worth offering for template workflows?

## References (relative links)

- [01 — Master architecture](../01-master-architecture.md) — the `Layer`/`Command`
  contracts and the tile compositor this plugs into.
- [00 — Vision & scope](../00-vision-and-scope.md) — non-destructive-everything;
  smart objects as a named pillar; the "no quality loss on re-scale" promise.
- [Glossary](../glossary.md) — Smart object, Smart filter, Non-destructive, Tile.
- Sibling systems: [10 — Transform system](10-transform-system.md),
  [12 — Filter engine](12-filter-engine.md), [16 — Camera Raw](16-camera-raw.md),
  [18 — Vector & paths](18-vector-paths.md), [20 — File I/O](20-file-io.md),
  [03 — Layer system](03-layer-system.md), [06 — Masks](06-masks.md),
  [22 — Performance](22-performance.md), [23 — GPU acceleration](23-gpu-acceleration.md).
- ADRs: [0003 — tile-based engine](../adr/0003-tile-based-engine.md),
  [0005 — command/history model](../adr/0005-command-history-model.md),
  [0006 — headless core](../adr/0006-headless-core-separation.md),
  [0007 — native document format](../adr/0007-native-document-format.md).
