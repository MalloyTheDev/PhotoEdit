# 10 — Transform System

> Milestone: M3 (basic move/scale/rotate) · M8 (full distort/perspective/warp/puppet) · Status: Spec

## Purpose

The transform system repositions and reshapes layer content — move, scale,
rotate, skew, distort, perspective, warp, and puppet warp — and is the geometry
engine behind Free Transform, the [Move tool](09-tool-system.md), and
[smart object](11-smart-objects.md) placement. It is where image quality is won
or lost: good resampling keeps edges crisp, bad math produces blur and jaggies.
For pixel layers a transform resamples pixels (destructive); for smart objects it
is stored as a matrix and re-applied to the preserved source (non-destructive).

## Requirements

**Functional**

- Affine transforms: translate, scale, rotate, shear/skew, and free combinations.
- Projective (perspective) transforms via a 3×3 homogeneous matrix.
- Mesh-based deformation: warp (grid), and puppet warp (pin-driven).
- Content-aware scale (seam-carve-style protect/scale) as a later mode.
- Interactive Free Transform: handles, numeric entry, pivot point, snapping;
  commit as a single [command](../01-master-architecture.md#2-command--one-reversible-edit).
- Resampling modes: nearest, bilinear, bicubic, and a high-quality (Lanczos)
  filter; the default is configurable per operation.
- Non-destructive transforms for smart objects (store matrix; never bake).

**Non-functional**

- `pe_core`, no Qt; the handle UI lives in the [app shell](24-ui-workspace.md).
- Tile-aware: transform a region by gathering source tiles plus an apron so
  interpolation kernels have neighbor context; produce destination tiles.
- The CPU reference defines correctness; a [GPU](23-gpu-acceleration.md) path for
  interactive previews must match within tolerance.
- Operate in the working [color space](15-color-management.md) at document bit
  depth; resample premultiplied to avoid edge halos.

## Data model

```cpp
namespace pe {

enum class Interpolation { Nearest, Bilinear, Bicubic, Lanczos };

// Row-major 3x3 homogeneous matrix covering affine + projective transforms.
struct Matrix3 {
    float m[9] = {1,0,0, 0,1,0, 0,0,1};
    static Matrix3 translate(float tx, float ty);
    static Matrix3 scale(float sx, float sy);
    static Matrix3 rotate(float radians);
    static Matrix3 shear(float kx, float ky);
    Matrix3 operator*(const Matrix3&) const;
    Matrix3 inverse() const;
    // Map a point (homogeneous divide for perspective).
    PointF map(PointF) const;
};

struct Transform {
    Matrix3 matrix;
    Interpolation interpolation = Interpolation::Bicubic;
};

// Mesh deformation for warp/puppet (a grid of source->dest control points).
struct WarpMesh {
    int cols = 4, rows = 4;
    std::vector<PointF> source;   // rest positions
    std::vector<PointF> dest;     // deformed positions
};

struct PuppetPin { PointF rest; PointF current; float rotation = 0.0f; };

} // namespace pe
```

## Behavior & algorithms

**Inverse (backward) mapping.** Resampling iterates destination pixels and pulls
from the source via the inverse matrix, so every output pixel is covered exactly
once with no holes:

```
for each dest pixel d in dirtyDest:
    s = matrix.inverse().map(d + 0.5)      # sub-pixel source position
    out[d] = sample(source, s, interpolation)   # premultiplied sampling
```

`sample` reads the kernel's footprint (1 px nearest, 2×2 bilinear, 4×4 bicubic,
wider Lanczos) from source tiles, fetching neighbor tiles as needed (apron).

**Free Transform** accumulates an interactive `Matrix3` from handle drags around a
pivot; the document shows a GPU preview each frame and bakes one resampled
`TransformCommand` on commit (pixel layer) or updates the stored matrix (smart
object).

**Warp / puppet warp** interpolate a displacement field from the mesh / pins
(thin-plate-spline or bilinear patch interpolation), then backward-map through it.

**Downscaling** must prefilter (mipmaps or area averaging) to avoid aliasing;
upscaling relies on the interpolation kernel; Lanczos is the quality default for
large scale factors.

## Interactions

- [Smart objects](11-smart-objects.md): own a `Transform` applied to the source
  on render — repeated re-transform without quality loss.
- [Tool system](09-tool-system.md): Move and Free Transform tools drive it.
- [Filter engine](12-filter-engine.md): shares the apron/halo tile-context model.
- [Layer system](03-layer-system.md): each layer carries a `Transform`.
- [Selection](07-selection-system.md): "Transform Selection" reuses the same math
  on the selection mask.

## Performance, threading & GPU

- Destination tiles are independent → resampling is embarrassingly parallel across
  the [worker pool](22-performance.md).
- Interactive previews run on the [GPU](23-gpu-acceleration.md) (a textured-quad
  draw for affine; a fragment shader for projective/warp); the final bake runs at
  full quality on CPU (or a verified GPU compute path).
- Only the transformed layer's dirty region recomposites.

## Edge cases & failure modes

- Near-singular / degenerate matrices (zero scale) → reject or clamp.
- Perspective points crossing the horizon (w ≤ 0) → clip affected pixels.
- Extreme upscaling of a pixel layer → warn (lossy); recommend smart object.
- Premultiplied vs straight alpha mismatch → colored fringes; always resample
  premultiplied.
- Repeated destructive transforms compound error → encourage smart objects.

## Testing strategy

- Unit: matrix compose/inverse/map identities; round-trip a transform and its
  inverse recovers positions within tolerance.
- Golden-image: rotate/scale/perspective a reference tile per interpolation mode;
  compare against committed references within tolerance.
- Property: an integer translate is exactly a pixel copy (no resampling error).
- Downscale aliasing test (high-frequency target) stays below an energy threshold.

## Phasing

- **M3**: affine move/scale/rotate of a pixel layer with bilinear/bicubic; basic
  Free Transform; tile-aware resampling.
- **M8**: skew/distort/perspective, warp mesh, puppet warp, Lanczos, content-aware
  scale; non-destructive smart-object transforms.

## Open questions

- Default interpolation per operation (bicubic vs Lanczos) and when to auto-switch
  on large scale factors.
- Whether puppet warp uses TPS or a faster as-rigid-as-possible solver.
- Mipmap cache policy for repeated smart-object downscales.

## References (relative links)

- [01 — Master architecture](../01-master-architecture.md) — `Command`, tiles.
- [Glossary](../glossary.md) — Tile, Smart object, Non-destructive.
- Sibling systems: [11 — Smart objects](11-smart-objects.md),
  [09 — Tool system](09-tool-system.md), [12 — Filter engine](12-filter-engine.md),
  [03 — Layer system](03-layer-system.md), [23 — GPU acceleration](23-gpu-acceleration.md),
  [22 — Performance](22-performance.md).
- ADRs: [0003 — tile-based engine](../adr/0003-tile-based-engine.md).
