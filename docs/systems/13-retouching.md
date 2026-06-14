# 13 — Retouching & Restoration

> Milestone: M9 · Status: Spec

## Purpose

Retouching tools repair and clean up imagery: clone, heal, patch, content-aware
fill, remove, red-eye, and the dodge/burn/sponge family. They range from simple
(clone = sample-and-copy) to hard (content-aware = analyze, infer, synthesize,
and blend new content that preserves structure). Modern removal blends into the
[generative AI](14-generative-ai.md) system; this spec covers the classical,
on-device algorithms and the sampling/brush model they share with the
[brush engine](08-brush-engine.md).

## Requirements

**Functional**

- **Clone Stamp**: copy pixels from a sampled source point to the destination,
  following the brush; aligned and non-aligned modes; sample current/all layers.
- **Healing Brush / Spot Healing**: copy *texture* from a source but *match* the
  destination's color and luminance, blending seamlessly at the edges.
- **Patch**: select a region and drag it to a source area; heal-blend the result.
- **Content-Aware Fill / Remove**: fill a selection/stroke by synthesizing from
  surrounding content.
- **Red Eye**: detect and desaturate/darken red pupils.
- **Dodge / Burn / Sponge**: local lighten/darken/saturation adjustments.
- All operate within the active [selection](07-selection-system.md) and emit
  [commands](21-history-undo.md); most can target a separate retouch layer.

**Non-functional**

- `pe_core`, no Qt; tile-aware; works at document bit depth in the working
  [color space](15-color-management.md).
- Heal/fill quality is the priority; content-aware runs on
  [workers](22-performance.md) with progress + cancel.

## Data model

```cpp
namespace pe {

struct CloneSource {
    LayerId layer;            // or "sampled merged"
    PointF  anchor;           // source point set on alt-click
    bool    aligned = true;   // keep source offset locked to the stroke
    Matrix3 transform;        // optional rotate/scale of the clone source
};

struct HealParams { float diffusion = 0.5f; bool sampleAllLayers = false; };

enum class RetouchKind { Clone, Heal, SpotHeal, Patch, ContentAwareFill,
                         Remove, RedEye, Dodge, Burn, Sponge };

struct DodgeBurnParams { enum Range { Shadows, Midtones, Highlights } range;
                         float exposure = 0.1f; bool protectTones = true; };

} // namespace pe
```

## Behavior & algorithms

**Clone** is a brush whose source is another image region:

```
for each dab d along the stroke:
    srcPos = aligned ? d - (firstDab - anchor) : anchor + (d - strokeStart)
    stamp(sample(source, srcPos), dest=d, brushFalloff)
```

**Healing** copies texture but reconciles tone/color with the destination using
gradient-domain (Poisson) blending: solve for an image whose *gradients* match the
source patch but whose *boundary* matches the surrounding destination, so the
seam disappears:

```
heal(region):
    g = gradient(sourcePatch)
    solve ∇²f = div(g)  subject to f = destination on ∂region   # Poisson
    write f into region
```

**Content-Aware Fill / Remove** synthesizes plausible content for a hole using
patch-based synthesis (PatchMatch): iteratively find, for each target patch, the
best-matching source patch from the surrounding valid area, vote, and propagate —
then heal-blend the boundary. Structure is preserved by matching gradients/edges,
not just colors. The AI [Remove tool](14-generative-ai.md) can substitute a model
for this synthesis step behind the same UX.

**Dodge/Burn/Sponge** apply a masked, tone-ranged multiply/curve to luminance (or
saturation for sponge), accumulated by the brush.

## Interactions

- [Brush engine](08-brush-engine.md): clone/heal/dodge/burn are brushes with a
  special per-dab operation.
- [Selection](07-selection-system.md): defines the fill/patch region and gates all
  tools.
- [Generative AI](14-generative-ai.md): content-aware remove can route to a
  generative provider; results land as a [layer](03-layer-system.md).
- [History](21-history-undo.md): every operation is an undoable tile-delta command.

## Performance, threading & GPU

- Clone/heal dabs are cheap and run inline with the stroke.
- Poisson solve and PatchMatch run on [workers](22-performance.md) over the region
  with progress and cancellation; tile-aware so memory stays bounded.
- GPU acceleration is optional for the iterative solvers (later).

## Edge cases & failure modes

- Clone source off-canvas or from a hidden layer → clamp/skip with feedback.
- Heal across a strong edge → boundary bleeding; restrict patch to the selection
  and respect edges.
- Content-aware with too little surrounding context → poor synthesis; warn and
  suggest enlarging the sample area or using generative fill.
- 8-bit banding in smooth gradients after Poisson blend → solve in 16/32-bit.

## Testing strategy

- Unit: aligned-clone offset math; dodge/burn tone-range masking.
- Golden-image: heal a known blemish on a reference texture; content-aware fill a
  known hole and compare structural similarity (SSIM) to a reference within
  tolerance (deterministic seed).
- Selection-gating test: retouch outside the selection is a no-op.

## Phasing

- **M9 early**: clone stamp, spot heal, healing brush, dodge/burn/sponge, red-eye.
- **M9 later**: patch, content-aware fill/remove (PatchMatch), AI-routed remove.

## Open questions

- PatchMatch iteration budget vs quality defaults; deterministic seeding for tests.
- When to auto-prefer the AI provider over classical synthesis.
- Separate retouch-layer workflow defaults (non-destructive by default?).

## References (relative links)

- [01 — Master architecture](../01-master-architecture.md) — `Command`, tiles.
- [Glossary](../glossary.md) — Brush dab, Non-destructive, Selection.
- Sibling systems: [08 — Brush engine](08-brush-engine.md),
  [07 — Selection](07-selection-system.md), [14 — Generative AI](14-generative-ai.md),
  [06 — Masks](06-masks.md), [21 — History & undo](21-history-undo.md),
  [22 — Performance](22-performance.md), [15 — Color management](15-color-management.md).
- ADRs: [0005 — command/history model](../adr/0005-command-history-model.md).
