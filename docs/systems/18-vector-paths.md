# 18 — Vector & Path System

> Milestone: M8 · Status: Spec

## Purpose

PhotoEdit is raster-first but has real vector capability: the Pen tool, Bézier
paths, shape layers, vector masks, and custom shapes. Paths give resolution-
independent geometry that stays crisp at any scale until rasterized at display or
export time. This system defines the path data model, editing, rasterization, and
the conversions between paths, [selections](07-selection-system.md), and
[vector masks](06-masks.md).

## Requirements

**Functional**

- Bézier paths: anchor points with in/out handles, straight and curved segments,
  open and closed subpaths, multiple subpaths per path.
- Pen tool (add/insert/delete anchors, drag handles), Path Selection (whole path),
  Direct Selection (anchors/handles).
- Shape layers: a path with a fill (solid/gradient/pattern) and a stroke style;
  rasterized non-destructively at render.
- Vector masks (path-backed layer masks), custom shape library.
- Conversions: path → selection, selection → path (trace), stroke path with a
  [brush](08-brush-engine.md), fill path.

**Non-functional**

- `pe_core`, no Qt; the on-canvas handle UI lives in the
  [app shell](24-ui-workspace.md).
- Rasterization is anti-aliased, tile-aware, and color-correct.

## Data model

```cpp
namespace pe {

struct AnchorPoint {
    PointF position;
    PointF inHandle;     // relative control point (incoming)
    PointF outHandle;    // relative control point (outgoing)
    bool   smooth;       // symmetric handles
};

struct SubPath { std::vector<AnchorPoint> anchors; bool closed = false; };
struct Path    { std::vector<SubPath> subPaths; };

struct StrokeStyle {
    float width = 1.0f;
    Rgbaf color{0,0,0,1};
    enum Join { Miter, Round, Bevel } join = Miter;
    enum Cap  { Butt, RoundCap, Square } cap = Butt;
    std::vector<float> dash;
    enum Align { Center, Inside, Outside } align = Center;
};

struct ShapeLayer {            // a Layer kind
    Path path;
    FillStyle fill;            // solid / gradient / pattern, or none
    StrokeStyle stroke;        // or none
    // rasterized cache (tiled), invalidated on path/style edit
};

struct VectorMask { Path path; bool inverted = false; };

} // namespace pe
```

## Behavior & algorithms

**Cubic Bézier evaluation** of a segment between anchors `a`→`b`:

```
p0 = a.position; p1 = a.position + a.outHandle
p2 = b.position + b.inHandle; p3 = b.position
B(t) = (1-t)³p0 + 3(1-t)²t·p1 + 3(1-t)t²·p2 + t³p3,  t∈[0,1]
```

**Rasterization**: flatten each segment adaptively to line segments (subdivide
until flatness < tolerance), then scanline-fill with a coverage-based anti-aliasing
accumulator (non-zero or even-odd winding) into the layer's tiles. Strokes are
rasterized by expanding the path to an outline (offset by half width, joins/caps)
and filling that.

**Path ↔ selection**: filling a path produces a coverage buffer = a
[selection mask](07-selection-system.md); tracing a selection's boundary fits
Béziers to the mask contour to produce a path.

**Editing**: hit-test anchors/handles/segments in path space (transformed by the
view); the Pen/Direct-Selection tools mutate the `Path` and commit a
[command](21-history-undo.md); the cache re-rasterizes the dirty region only.

## Interactions

- [Masks](06-masks.md): vector masks are path-backed and rasterized into mask
  coverage; non-destructive and re-editable.
- [Layer system](03-layer-system.md): `ShapeLayer` is a layer kind.
- [Selection](07-selection-system.md): path↔selection conversions.
- [Text](17-text-typography.md): convert-to-shape produces paths; type-on-path.
- [Transform](10-transform-system.md): paths transform exactly (no resampling) by
  transforming their control points.
- [Brush engine](08-brush-engine.md): stroke-path runs a brush along the curve.

## Performance, threading & GPU

- Paths are tiny (control points); only rasterization costs, and only on edit.
- Flattening tolerance scales with zoom so curves stay smooth without overwork.
- Rasterization is tile-parallel; a [GPU](23-gpu-acceleration.md) path-fill is a
  later optimization.

## Edge cases & failure modes

- Self-intersecting/winding ambiguity → explicit fill rule.
- Degenerate (zero-length) segments and coincident anchors → skip safely.
- Very thin/huge strokes → join/cap correctness at extremes.
- Open-path fill → implicitly close for fill, leave open for stroke.

## Testing strategy

- Unit: Bézier evaluation against known points; flattening within tolerance;
  transform of control points equals transform of sampled curve points.
- Golden-image: fill/stroke reference shapes per winding rule and join/cap.
- Conversion: rectangle path → selection equals a rectangular marquee selection.

## Phasing

- **M8 early**: paths, Pen/selection tools, shape layers (fill/stroke), vector
  masks, AA fill, path→selection.
- **M8 later**: custom shapes library, selection→path tracing, dashed/aligned
  strokes, stroke-path-with-brush.

## Open questions

- Curve-fitting algorithm for selection→path tracing and its tolerance defaults.
- Gradient/pattern fill model shared with [fill layers](03-layer-system.md).

## References (relative links)

- [Glossary](../glossary.md) — Mask, Selection, Non-destructive.
- Sibling systems: [06 — Masks](06-masks.md), [03 — Layer system](03-layer-system.md),
  [07 — Selection](07-selection-system.md), [17 — Text](17-text-typography.md),
  [10 — Transform](10-transform-system.md), [08 — Brush engine](08-brush-engine.md),
  [09 — Tool system](09-tool-system.md).
- ADRs: [0003 — tile-based engine](../adr/0003-tile-based-engine.md).
