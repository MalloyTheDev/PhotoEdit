# 05 — Adjustment Layers

> Milestone: M5 · Status: Spec

## Purpose

An **adjustment layer** is a [non-destructive](../glossary.md) layer that applies a
**live color/tonal operation to everything beneath it** at composite time. Unlike a
pixel layer, it owns *no pixels* — it stores **parameters** and a transform function
and, when the [compositor](02-canvas-rendering.md) reaches it, it takes the
accumulated `result` from the layers below and rewrites it. A Curves layer above a
photo bends the tones of the whole stack under it; drag a control point and the
visible image updates because the recipe changed, not because any pixel was
overwritten.

This is the distinction the [vision](../00-vision-and-scope.md) calls
*"non-destructive everything"*. A **destructive** Curves command bakes the new tones
into a layer's pixels and is gone from the recipe; an **adjustment layer** keeps the
parameters and recomputes at render. Because it is still a [Layer](03-layer-system.md),
it carries its own [mask](06-masks.md), [blend mode](04-blend-modes.md), opacity, and
**clipping** to the layer below — so the same adjustment can be localized, faded, or
restricted to one underlying layer. This document specifies the adjustment-layer
node, the `Adjustment` interface its operations satisfy, the concrete parameter
structs, and how live preview, recomputation, and LUT baking keep it fast.

## Requirements (Functional + Non-functional)

**Functional**

- Provide an `AdjustmentLayer` [Layer](03-layer-system.md) kind that, instead of
  contributing pixels, **transforms the accumulated composite** beneath it.
- Ship the full adjustment set, each as stored parameters + an apply function:
  **Brightness/Contrast, Levels, Curves, Exposure, Vibrance, Hue/Saturation, Color
  Balance, Black & White, Photo Filter, Channel Mixer, Gradient Map, Selective
  Color, LUT (Color Lookup / 1D & 3D)**.
- Each adjustment operates on **working-space float pixels** (`Rgbaf`) at the
  document's [bit depth](../glossary.md); correctness is defined by a CPU reference.
- Each adjustment layer supports its **own mask** (where the adjustment applies), a
  **blend mode** and **opacity** (how its result mixes back), and **clipping** to
  the single layer directly below (restrict the effect to that layer's pixels).
- **Live preview**: while a dialog/Properties panel edits parameters, the visible
  result updates by recompositing only the affected dirty tiles.
- **Recomputation on change**: changing parameters invalidates the dirty region
  *below and through* the adjustment layer (everything it covers), not the whole
  document where avoidable.
- Bake expensive parametric curves into **lookup tables** (1D per-channel LUTs; 3D
  cube LUTs for cross-channel ops) so the per-pixel kernel is a table fetch.
- Every parameter change is a reversible [Command](21-history-undo.md); the
  parameters themselves serialize for the native format, scripting, and actions.

**Non-functional**

- `pe_core`, pure C++20: no Qt. Adjustment **dialogs**, the curve editor widget, and
  on-canvas controls (e.g. the Hue/Sat targeted-adjustment eyedropper) live in the
  [app shell](24-ui-workspace.md).
- Cost is proportional to the **dirty tiles** the adjustment covers, never the whole
  canvas, and never the layers *above* it.
- A disabled or fully-masked-out adjustment costs nothing in the composite.
- SIMD/GPU implementations must match the CPU reference within the golden-image
  tolerance; LUT baking must reproduce the analytic function within a stated error.

## Data model (concrete C++ in `namespace pe`)

Adjustments compute over the same `Rgbaf` working pixels the
[blend kernel](04-blend-modes.md) uses, on the engine's tiles (`kTileSize`, `Rect`
from `pe/core/Tile.hpp`).

```cpp
namespace pe {

enum class AdjustmentKind : uint8_t {
    BrightnessContrast = 0, Levels, Curves, Exposure, Vibrance, HueSaturation,
    ColorBalance, BlackAndWhite, PhotoFilter, ChannelMixer, GradientMap,
    SelectiveColor, ColorLookup /* LUT */,
};

// Which channel a per-channel control targets (composite = master/RGB curve).
enum class ToneChannel : uint8_t { Composite = 0, Red, Green, Blue, Alpha };

// The interface every adjustment satisfies: parameters + an apply over a tile of
// working-space float pixels. 'apply' reads and writes the accumulated composite
// in place (straight-alpha Rgbaf); it must not touch alpha unless the adjustment
// is defined to (most are color-only). 'region' lets per-pixel ops know absolute
// coordinates (for Gradient Map dithering, etc.).
class Adjustment {
public:
    virtual ~Adjustment() = default;
    [[nodiscard]] virtual AdjustmentKind kind() const noexcept = 0;
    [[nodiscard]] virtual std::string name() const = 0;

    // Transform the composite tile beneath the adjustment layer, in working space.
    virtual void apply(std::span<Rgbaf> tile, Rect region,
                       const WorkingSpace& space) const = 0;

    // Bake to a fast path where possible: a 1D LUT for per-channel curves, a 3D
    // LUT for cross-channel ops. Returns false if the op is not LUT-reducible
    // (then apply() is the path). Rebuilt when params change.
    [[nodiscard]] virtual bool bakeLut(LutCache&) const { return false; }

    virtual bool serialize(Writer&) const = 0;   // params -> native/actions/scripts
};

// The Layer kind. Owns the Adjustment (its parameters) and the usual Layer state:
// mask, blend mode, opacity, clipping. Contributes no pixels of its own.
class AdjustmentLayer : public Layer {
public:
    [[nodiscard]] const Adjustment& adjustment() const noexcept;
    [[nodiscard]] bool clippedToBelow() const noexcept;  // limit to one layer below

    // Layer contract: an adjustment layer does NOT renderInto a pixel source;
    // the compositor invokes applyTo() on the accumulated backdrop instead.
    void applyTo(std::span<Rgbaf> backdropTile, Rect region,
                 const WorkingSpace& space) const;   // = adjustment().apply(...)
};

// ---- Concrete parameter structs (illustrative; shapes stable, detail may vary) ----

struct BrightnessContrastParams { float brightness = 0.0f;  // [-1,1]
                                  float contrast   = 0.0f;   // [-1,1]
                                  bool  useLegacy  = false; };

// Per-channel input black/gamma/white + output range; bakes to a 1D LUT/channel.
struct LevelsParams {
    struct Channel { float inBlack = 0.0f, inWhite = 1.0f, gamma = 1.0f,
                           outBlack = 0.0f, outWhite = 1.0f; };
    Channel composite;                 // master
    Channel perChannel[3];             // R,G,B (mode-dependent count)
};

// A monotone tone curve per channel, defined by control points, baked to a 1D LUT.
struct CurvesParams {
    struct Point { float x = 0.0f; float y = 0.0f; };   // both in [0,1]
    struct Curve { ToneChannel channel = ToneChannel::Composite;
                   std::vector<Point> points;            // >=2, x-monotonic
                   bool smooth = true; };                // spline vs. linear
    std::vector<Curve> curves;         // composite + any per-channel curves
    // bakeLut() interpolates each curve into a 256/1024/4096-entry table by depth.
};

struct ExposureParams { float exposureStops = 0.0f;   // scene-linear gain
                        float offset        = 0.0f;   // lift
                        float gammaCorrection = 1.0f; };

struct VibranceParams { float vibrance   = 0.0f;   // [-1,1] non-linear, skin-safe
                        float saturation = 0.0f; };// [-1,1] linear

struct HueSatParams {
    struct Band { float hueShift = 0.0f, satScale = 1.0f, lightness = 0.0f;
                  float centerDeg = 0.0f, rangeDeg = 360.0f; }; // master or 6 ranges
    std::vector<Band> bands;           // [0] = master; optional per-color bands
    bool  colorize = false; float colorizeHue = 0, colorizeSat = 0, colorizeLight = 0;
};

struct ColorBalanceParams { float shadows[3]{}, midtones[3]{}, highlights[3]{};
                            bool  preserveLuminosity = true; }; // C-R/M-G/Y-B per range

struct BlackAndWhiteParams { float mix[6]{};   // R,Y,G,C,B,M weights -> gray
                             bool  tint = false; float tintHue = 0, tintSat = 0; };

struct PhotoFilterParams { Rgbaf color{}; float density = 0.25f;
                           bool  preserveLuminosity = true; };

struct ChannelMixerParams { float out[3][4]{}; // per output: R,G,B coeff + constant
                            bool  monochrome = false; };

struct GradientMapParams { GradientRef gradient;     // map luminance -> gradient
                           bool dither = false; bool reverse = false; }; // 1D LUT

struct SelectiveColorParams {  // CMYK deltas per the 9 color/neutral ranges
    struct Range { float c = 0, m = 0, y = 0, k = 0; };
    Range ranges[9]; bool relative = true; };

struct ColorLookupParams {     // 1D or 3D LUT loaded from .cube/.3dl/.look
    LutRef  lut; float amount = 1.0f; bool useInterpolation = true; };

} // namespace pe
```

A layer may carry **both** an `Adjustment` and an ordinary [`Mask`](06-masks.md); the
mask scopes *where* the adjustment applies, exactly as for a pixel layer.

## Behavior & algorithms

**Compositor integration — the twist in the loop.** From the
[master architecture](../01-master-architecture.md#4-the-compositor--tree--tiles)
loop: most layers contribute a `src` that is blended onto `result`. An adjustment
layer instead **transforms `result` in place**, then that transformed copy is mixed
back according to the adjustment layer's own blend mode/opacity/mask:

```
compositeTile(region):
    result = transparent
    for layer in tree bottom→top:
        if not layer.visible: continue
        if layer.isAdjustment:
            adjusted = copy(result)                    # work on the backdrop
            layer.applyTo(adjusted, region, workingSpace)   # in float, in place
            m = layer.mask ? evaluateMask(layer, region) : 1
            result = blend(result, adjusted, layer.blendMode,
                           layer.opacity * m)          # usually Normal@100% => replace
        else:
            src = layer.renderInto(region)
            src = applyMask(src, layer.mask, region)
            result = blend(result, src, layer.blendMode, layer.opacity)
    return result
```

So at default Normal/100%/no-mask the adjustment simply replaces `result` with its
transformed version; with a mask it applies only where the mask reveals; with opacity
< 1 it fades; with a non-Normal blend mode the *adjusted* version blends back over the
original (e.g. a Curves layer set to Luminosity affects tone but not color).

**Clipping to the layer below.** A clipped adjustment must affect only the pixels of
the single layer immediately beneath it, not the whole stack. The compositor handles
this with the same mechanism as [clipping masks](06-masks.md): it composites the base
layer into a scratch buffer, applies the adjustment there, multiplies by the base
layer's alpha, and blends that down — so an exposure tweak clipped to one photo does
not spill onto layers below it.

**Working in float, at depth.** Every `apply` reads `Rgbaf` straight-alpha pixels in
the [working space](15-color-management.md). Tone operations (Levels/Curves/Exposure)
that are physically "light" operate on linear values; perceptual operations
(Hue/Sat, Vibrance) convert to HSL/HSV (or a better model) and back. The reference
implementation does the obvious float math; the production path uses baked tables.

**LUT baking for speed.** Re-evaluating a Catmull-Rom curve or an HSL round-trip per
pixel is wasteful when the mapping is fixed for the whole frame:

- **1D LUTs** — per-channel ops (Levels, Curves, Black & White's gray ramp, Gradient
  Map's luminance ramp, Photo Filter) bake one table per channel sized to depth
  (256 entries at 8-bit, 1024–4096 at 16/32-bit). The kernel becomes
  `out = lut[channel][quantize(in)]` with interpolation at high depth.
- **3D LUTs** — cross-channel ops (Selective Color, Channel Mixer in some modes, and
  of course the **Color Lookup** adjustment itself) bake a 3D cube (e.g. 33³) and the
  kernel is a **trilinear (or tetrahedral) interpolation** into it. Loaded `.cube`
  LUTs feed this path directly.

Tables live in a `LutCache` keyed by the adjustment's parameter hash; they are rebuilt
**only when parameters change** (live editing rebuilds the small table once, then all
dirty tiles fetch from it — cheap).

**Live preview & recomputation.** Editing an adjustment's parameters:

1. The shell's dialog updates `*Params` and issues a *preview* (uncommitted) update.
2. The adjustment re-bakes its LUT (one small table).
3. The dirty region = the document area the adjustment layer covers (its mask bounds
   ∩ canvas, or the clipped base layer's bounds), unioned across the parameter change.
4. The compositor recomposites **only those tiles**, *from the adjustment layer
   upward* (layers below are unchanged and reused from cache).
5. On commit, a single `EditAdjustmentCommand` records old→new params for undo.

Because adjustment layers don't store pixels, their undo is **parameter deltas**, not
tile deltas — tiny and exact. The recomposite is the only cost, and it is dirty-tile
bounded.

## Interactions

- **[Layer system](03-layer-system.md)** — `AdjustmentLayer` is a `Layer` kind; the
  compositor branches on it in the per-tile loop; ordering/clipping are layer-tree
  structure.
- **[Compositor / blend modes](02-canvas-rendering.md), [04](04-blend-modes.md)** —
  the adjustment transforms the backdrop, then blends back via the layer's blend
  mode/opacity using the same `compositeOver` kernel.
- **[Masks](06-masks.md)** — every adjustment layer carries a `Mask` scoping where it
  applies; clipping reuses the clipping-mask mechanism.
- **[Selection system](07-selection-system.md)** — creating an adjustment layer with
  an active selection seeds its mask from the selection.
- **[Color management](15-color-management.md)** — adjustments operate in the working
  space at document depth; tone vs. perceptual ops choose linear vs. encoded values;
  16/32-bit paths arrive in M6 (see [ADR-0004](../adr/0004-color-management.md)).
- **[Channels](19-channels.md)** — per-channel adjustments (Levels/Curves/Channel
  Mixer) read and write specific [channels](19-channels.md); the channel set depends
  on color mode.
- **[Filter engine](12-filter-engine.md)** — adjustments share the working-space
  float tile pipeline, preview/full-quality split, and LUT/SIMD/GPU patterns with
  filters; the difference is that an adjustment is a *live layer*, a filter is applied
  (destructively, or as a smart filter).
- **[Presets & assets](25-presets-assets.md)** — Curves/Levels presets, gradients for
  Gradient Map, and `.cube` LUTs for Color Lookup come from the asset system.
- **[Command/history](21-history-undo.md)** — parameter edits are reversible
  commands recording old/new `*Params` (not tiles).
- **[File I/O](20-file-io.md)** — adjustment kind + parameters + mask + clip flag are
  persisted in the native format and mapped to PSD adjustment layers where structure
  exists.
- **UI touchpoints (app shell)** — the Adjustments panel, the Properties panel curve
  editor, gradient picker, eyedroppers, and on-canvas targeted-adjustment are Qt UI;
  the engine exposes only the parameter structs, the `apply`/`bakeLut` functions, and
  the editing commands.

## Performance, threading & GPU

- The per-pixel kernel is, after baking, a **table lookup** (1D fetch or 3D
  trilinear) — trivially SIMD on CPU and a small shader on the
  [GPU/RHI](23-gpu-acceleration.md) path; it runs inside the same per-tile composite
  job, partitioned so workers never alias.
- LUT rebuild is **O(table size)**, done once per parameter change, not per tile or
  per frame — this is what makes live curve-dragging smooth.
- Recomposite is bounded to the adjustment's covered dirty tiles; layers below are
  served from the tile cache; layers above are untouched by the adjustment's edit.
- For very large adjusted regions, the compositor renders a **reduced-resolution
  preview** first and refines to full-res in the background (the standard preview
  path), so dragging a slider stays interactive on a 30k×30k document.
- Clipped adjustments add one scratch-buffer composite of the base layer's covered
  tiles; still dirty-tile bounded.

## Edge cases & failure modes

- **Adjustment over transparency** — where the backdrop alpha is 0 there is no color
  to adjust; most adjustments leave fully-transparent pixels unchanged (operate on
  un-premultiplied color; skip `a==0`).
- **Bottom-of-stack adjustment** — nothing beneath it; the backdrop is transparent, so
  it is a (visible) no-op until content appears below — never an error.
- **Out-of-gamut / extreme parameters** — Exposure and Curves can push values outside
  `[0,1]`; at 32-bit-float these are preserved (HDR), at 8/16-bit they clamp on
  output. The float pipeline carries the overshoot until quantization.
- **Non-monotonic Curves input** — control points are kept x-monotonic by
  construction; a degenerate curve (two points at same x) is sanitized when baking.
- **LUT precision at 8-bit** — a 256-entry 1D LUT is exact for 8-bit input; at 16/32-
  bit the kernel interpolates between entries (or uses a larger table) to avoid
  banding — validated against the analytic reference.
- **3D LUT amount < 1 / out-of-cube values** — `amount` lerps toward identity;
  values outside the cube clamp to the boundary (or extrapolate per LUT type).
- **Mask fully black** — the adjustment contributes nothing; the compositor skips its
  apply for those tiles (zero cost).
- **Color-mode mismatch** (e.g. a Channel Mixer authored for RGB on a Gray document)
  — the adjustment adapts to the [channel set](19-channels.md) or is disabled with a
  warning; never crashes.

## Testing strategy

Headless `pe_core` unit + golden tests:

- **Reference vs. baked** — each adjustment's analytic `apply` matches its baked-LUT
  output within tolerance across a value sweep (1D and 3D).
- **Golden image** — known content + known adjustment params → known composite, for
  every adjustment kind, at Normal/100% and with a mask, blend mode, opacity, and
  clipping variant.
- **Identity** — default parameters (e.g. Curves = straight line, Hue/Sat = zero)
  produce a composite identical to no adjustment layer.
- **Non-destruction** — applying then deleting an adjustment layer restores the exact
  prior composite; editing params then undoing restores byte-exact (the source pixels
  never changed).
- **Clipping** — a clipped adjustment affects only the base layer's pixels; a pixel
  probe below the base layer is unchanged.
- **Order independence where claimed** — independent per-channel ops compose; order-
  dependent stacks (e.g. Curves then Hue/Sat) match a reference sequence.
- **Depth** — 8/16/32-bit float paths match the reference within depth-appropriate
  tolerance; 32-bit preserves out-of-range overshoot.
- **Undo/serialize** — `*Params` round-trip through serialize/deserialize and through
  the native-format writer.

## Phasing

- **M5 (this doc lands)** — `AdjustmentLayer` + `Adjustment` interface; the full
  adjustment set above; per-channel 1D and cross-channel 3D LUT baking; own mask,
  blend mode, opacity, clipping; live preview + dirty-tile recompute; parameter
  commands. Operates in the working space at **8-bit** (the M5 storage path).
- **M6** — real **16-bit / 32-bit-float** adjustment math riding in with
  [color management](15-color-management.md); larger/interpolated LUTs; linear-vs-
  encoded correctness for tone ops; per-channel ops aware of the
  [channel set](19-channels.md).
- **M7** — adjustment layers (kind + params + mask) round-trip through the native
  format and map to/from PSD adjustment layers.
- **M5+ (cross-cutting)** — [GPU](23-gpu-acceleration.md) LUT kernels where they pay;
  reduced-res preview refinement.

## Open questions

- Default working-space domain per adjustment: which operate in linear light vs.
  display-encoded by default, and is that user-overridable per layer?
- 3D LUT cube resolution and interpolation (trilinear vs. tetrahedral) — fixed, or
  adaptive to the loaded LUT and the quality/preview mode?
- Should Vibrance/Hue-Sat use a perceptual model (e.g. Oklab/Oklch) rather than HSL
  for more pleasing skin-tone behavior, at some cost in PSD-parity?
- Do we expose a generic "user 3D LUT layer" (Color Lookup) and a curve layer as the
  same node with different param payloads, or keep distinct kinds for clarity? (Data
  model leans toward distinct kinds, shared interface.)

## References

- [03 — Layer system](03-layer-system.md) — `AdjustmentLayer` as a `Layer` kind.
- [02 — Canvas & rendering](02-canvas-rendering.md) · [04 — Blend modes](04-blend-modes.md)
  — the compositor twist and blend-back kernel.
- [06 — Masks](06-masks.md) — per-adjustment mask and clipping mechanism.
- [07 — Selection system](07-selection-system.md) — seeding a mask from a selection.
- [15 — Color management](15-color-management.md) — working space, depth, linear vs.
  encoded; [ADR-0004](../adr/0004-color-management.md).
- [19 — Channels](19-channels.md) — per-channel adjustments and the mode-dependent
  channel set.
- [12 — Filter engine](12-filter-engine.md) — shared float tile / preview / SIMD-GPU
  patterns.
- [25 — Presets & assets](25-presets-assets.md) — gradients and `.cube` LUTs.
- [Master architecture](../01-master-architecture.md) · [Glossary](../glossary.md)
  (Adjustment layer, Non-destructive, Working space, Bit depth).
