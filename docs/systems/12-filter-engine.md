# 12 — Filter Engine & Smart Filters

> Milestone: M5 (destructive filters + engine) · M8 (smart filters) · Status: Spec

## Purpose

The filter engine runs image-processing operations — blur, sharpen, noise,
distort, stylize, render, pixelate, liquify, lens correction, and more — over a
region of pixels, with fast previews and full-quality final renders. A
**destructive filter** rewrites pixels (as a [command](21-history-undo.md)); a
**smart filter** is the same operation applied non-destructively to a
[smart object](11-smart-objects.md), re-editable, reorderable, and removable
after the fact. This is one of the most compute-heavy systems, so it is built
tile-aware, multithreaded, and color-correct from the start.

## Requirements

**Functional**

- Filter families: blur (Gaussian, box, motion, lens), sharpen (unsharp mask,
  smart sharpen), noise (add/reduce), distort, stylize, render (clouds,
  gradient), pixelate, plus later liquify, lens correction, vanishing point, and
  neural/AI filters.
- A uniform `Filter` interface so built-ins and [plugins](29-plugin-extension.md)
  are interchangeable.
- Per-filter parameter model that drives both a UI and serialization.
- Live preview (reduced-resolution or viewport-only) and a full-quality render.
- Mask support: a filter is constrained by the active [selection](07-selection-system.md)
  and/or a [filter mask](06-masks.md).
- Smart filters: stored on a smart object as an ordered, editable stack.

**Non-functional**

- `pe_core`, no Qt. Correct at 8/16/32-bit in the working
  [color space](15-color-management.md); many filters belong in linear light.
- Tile-aware with an **apron/halo**: kernels that read neighbors fetch
  surrounding tile context so results are seamless across tile boundaries.
- CPU reference defines correctness; [SIMD/GPU](23-gpu-acceleration.md) paths
  match within tolerance.

## Data model

```cpp
namespace pe {

// Parameters are a typed bag the UI renders and the engine serializes.
struct FilterParam { std::string key; ParamValue value; };   // float/int/bool/enum/curve
using FilterParams = std::vector<FilterParam>;

class Filter {
public:
    virtual ~Filter() = default;
    virtual std::string id() const = 0;          // stable, e.g. "blur.gaussian"
    virtual std::string displayName() const = 0;
    virtual FilterParams defaultParams() const = 0;

    // How far the kernel reads beyond the output region (apron in pixels), so the
    // engine can fetch enough source tiles for seamless tiling.
    virtual int apron(const FilterParams&) const { return 0; }

    // Process source -> dest for one tile region, in the working space.
    virtual void apply(const TileView& src, TileView& dst,
                       const FilterParams&, const FilterContext&) const = 0;
};

// A non-destructive filter instance on a smart object.
struct SmartFilter {
    std::string filterId;
    FilterParams params;
    BlendMode blendMode = BlendMode::Normal;
    float opacity = 1.0f;
    Mask mask;            // optional filter mask
    bool enabled = true;
};

} // namespace pe
```

## Behavior & algorithms

**Convolution / separable blur.** A Gaussian blur is separable: blur horizontally
then vertically, O(2r) instead of O(r²). Each pass reads `apron = ceil(3σ)` pixels
of neighbor context:

```
gaussianBlur(src, σ):
    k = gaussianKernel1D(σ)
    tmp = convolveRows(src_with_apron, k)
    return convolveCols(tmp, k)
```

**Unsharp mask (sharpen).**

```
blurred = gaussianBlur(src, radius)
detail  = src - blurred
result  = src + amount * detail        # thresholded to avoid amplifying noise
```

**Smart filter stack** is evaluated as part of rendering the smart object:

```
pixels = renderSource(smartObject)        # at needed resolution
for f in smartFilters (bottom→top, enabled):
    out = applyFilter(f.filterId, pixels, f.params)
    pixels = blend(pixels, out, f.blendMode, f.opacity, f.mask)
return pixels
```

Editing any filter's params invalidates the smart object's cached preview and the
[dirty region](22-performance.md) below it.

**Preview vs final.** Previews render the visible viewport (or a downscaled proxy)
for responsiveness; on commit/zoom the engine schedules the full-resolution render
on [worker threads](22-performance.md).

## Interactions

- [Smart objects](11-smart-objects.md) own the smart-filter stack.
- [Selection](07-selection-system.md)/[masks](06-masks.md) constrain application.
- [History](21-history-undo.md): destructive filters are tile-delta commands.
- [Transform](10-transform-system.md): shares the apron/tile-context model.
- [GPU](23-gpu-acceleration.md): blur/sharpen/convolution run as compute shaders.
- [Retouching](13-retouching.md) and [generative AI](14-generative-ai.md) reuse
  the engine's region/preview plumbing.

## Performance, threading & GPU

- Tiles are processed in parallel; wide kernels fetch an apron of neighbor tiles.
- Separable and box-accumulator algorithms keep large-radius blur cheap.
- GPU compute is the default for data-parallel filters; CPU SIMD is the fallback
  and the correctness reference.
- Large radii on huge documents use downsampled approximation for preview.

## Edge cases & failure modes

- Apron at document/layer edges → clamp/mirror/transparent edge policy per filter.
- 8-bit precision loss on iterated filters → compute in 16/32-bit internally.
- Non-linear vs linear light mismatch → blur halos; document each filter's space.
- Huge-radius blur OOM → tile + downsample strategy, never allocate full frame.

## Testing strategy

- Unit: separable Gaussian equals reference 2D convolution within tolerance;
  zero-radius is identity; unsharp with amount 0 is identity.
- Golden-image: each filter on a reference image per bit depth.
- Tiling seam test: filtering a tiled image equals filtering it whole (apron
  correctness).

## Phasing

- **M5**: engine, params/preview/full-render, masking, core blur/sharpen/noise/
  pixelate/render filters (CPU, then GPU for blur/sharpen).
- **M8**: smart filters on smart objects; lens correction, liquify, distort/
  stylize families; neural/AI filters via the [AI](14-generative-ai.md) provider.

## Open questions

- Per-filter declared working space (linear vs encoded) — table or per-filter flag.
- Plugin filter ABI (see [29](29-plugin-extension.md)).
- Liquify mesh representation shared with [warp](10-transform-system.md)?

## References (relative links)

- [01 — Master architecture](../01-master-architecture.md) — `Command`, tiles, RHI.
- [Glossary](../glossary.md) — Smart filter, Non-destructive, Tile, Dirty region.
- Sibling systems: [11 — Smart objects](11-smart-objects.md),
  [06 — Masks](06-masks.md), [07 — Selection](07-selection-system.md),
  [21 — History & undo](21-history-undo.md), [23 — GPU](23-gpu-acceleration.md),
  [15 — Color management](15-color-management.md), [13 — Retouching](13-retouching.md),
  [29 — Plugins](29-plugin-extension.md).
- ADRs: [0003 — tile-based engine](../adr/0003-tile-based-engine.md).
