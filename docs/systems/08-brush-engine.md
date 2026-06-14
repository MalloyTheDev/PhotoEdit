# 08 — Brush Engine

> Milestone: M3 · Status: Spec

## Purpose

The brush engine is the core of painting — the system the
[vision](../00-vision-and-scope.md) calls out for "digital painters /
illustrators" and the canonical example the
[master architecture](../01-master-architecture.md#data-flow-the-life-of-a-brush-stroke)
walks through end to end. Its job is to turn a stream of pointer/tablet samples
into pixels on a layer, and to do so the way artists expect: not as one
geometric line, but as **many stamped dabs spaced along a stabilized input
path**.

The crucial mental model: a *stroke is not a polyline*. When you drag the Brush,
the engine receives a noisy sequence of input samples (position, pressure, tilt,
time), smooths and stabilizes them into a clean path, walks that path placing a
**dab** (one placement of the brush tip) every *spacing* pixels, and blends each
dab into the target buffer — respecting the active [selection](07-selection-system.md),
the layer [mask](06-masks.md), and layer locks. The touched [tiles](../adr/0003-tile-based-engine.md)
are marked dirty and the canvas preview updates. The whole stroke is recorded as
a single reversible [`PaintCommand`](21-history-undo.md).

This document owns the painting *pipeline and math*: tip shapes, the dab-stamping
falloff, the opacity-vs-flow distinction and per-stroke accumulation buffer, all
the brush dynamics (pressure, tilt, rotation, scatter, texture, dual brush, wet
edges, color dynamics), and the command that makes a stroke undoable. The
[tool system](09-tool-system.md) owns the *Brush tool* (the state machine and its
options UI); this engine is what that tool drives. The same machinery paints
**grayscale into a mask** (white reveals, black conceals) with no change to the
pipeline.

## Requirements (Functional + Non-functional)

**Functional**

- Consume an input stream of `StrokePoint`s (sub-pixel position, pressure,
  tilt X/Y, optional rotation/barrel/wheel, timestamp) from mouse, pen tablet,
  or programmatic source.
- **Stabilize/smooth** the path before stamping (configurable strength), with at
  least position averaging / pulled-string stabilization, so jitter and slow-hand
  wobble don't reach the canvas.
- Place dabs along the stabilized path at a configurable **spacing** (as a percent
  of tip diameter), interpolating position *and* all dynamics between samples so
  spacing is arc-length-uniform regardless of cursor speed.
- Support a **tip shape**: a round/elliptical parametric tip (size, hardness,
  roundness, angle) and an arbitrary **sampled tip** (a grayscale stamp image),
  plus tip spacing, scatter, and count.
- Apply per-dab **dynamics** driven by pressure, tilt, velocity, direction, or a
  random source: size, opacity, flow, hardness, angle/rotation, roundness,
  scatter, and color (hue/sat/bright/foreground↔background jitter).
- Honor the **opacity vs. flow** distinction (see below): `opacity` caps the
  whole stroke's strength; `flow` is the per-dab deposition rate. They must
  combine via a **stroke buffer** so overlapping dabs within one stroke do not
  double-darken past `opacity`.
- Composite each dab through a selectable [blend mode](04-blend-modes.md) and the
  foreground color, into the active pixel layer **or** a [mask](06-masks.md).
- Support **texture** (a pattern that modulates dab coverage), **dual brush** (a
  second tip that masks the first), and **wet edges** (heavier deposition at the
  stroke perimeter, watercolor-like).
- Respect the active [selection](07-selection-system.md) as a coverage gate, the
  layer's mask, and **locks** (lock transparency = paint only where alpha already
  exists; lock pixels = reject).
- Record the entire stroke as a single `PaintCommand` with tile-delta undo, and
  mark exactly the touched tiles dirty for recompositing.

**Non-functional**

- `pe_core`, pure C++20 — **no Qt**. Tablet events arrive already translated to
  engine `StrokePoint`s by the [app shell](24-ui-workspace.md); cursors and the
  options bar are the shell's job ([ADR-0006](../adr/0006-headless-core-separation.md)).
- **Interactive latency**: a dab must reach the on-screen preview within a frame
  budget; painting is the highest-frequency edit and stresses the dirty-region
  path. The engine stamps and previews incrementally, never waiting for stroke
  end.
- Cost scales with **painted area**, not document size: only dirty tiles are
  touched, composited, and re-uploaded ([ADR-0003](../adr/0003-tile-based-engine.md)).
- **CPU reference defines correctness**; the SIMD and (later) GPU dab compositors
  must match it within the golden-image tolerance.
- Works at 8/16/32-bit; dab math is done in float (`Rgbaf`) so heavy stacking
  does not band.

## Data model (concrete C++ in `namespace pe`)

Building on the engine foundation (`Point`/`Rect` from `pe/core/Geometry.hpp`,
`Rgbaf`/`Rgba8` from `pe/core/Color.hpp`, `BlendMode`, `kTileSize`). Sub-pixel
positions use float; integer `Point` is only for addressing.

```cpp
namespace pe {

// A 2D sub-pixel position (brush sampling lives between pixel centers).
struct Vec2 { float x = 0.0f; float y = 0.0f; };

// One raw or interpolated input sample along the stroke.
struct StrokePoint {
    Vec2   pos;                 // document-space, sub-pixel
    float  pressure = 1.0f;     // [0,1]; 1.0 for devices without pressure
    float  tiltX = 0.0f;        // pen tilt, radians (-pi/2..pi/2)
    float  tiltY = 0.0f;
    float  rotation = 0.0f;     // barrel rotation / art-pen, radians
    float  velocity = 0.0f;     // px/ms, derived during resampling
    double timeMs = 0.0;        // sample timestamp (for velocity & timing dynamics)
};

// What a dynamic reads from to drive a parameter.
enum class DynamicSource : uint8_t {
    None, Pressure, TiltAngle, TiltMagnitude, Velocity, Direction,
    Rotation, Random, StrokeFade, WheelOrBarrel
};

// A response curve mapping a [0,1] source value to a [0,1] output, with
// optional inversion and per-stroke jitter. Default: identity (output=source).
struct DynamicCurve {
    DynamicSource source = DynamicSource::None;
    std::array<float, 5> controlPoints{0,0.25f,0.5f,0.75f,1.0f}; // sampled curve
    float jitter = 0.0f;        // [0,1] random spread added each dab
    float minimum = 0.0f;       // floor so e.g. light pressure still paints
    bool  invert = false;
    [[nodiscard]] float eval(float sourceValue, RandomState&) const noexcept;
};

// The brush tip: either a parametric round/elliptical mask or a sampled stamp.
struct BrushTip {
    enum class Kind : uint8_t { Round, Sampled };
    Kind  kind = Kind::Round;

    // Parametric (Round):
    float hardness = 1.0f;      // [0,1]; 1 = crisp edge, 0 = fully soft gaussian-ish
    float roundness = 1.0f;     // [0,1]; <1 makes an ellipse (height/width)
    float angle = 0.0f;         // tip rotation, radians

    // Sampled (Sampled): an external grayscale coverage image, 0..255.
    SampledTipId stampId = 0;   // resolved via the brush/preset store (25-presets)

    float spacing = 0.25f;      // dab spacing as a fraction of diameter (0.25 = 25%)
    int   countPerStamp = 1;    // dabs dropped per stamp position (with scatter)
    float scatter = 0.0f;       // [0,1] random offset perpendicular/along path
};

// Texture that modulates dab coverage (paper grain etc.).
struct BrushTexture {
    PatternId pattern = 0;      // from the pattern store
    float     scale = 1.0f;
    float     depth = 1.0f;     // [0,1] how strongly it gates coverage
    BlendMode mode = BlendMode::Multiply;
    bool      eachDab = true;   // re-evaluate per dab vs. per stroke
};

// A second tip whose coverage masks the primary tip ("dual brush").
struct DualBrush {
    bool      enabled = false;
    BrushTip  tip;
    BlendMode combineMode = BlendMode::Multiply;
};

// Color dynamics: how the paint color varies dab-to-dab.
struct ColorDynamics {
    float foregroundBackgroundJitter = 0.0f; // [0,1] mix toward background color
    float hueJitter = 0.0f;     // [0,1] -> +/- degrees
    float saturationJitter = 0.0f;
    float brightnessJitter = 0.0f;
    bool  perDab = true;        // vary per dab vs. once per stroke
};

// The full brush definition. A saved Brush preset (25-presets-assets) is this
// struct plus a name/icon.
struct BrushSettings {
    BrushTip  tip;
    float     baseSize = 30.0f;     // diameter in px at pressure dynamic = 1
    float     opacity = 1.0f;       // [0,1] STROKE ceiling
    float     flow = 1.0f;          // [0,1] per-dab deposition rate
    BlendMode blendMode = BlendMode::Normal;
    float     smoothing = 0.3f;     // [0,1] stabilization strength
    bool      wetEdges = false;

    // Dynamics (each may be DynamicSource::None == constant):
    DynamicCurve sizeDyn{DynamicSource::Pressure};
    DynamicCurve opacityDyn{DynamicSource::None};
    DynamicCurve flowDyn{DynamicSource::Pressure};
    DynamicCurve hardnessDyn{DynamicSource::None};
    DynamicCurve angleDyn{DynamicSource::Direction}; // e.g. follow stroke heading
    DynamicCurve roundnessDyn{DynamicSource::TiltMagnitude};

    BrushTexture  texture;
    DualBrush     dual;
    ColorDynamics color;
};

// A resolved dab: everything needed to stamp ONE placement, after dynamics.
struct Dab {
    Vec2      center;           // sub-pixel
    float     diameter;         // px
    float     hardness;         // [0,1]
    float     roundness;        // [0,1]
    float     angle;            // radians
    float     flow;             // [0,1] per-dab strength
    Rgbaf     color;            // straight-alpha paint color for this dab
};

} // namespace pe
```

The `PaintCommand` (full sketch under [Behavior](#behavior--algorithms)) records
the *prior* contents of every tile the stroke touches, so undo restores them
byte-exact and unchanged tiles stay shared copy-on-write.

## Behavior & algorithms

### The stroke pipeline

```
beginStroke(brush, target, color, selection):
    init stroke buffer (sparse, per-touched-tile coverage accumulator at float)
    init resampler with brush.smoothing; remember 'leftover' arc-length = 0

addInput(rawPoint):                       # called per tablet sample
    p = stabilize(rawPoint)               # smoothing: averaging / pulled-string
    for each segment (prev .. p):
        emitDabsAlong(segment)            # spacing-driven, sub-pixel
    prev = p

emitDabsAlong(seg):
    dist = arcLength(seg)
    step = max(1px, brush.tip.spacing * currentDiameter())
    while leftover + walked < dist:
        t = (stepTarget - segStart)/dist  # param along segment
        sp = interpolate(seg.a, seg.b, t) # position + pressure + tilt + velocity
        dab = resolveDab(sp)              # apply all dynamics -> concrete Dab
        for i in 0..tip.countPerStamp:    # scatter / count
            stampDab(jitter(dab))
        walked += step
    leftover = (dist - walked carry)

endStroke():
    flush stroke buffer into target via blendMode at brush.opacity
    build PaintCommand from recorded tile deltas; push to history
```

Stabilization (`stabilize`) keeps a short history of recent points and outputs a
smoothed position — either a moving average weighted by `smoothing`, or a
"pulled string" where the cursor drags an anchored point a fixed distance behind
(predictable, lag-trading-for-smoothness). Strong smoothing also *interpolates
the tail* on `endStroke` so the line catches up to the final cursor position.

`resolveDab` evaluates every active `DynamicCurve` against the interpolated
sample to produce a concrete `Dab`: `diameter = baseSize * sizeDyn.eval(...)`,
`flow = flow * flowDyn.eval(...)`, `angle` from direction/tilt, color from
`ColorDynamics`, etc. `StrokeFade` lets a parameter ramp down over a set distance
for tapering tails.

### Dab stamping math (falloff & hardness)

A round dab's coverage at a pixel is a radial falloff from its center. With
radius `r = diameter/2`, normalized distance `d = |pixel - center| / r`
(elliptical distance once roundness/angle are applied), coverage is:

```
coverage(d):
    if d >= 1: return 0
    if d <= hardness: return 1               # solid core
    # smooth shoulder from the hardness radius to the edge:
    t = (d - hardness) / (1 - hardness)      # 0..1 across the soft band
    return smoothstep(1 - t)                 # = 1 at core edge, 0 at rim
```

So `hardness = 1` gives a crisp 1-pixel-antialiased edge (only the boundary
pixel is partial); `hardness = 0` gives a wide soft shoulder spanning the whole
radius. The shoulder uses `smoothstep` (3t²−2t³) rather than a linear ramp to
avoid a visible Mach band. A **sampled tip** replaces this formula with a
bilinear lookup into the stamp image (rotated/scaled to the dab), and texture /
dual-brush coverage *multiply* into it. Elliptical tips transform the pixel into
tip space (rotate by `-angle`, divide y by `roundness`) before computing `d`.

### Opacity vs. flow, and the stroke buffer (no double-darkening)

This is the subtle part that separates a real brush engine from naive stamping.

- **Flow** is how much paint each dab lays down (a deposition rate). Low flow +
  many overlapping dabs builds up gradually, like an airbrush.
- **Opacity** is the *maximum* a single continuous stroke can reach. Re-crossing
  your own wet stroke at 50% opacity must **not** push past 50% — but two
  *separate* strokes at 50% should stack toward 75%.

Naively compositing each dab straight onto the layer at `flow` would let
overlapping dabs *within one stroke* accumulate past `opacity` (double-darkening
at crossings). The fix is a per-stroke **stroke buffer**: a sparse, float
coverage accumulator covering the touched tiles. Each dab accumulates into it
with a *max/over* rule, then the whole buffer is composited onto the layer
**once** at stroke end, clamped to `opacity`:

```
# accumulate per dab into the stroke buffer (straight coverage in [0,1]):
stampDab(dab):
    for pixel p under dab (in touched tiles, gated by selection & mask & locks):
        c = coverage(p, dab) * dab.flow * texture(p) * dual(p)
        # "paint over wet paint" rule: deposition adds but each dab can only
        # raise this stroke's coverage, never exceed flow-limited buildup:
        buf[p] = buf[p] + c * (1 - buf[p])        # Porter-Duff 'over' on coverage
        markTileDirty(tileOf(p))

flushStroke():                                    # at endStroke
    for pixel p in stroke buffer:
        a = min(buf[p], opacity) * selection(p) * mask(p)
        layer[p] = compositeOver(blendMode, layer[p], color_at(p) with alpha a, 1.0)
```

Because the buffer caps coverage with the `1 - buf[p]` factor, overlapping dabs
within the stroke asymptote to "fully deposited for this stroke," and the final
clamp to `opacity` enforces the ceiling. For **interactive preview** the engine
flushes *incrementally*: it composites the dirty tiles' current stroke-buffer
state into a preview overlay each frame, so the artist sees the stroke as it is
drawn; the authoritative flush at `endStroke` produces the committed pixels (and
the two must agree, a golden-image invariant). **Pencil mode** is the same
pipeline with `hardness = 1` and coverage thresholded to 0/1 (aliased).

### Wet edges, texture, dual brush, color dynamics

- **Wet edges**: bias coverage toward the dab rim (e.g. multiply by a function
  that dips in the center) and let the stroke buffer pool at the perimeter — a
  cheap watercolor look without a fluid sim.
- **Texture**: sample the pattern at the pixel's document position (so the grain
  is stationary under the stroke) and multiply it into coverage at `depth`.
- **Dual brush**: stamp the secondary `BrushTip` and multiply its coverage into
  the primary's, breaking up the mark.
- **Color dynamics**: jitter the dab `color` in HSB and toward the background
  color per dab (or once per stroke), within the configured spreads.

### Painting into a mask

When the target is a [mask](06-masks.md) buffer rather than a color layer, the
pipeline is identical but the "color" is a grayscale value (white = reveal,
black = conceal) written into single-channel coverage. The command is a
`PaintMaskCommand` sharing the same tile-delta machinery.

## Interactions (Document/Command/Layer/compositor/Tool/RHI + sibling links)

- **[Tool system](09-tool-system.md)** — the Brush/Pencil/Eraser/Mixer tools are
  state machines that feed `StrokePoint`s in and commit the resulting
  `PaintCommand`; this engine is the shared core they drive. Eraser paints with
  the *clear* operation (or onto a mask); the Mixer reads canvas color back into
  the dab.
- **[Document](01-document-system.md) & [history](21-history-undo.md)** — a stroke
  is one `Command`; `execute` deposits the recorded result, `undo` restores the
  prior tiles. The document emits `DocumentChange{Pixels, dirtyRegion}` so the
  canvas recomposites only those tiles
  ([ADR-0005](../adr/0005-command-history-model.md)).
- **[Layer system](03-layer-system.md) & [compositor](02-canvas-rendering.md)** —
  paint writes into the active pixel layer's tiles; the dirty region drives
  recompositing through the tree and blend modes.
- **[Masks](06-masks.md)** — painting on a mask uses the identical pipeline; the
  layer mask also *gates* color painting (coverage × mask).
- **[Selection](07-selection-system.md)** — the active selection multiplies into
  every dab's coverage, so a brush is confined to the selected region with soft
  (feathered) edges for free.
- **[Blend modes](04-blend-modes.md)** — the stroke flush calls `compositeOver`
  with the brush's blend mode; the per-channel kernel is shared with the layer
  compositor (one definition of correctness).
- **[Color management](15-color-management.md)** — the foreground color and dab
  math live in the document's working space at its bit depth; no bare RGB.
- **[Presets & assets](25-presets-assets.md)** — `BrushSettings`, sampled tips,
  patterns, and textures are presets loaded from the brush/asset store.
- **[RHI / GPU](23-gpu-acceleration.md)** — dab rasterization and stroke-buffer
  compositing are data-parallel and a strong GPU-acceleration candidate (see
  below); the CPU reference remains authoritative.
- **App shell** — translates Qt tablet events to `StrokePoint`s, draws the brush
  cursor (size/hardness ring) and renders the options bar from the
  [tool options model](09-tool-system.md); the engine owns none of this.

## Performance, threading & GPU

- **Tile-local work.** A dab touches only the few tiles under it (radius +
  scatter). The engine computes the dab's bounding `Rect`, derives the
  `TileSpan` via `tilesForRect`, allocates stroke-buffer tiles lazily, and marks
  exactly those dirty. A single dab on a 30k×30k document is as cheap as on a
  small one.
- **Incremental preview.** Stamping runs on the document thread at input rate;
  the dirty tiles are previewed each frame. Heavy strokes coalesce inputs but
  never drop the final sample (the committed line must reach the last cursor
  position).
- **SIMD dab compositing.** The inner coverage/accumulate loop is the hottest in
  the app: it is written for the CPU reference clearly, then vectorized (process
  a row of pixels per SIMD lane for the falloff and the `over` accumulate). The
  SIMD path must match the reference within tolerance.
- **GPU acceleration potential.** Dab stamping maps naturally to the
  [RHI](23-gpu-acceleration.md): render dabs as instanced quads into a stroke
  render-target with the falloff in a fragment shader, accumulate coverage there,
  and composite the target into the layer tiles at stroke end. This keeps very
  large soft brushes interactive. We ship the CPU path first (M3) and offer GPU
  acceleration where it pays, validated against the reference.
- **Memory.** The stroke buffer is sparse (only touched tiles) and freed at
  stroke end; the `PaintCommand` keeps only the tile deltas (prior contents of
  touched tiles, copy-on-write), so undo memory scales with painted area.

## Edge cases & failure modes

- **Single click (no drag)** — one dab at the click point; spacing logic must
  emit at least one stamp for a zero-length stroke.
- **Very fast stroke / sparse samples** — interpolation along each segment must
  keep spacing arc-length-uniform; a fast flick must not leave gaps. Conversely a
  very slow hand must not pile dabs (spacing is by distance, not by sample count).
- **Tiny spacing + huge brush** — dab count explodes; clamp `step` to ≥ 1px and
  guard total dab count per segment so a degenerate setting can't hang.
- **Pressure = 0 at touch-down** — `DynamicCurve.minimum` keeps a faint mark or
  none per the brush; never divide-by-zero on `roundness`/`hardness == 0` (clamp
  denominators).
- **Painting outside the layer / off-canvas** — legal; layers may extend past the
  canvas (negative coords handled by `floorDiv`). Locks: *lock transparency*
  multiplies coverage by existing alpha; *lock pixels* rejects the stroke (UI
  warns, no command pushed).
- **Selection excludes the cursor** — dabs land but contribute zero; no tiles go
  dirty, no command if nothing changed.
- **Empty/disabled mask target** — painting on a mask with no tiles allocates
  them on write; absent ≠ error.
- **Interrupted stroke** (focus loss, tablet disconnect) — `endStroke` is forced;
  a partial-but-valid `PaintCommand` is committed (or discarded if zero dabs).
- **16/32-bit** — accumulation stays in float; only the final flush quantizes to
  the layer's storage depth.

## Testing strategy

Headless `pe_core` tests plus golden images:

- **Spacing/arc-length** — a straight drag at varying speeds produces dabs at the
  configured spacing in *document* space (assert centers are evenly spaced,
  independent of sample rate).
- **Falloff math** — `coverage(d)` unit table for hardness 0 / 0.5 / 1; monotonic,
  1 at center, 0 at rim, C¹ at the shoulder; elliptical transform correctness.
- **Opacity vs. flow (the key invariant)** — paint a stroke that crosses itself at
  50% opacity: the crossing must not exceed 50% (stroke-buffer cap); two separate
  50% strokes stack toward 75%. Golden image + numeric assertion.
- **Preview == commit** — the incremental preview composite equals the
  authoritative `endStroke` flush, pixel-for-pixel within tolerance.
- **Dynamics** — pressure→size and direction→angle curves produce the expected
  per-dab parameters on a synthetic input stream.
- **Selection/mask/lock gating** — a stroke confined to a feathered selection
  matches a golden; lock-transparency only paints existing alpha.
- **Undo** — `PaintCommand` restores byte-exact prior tiles; unchanged tiles
  remain shared (CoW), verified on a large fixture (the M3 exit criterion).
- **SIMD/GPU parity** — vectorized and GPU dab compositors match the scalar
  reference within the golden tolerance.

## Phasing

- **M3 (this doc lands)** — round + sampled tips; spacing/size/hardness;
  opacity/flow with the stroke buffer; smoothing/stabilization; pressure & tilt
  dynamics; blend mode; eraser/pencil; `PaintCommand` tile-delta undo; CPU +
  SIMD dab compositing; incremental preview.
- **M4** — painting into [masks](06-masks.md) wired to the masks workflow
  (white/black reveal/conceal), selection gating fully honored.
- **M5/M6** — 16/32-bit float dab math through the high-depth pixel path; texture
  and color dynamics matured with [color management](15-color-management.md).
- **M8** — dual brush, advanced scatter/rotation, the Mixer brush (reads canvas
  color), and brush-preset polish via [presets](25-presets-assets.md).
- **Continuous** — [GPU](23-gpu-acceleration.md) dab acceleration lands where it
  pays and is benchmarked against the CPU reference.

## Open questions

- **Stabilizer model** — ship moving-average *and* pulled-string and let the
  brush choose, or pick one default? Pulled-string feels best for inking but adds
  visible lag.
- **Stroke-buffer precision** — 8-bit coverage is cheaper but soft low-flow
  airbrushing may band; default to 16-bit float accumulation even in 8-bit
  documents?
- **Speed→size dynamic by default** — many users expect velocity-driven taper;
  do we enable a subtle default, or keep brushes literal until configured?
- **GPU stroke buffer determinism** — guaranteeing bit-identical accumulation
  order on the GPU is hard; is "within golden tolerance" sufficient for the
  committed result, with the CPU path as the canonical bake?
- **ABR import** — do we parse Photoshop `.abr` brush sets in
  [presets](25-presets-assets.md), and how faithfully do their dynamics map onto
  our `DynamicCurve` model?

## References (relative links)

- [01 — Master architecture](../01-master-architecture.md) — the "life of a brush
  stroke" data flow this elaborates; the `Tool`/`Command` contracts.
- [00 — Vision & scope](../00-vision-and-scope.md) — digital-painting target user;
  non-destructive, reference-defined correctness.
- [Glossary](../glossary.md) — Brush dab / stamp, Tile, Dirty region, Command.
- Sibling systems: [09 — Tool system](09-tool-system.md),
  [06 — Masks](06-masks.md), [07 — Selection system](07-selection-system.md),
  [21 — History & undo](21-history-undo.md), [04 — Blend modes](04-blend-modes.md),
  [15 — Color management](15-color-management.md),
  [02 — Canvas & rendering](02-canvas-rendering.md),
  [25 — Presets & assets](25-presets-assets.md),
  [23 — GPU acceleration](23-gpu-acceleration.md).
- ADRs: [0003 — tile-based engine](../adr/0003-tile-based-engine.md),
  [0005 — command/history model](../adr/0005-command-history-model.md),
  [0006 — headless core](../adr/0006-headless-core-separation.md).
