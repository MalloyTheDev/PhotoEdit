# 04 — Blend Modes

> Milestone: M1 · Status: Spec

## Purpose

A [blend mode](../glossary.md) is the per-pixel math that controls how a
[layer](03-layer-system.md) mixes with what is below it. It is the innermost
kernel of the [compositor](../01-master-architecture.md#4-the-compositor--tree--tiles):
for every pixel of every dirty tile, the blend mode combines the **backdrop**
(`b`, the accumulated result so far) with the **source** (`s`, the layer's
rendered pixel) and the source is composited "over" the backdrop with the layer's
opacity.

The **separable** modes — Normal, Multiply, Screen, Overlay, Darken, Lighten,
Color Dodge, Color Burn, Hard Light, Soft Light, Difference, Exclusion — are
**already implemented** in `pe_core` (`src/core/include/pe/core/BlendMode.hpp`,
`src/core/src/BlendMode.cpp`) as the M0 correctness reference. This document
records the math for each, documents the Porter-Duff "over" composite that
`compositeOver` already performs (with layer opacity folded in), and specifies the
**non-separable** modes still to add — **Hue, Saturation, Color, Luminosity** —
which operate on whole pixels via luminance/HSY, giving the `setLum`, `setSat`,
and `clipColor` algorithms. It also states the reference-vs-SIMD/GPU correctness
contract that golden-image tests enforce.

## Requirements

**Functional**

- Provide `blendChannel(mode, b, s)` for every **separable** mode: a scalar
  function on one normalized channel, with `b`, `s` in `[0,1]`.
- Provide `compositeOver(mode, backdrop, source, opacity)`: composite a
  straight-alpha source over a straight-alpha backdrop using the mode and a layer
  opacity in `[0,1]`, returning straight alpha. (This is the per-pixel kernel.)
- Add the four **non-separable** modes (Hue, Saturation, Color, Luminosity) that
  operate on the whole RGB triple via a luminance model, integrated through the
  same `compositeOver` "where backdrop is opaque" weighting.
- Stable, human-readable names per mode (`blendModeName`).
- The on-disk `BlendMode` enum values are **stable and append-only** — already a
  comment in the header; new modes append, existing values never reorder/reuse.

**Non-functional**

- Pure C++20 in `pe_core`; no Qt ([ADR-0006](../adr/0006-headless-core-separation.md)).
- Math is done in **float** on normalized values to avoid banding/rounding
  (`Rgbaf`); 8-bit storage converts in/out at the boundary (`Color.hpp`).
- The reference implementation is written for **clarity**; the production path is
  SIMD on CPU and a shader on the [GPU/RHI](23-gpu-acceleration.md), with
  **identical semantics**, validated against the reference by tests
  ([ADR-0002](../adr/0002-gpu-abstraction.md)).
- Premultiplied alpha is the correct space for the alpha composite; the kernel
  composites premultiplied internally and returns straight alpha
  (see [glossary: premultiplied alpha](../glossary.md)).

## Data model

The enum and free functions already exist in `pe/core/BlendMode.hpp`. The shape,
reproduced for reference (values are the on-disk format — append-only):

```cpp
namespace pe {

enum class BlendMode : uint8_t {
    Normal = 0,
    Multiply = 1,
    Screen = 2,
    Overlay = 3,
    Darken = 4,
    Lighten = 5,
    ColorDodge = 6,
    ColorBurn = 7,
    HardLight = 8,
    SoftLight = 9,
    Difference = 10,
    Exclusion = 11,
    // Separable modes end here. Non-separable modes append next:
    Hue = 12,         // (to add)
    Saturation = 13,  // (to add)
    Color = 14,       // (to add)
    Luminosity = 15,  // (to add)
    Count
};

[[nodiscard]] const char* blendModeName(BlendMode) noexcept;

// Separable: one channel of backdrop b vs source s, both in [0,1].
[[nodiscard]] float blendChannel(BlendMode, float b, float s) noexcept;

// The per-pixel compositor kernel: straight-alpha source over straight-alpha
// backdrop, with layer opacity in [0,1]; returns straight alpha.
[[nodiscard]] Rgbaf compositeOver(BlendMode, Rgbaf backdrop, Rgbaf source,
                                  float opacity) noexcept;

// (to add) Non-separable kernel: operates on the whole RGB triple.
[[nodiscard]] Rgbaf blendNonSeparable(BlendMode, Rgbaf b, Rgbaf s) noexcept;

} // namespace pe
```

`compositeOver` is a free function precisely because it is stateless per pixel and
must be trivially vectorizable; it carries no document or layer state — the
[layer system](03-layer-system.md) supplies `mode`, `opacity`, and the already
masked/effected `source`.

## Behavior & algorithms

### Separable modes (implemented — the reference)

Each is a scalar function of one backdrop channel `b` and one source channel `s`,
both in `[0,1]`. These are exactly the W3C/Photoshop separable formulas as coded
in `BlendMode.cpp`:

| Mode | `blend(b, s)` |
| --- | --- |
| **Normal** | `s` |
| **Multiply** | `b · s` |
| **Screen** | `b + s − b·s` |
| **Overlay** | `b ≤ 0.5 ? 2·b·s : 1 − 2·(1−b)·(1−s)`  (= HardLight(s, b)) |
| **Darken** | `min(b, s)` |
| **Lighten** | `max(b, s)` |
| **Color Dodge** | `b ≤ 0 → 0;  s ≥ 1 → 1;  else clamp01(b / (1 − s))` |
| **Color Burn** | `b ≥ 1 → 1;  s ≤ 0 → 0;  else 1 − clamp01((1 − b) / s)` |
| **Hard Light** | `s ≤ 0.5 ? 2·b·s : 1 − 2·(1−b)·(1−s)` |
| **Soft Light** | W3C form (below) |
| **Difference** | `|b − s|` |
| **Exclusion** | `b + s − 2·b·s` |

**Soft Light** (W3C-compatible, as `softLight()` in the reference):

```
softLight(b, s):
    if s <= 0.5:
        return b - (1 - 2·s) · b · (1 - b)
    else:
        D = (b <= 0.25) ? ((16·b - 12)·b + 4)·b : sqrt(b)
        return b + (2·s - 1) · (D - b)
```

Two identities worth noting (and asserted in tests): `Overlay(b,s) ==
HardLight(s,b)`, and `Normal` ignores the backdrop (`blend = s`). The dodge/burn
guards prevent division blow-ups at the `0`/`1` extremes.

### The Porter-Duff "over" composite (implemented — `compositeOver`)

The separable channel math produces a *blended color* that only applies where the
backdrop is opaque; where the backdrop is transparent, plain source-over must
show the source unchanged. `compositeOver` already encodes this. Given straight-
alpha `backdrop` and `source`, layer `opacity`:

```
compositeOver(mode, backdrop, source, opacity):
    sa = clamp01(source.a) · clamp01(opacity)      # source coverage incl. opacity
    ba = clamp01(backdrop.a)

    # Per-channel separable blend on straight-alpha colors:
    blended = blendChannel(mode, b_chan, s_chan)   for r,g,b

    # Weight the blended color by backdrop coverage (W3C blend-then-composite):
    #   Cs' = (1 - ba)·Cs + ba·blend(Cb, Cs)
    mix = (1 - ba)·source.rgb + ba·blended

    outA = sa + ba·(1 - sa)                         # source-over alpha
    if outA <= 0: return transparent

    # Composite premultiplied, return straight alpha:
    out.rgb = (mix·sa + backdrop.rgb·ba·(1 - sa)) / outA
    out.a   = outA
    return out
```

The key line is `Cs' = (1 − ba)·Cs + ba·blend(Cb, Cs)`: when the backdrop is
fully transparent (`ba = 0`) the blended color collapses to the raw source (so
Multiply over nothing is just the source), and when fully opaque (`ba = 1`) it is
the pure blend — the standard separable-mode-over-alpha rule. The final divide
returns to straight alpha so the result can feed back in as the next backdrop.

### Non-separable modes (to add — Hue, Saturation, Color, Luminosity)

These cannot be done channel-by-channel: they mix the **hue/saturation/luminosity
of whole pixels**. The model is the standard HSY/luminance one (W3C compositing,
matching Photoshop). Helpers, on an RGB triple `C = (r,g,b)`:

```
Lum(C)   = 0.30·r + 0.59·g + 0.11·b           # ITU-ish luma
Sat(C)   = max(r,g,b) - min(r,g,b)            # chroma range

clipColor(C):                                  # pull out-of-gamut back into [0,1]
    L = Lum(C); n = min(r,g,b); x = max(r,g,b)
    if n < 0:  C = L + (C - L)·L / (L - n)
    if x > 1:  C = L + (C - L)·(1 - L) / (x - L)
    return C

setLum(C, l):                                  # set luminosity to l, keep hue/sat
    d = l - Lum(C)
    return clipColor(C + d)                     # add to each channel, then clip

setSat(C, s):                                   # set saturation to s, keep hue
    sort channels into min < mid < max (by value, keep identity)
    if Cmax > Cmin:
        Cmid = (Cmid - Cmin)·s / (Cmax - Cmin)
        Cmax = s
    else:
        Cmid = Cmax = 0
    Cmin = 0
    return recombined triple
```

With those, the four modes (backdrop `Cb`, source `Cs`, full-color triples):

| Mode | Result color |
| --- | --- |
| **Hue** | `setLum(setSat(Cs, Sat(Cb)), Lum(Cb))` — source hue, backdrop sat+lum |
| **Saturation** | `setLum(setSat(Cb, Sat(Cs)), Lum(Cb))` — source sat, backdrop hue+lum |
| **Color** | `setLum(Cs, Lum(Cb))` — source hue+sat, backdrop lum |
| **Luminosity** | `setLum(Cb, Lum(Cs))` — backdrop hue+sat, source lum |

`blendNonSeparable(mode, b, s)` computes the result triple above, and the
**same** `compositeOver` weighting applies: the non-separable result is the
`blend(Cb, Cs)` term, woven into `Cs' = (1 − ba)·Cs + ba·blend(Cb, Cs)` and then
source-over alpha-composited. So the alpha handling is identical to the separable
path; only the per-pixel color function differs. In code this is a branch in
`compositeOver` (separable channel-wise vs. non-separable whole-pixel) ahead of
the shared alpha math.

**Fill opacity & the "special" modes.** A handful of modes (the Photoshop
"special 8": Color Dodge/Burn, Linear Dodge/Burn, Vivid/Linear Light, Hard Mix,
and the difference-like ones) respond to **fill opacity** differently from layer
opacity. The [layer system](03-layer-system.md) passes the effective opacity; the
kernel itself stays a pure function of `(mode, b, s, opacity)`. The exact
fill-opacity treatment for these modes is recorded as an open question below and
deferred until those extra modes land.

## Interactions

- **[Layer system](03-layer-system.md):** the compositor tree walk calls
  `compositeOver` for each layer with the layer's `blendMode` and effective
  opacity; the source is already masked/effected before it arrives.
- **[Canvas & rendering](02-canvas-rendering.md):** blending happens inside
  `compositeTile`, per dirty visible tile, before display conversion.
- **[Color management](15-color-management.md):** blend math runs in the document's
  **working space** at its bit depth; the choice of space (linear vs.
  gamma-encoded) changes results, so golden images are tied to a defined working
  space. Non-separable `Lum` weights assume the working primaries.
- **[Masks](06-masks.md):** masks scale source coverage *before* the blend; they
  are orthogonal to blend mode.
- **[GPU acceleration](23-gpu-acceleration.md) + [ADR-0002](../adr/0002-gpu-abstraction.md):**
  the shader implementations of `blendChannel`/`blendNonSeparable`/`compositeOver`
  must match the CPU reference within tolerance.
- **[File I/O](20-file-io.md):** persists the `BlendMode` enum value; because
  values are append-only they round-trip stably and map to PSD's blend keys.

## Performance, threading & GPU

- The kernel is a **pure, stateless per-pixel function** — ideal for SIMD (process
  4/8 pixels per lane) and for a one-pass fragment/compute shader. No allocation,
  no branches that depend on neighboring pixels.
- Separable modes are branchy at the channel level (dodge/burn/soft-light); SIMD
  uses select/blend instructions rather than data-dependent branches to stay
  vectorized. Non-separable modes do a small sort (3 elements) per pixel — still
  cheap and branch-predictable, and identical on GPU.
- Runs inside the per-tile composite job, partitioned across the
  [worker pool](../01-master-architecture.md#threading-model); workers never alias
  pixels. On the GPU it is part of the tile blit/composite pass.
- Working in float (`Rgbaf`) avoids intermediate banding; the only quantization is
  at 8-bit storage conversion via `fromUnit`/`toFloat`.

## Edge cases & failure modes

- **Transparent backdrop (`ba = 0`):** every mode reduces to plain source-over of
  the (opacity-scaled) source — the `(1 − ba)·Cs` term dominates. Tested per mode.
- **Transparent source (`sa = 0`):** `outA = ba`, output equals the backdrop;
  no-op layer.
- **Both transparent:** `outA ≤ 0` → returns fully transparent (guarded divide).
- **Division extremes:** Color Dodge/Burn guard the `0`/`1` endpoints explicitly;
  no NaN/Inf escapes. Soft Light's piecewise `D` avoids `sqrt` discontinuity.
- **Out-of-gamut non-separable results:** `setLum` can push channels outside
  `[0,1]`; `clipColor` pulls them back toward luminance — never a hard clamp that
  would shift hue. Required for correctness, not optional.
- **Values outside [0,1] on input** (possible in 32-bit float / HDR, M6): the
  reference clamps inputs to `[0,1]` today; the HDR-aware behavior (unclamped
  scene-referred blending) is revisited with [color management](15-color-management.md).
- **Unknown/`Count` enum value:** `blendChannel`/`blendModeName` fall back to
  `Normal`/`"?"` rather than misbehaving (defensive default in the switch).

## Testing strategy

Headless `pe_core` unit tests (the dependency-free harness):

- **Per-mode scalar checks:** `blendChannel(mode, b, s)` matches hand-computed
  values at characteristic points (`0, 0.25, 0.5, 0.75, 1`), including the
  dodge/burn/soft-light boundaries.
- **Identities:** `Overlay(b,s) == HardLight(s,b)`; `Normal` ignores `b`;
  `Difference` is symmetric; `Exclusion(b, 0) == b`.
- **Composite algebra:** `compositeOver` with `opacity = 0` returns the backdrop;
  source-over a transparent backdrop returns the opacity-scaled source; output
  alpha equals `sa + ba(1 − sa)`; result is always valid straight alpha in `[0,1]`.
- **Non-separable invariants:** `Lum(setLum(C, l)) == l`; `Sat(setSat(C, s)) == s`
  (within ε); `clipColor` keeps `Lum` unchanged while clamping range; **Color**
  then **Luminosity** of complementary operands reconstruct expected results.
- **Enum stability:** the integer value of each named mode is asserted (guards
  against accidental reordering of the on-disk format).

Golden-image tests (the visual contract, M1 gate):

- A fixed two-layer fixture composited under **every** mode → committed reference
  buffers; partial-alpha and gradient backdrops included so the
  blend-then-composite weighting is exercised.
- **CPU reference vs. SIMD vs. GPU** of the same fixtures must match within the
  documented tolerance ([ADR-0002](../adr/0002-gpu-abstraction.md)); any drift
  fails CI.
- Non-separable modes get their own fixtures (saturated/desaturated, colored
  backdrops) since `Lum`/`Sat` are where implementations most often diverge.

## Phasing

- **M0 (done):** all twelve separable modes and `compositeOver` implemented and
  unit-tested as the reference.
- **M1 (this doc's gate):** golden-image coverage for every separable mode through
  the real [compositor](03-layer-system.md); enum stability locked.
- **M5/M6:** the four **non-separable** modes (Hue/Saturation/Color/Luminosity)
  added with `blendNonSeparable`, golden-tested; behavior validated once 16/32-bit
  and working-space color land in [color management](15-color-management.md).
- **Later:** the remaining Photoshop modes (Linear Dodge/Burn, Vivid/Linear/Pin
  Light, Hard Mix, Lighter/Darker Color, Subtract, Divide) append to the enum with
  the same reference-vs-GPU discipline; fill-opacity special-casing resolved then.

## Open questions

- **Working space for blending:** do we blend in linear light or in the document's
  gamma-encoded working space by default? This materially changes Multiply/Screen
  results; must be fixed before golden images are frozen
  ([color management](15-color-management.md)).
- **Fill-opacity treatment of the "special 8" modes:** replicate Photoshop's exact
  behavior, or document a principled approximation? Decide when those modes land.
- **HDR / unclamped inputs (32f):** should non-Normal modes clamp to `[0,1]` or
  blend scene-referred values above 1.0? Revisit in M6.
- **`Lum` coefficients:** keep the classic `0.30/0.59/0.11` luma, or derive from
  the working-space primaries for color accuracy? Lean classic for parity, note
  the trade-off.

## References

- [01 — Master Architecture](../01-master-architecture.md) — the compositor loop
  and where the blend kernel sits.
- [00 — Vision & Scope](../00-vision-and-scope.md) — "correctness is defined by a
  reference"; fast paths must match.
- [Glossary](../glossary.md) — Blend mode, Compositor, Premultiplied alpha,
  Working space, Golden image.
- Source: `src/core/include/pe/core/BlendMode.hpp`, `src/core/src/BlendMode.cpp`,
  `src/core/include/pe/core/Color.hpp` (the implemented reference).
- ADRs: [0002 — GPU abstraction](../adr/0002-gpu-abstraction.md) (reference-vs-GPU
  tolerance), [0006 — headless core](../adr/0006-headless-core-separation.md).
- Sibling systems: [layer system](03-layer-system.md),
  [canvas & rendering](02-canvas-rendering.md),
  [color management](15-color-management.md), [masks](06-masks.md),
  [GPU acceleration](23-gpu-acceleration.md), [file I/O](20-file-io.md).
