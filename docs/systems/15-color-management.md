# 15 — Color Management

> Milestone: M6 · Status: In progress (foundation) — see [STATUS](../STATUS.md)

## Purpose

Color management is the system that makes the same document look *the same* across
a laptop screen, a wide-gamut monitor, a CMYK press, a phone, and a web browser —
and that lets a photographer trust that what they edit is what they will print.
It is one of the most underrated-hard systems in the whole program: the failure
mode is not a crash but a subtle, pervasive *wrongness* (skin tones that shift,
skies that band, a logo blue that prints purple) that compounds invisibly through
every later operation.

The central truth this system encodes is that **the same RGB numbers describe
different colors in different spaces.** `(220, 30, 40)` is one red in sRGB,
a more saturated red in Display P3, and a different red again in Adobe RGB. A pixel
is therefore never "just RGB"; it is a triple (or quad) of numbers *plus* the
space that gives them meaning. Strip the space and the numbers are noise.

This document specifies how PhotoEdit attaches meaning to color: the **color
modes** (RGB, CMYK, Grayscale, Lab, Indexed, Bitmap), **bit depths** (8/16/32f),
**ICC profiles** (`ColorProfile`), the **working space** the
[compositor](02-canvas-rendering.md) does its math in, the cached **color
transforms** (`ColorTransform`) that move pixels between spaces, and the three-leg
pipeline — *input→working* on import, *working→display* for the screen,
*working→output* for export and print. It is built on **Little-CMS 2 (lcms2)** per
[ADR-0004](../adr/0004-color-management.md); we own the policy and the pipeline,
lcms2 owns the ICC math.

## Requirements

**Functional**

- Represent and convert between **color modes**: RGB, CMYK, Grayscale, Lab,
  Indexed (palette), and Bitmap (1-bit). The document's mode is owned by the
  [document](01-document-system.md); this system performs the managed conversions.
- Wrap **ICC profiles** in a `ColorProfile` (input, working, display, and output
  profiles) loaded from embedded data, files, or built-in resources.
- Ship built-in working spaces: **sRGB**, **Display P3**, **Adobe RGB (1998)**,
  **ProPhoto RGB**, plus **Gray Gamma 2.2 / Dot Gain** grayscale spaces and stock
  **CMYK** profiles (e.g. US Web Coated SWOP, FOGRA39, GRACoL).
- Composite in a defined **working space** at the document
  [bit depth](../glossary.md); blend/filter math is done in float regardless of
  storage depth.
- **Assign** vs. **Convert**: *assign* changes the profile a document is
  interpreted with (numbers unchanged, appearance changes); *convert* changes the
  numbers to preserve appearance into a new space. Both are reversible
  [commands](../glossary.md).
- Drive the screen via the **display profile** (the monitor's ICC profile),
  per-monitor on multi-display setups.
- **Soft proofing**: simulate on screen how the document will appear on a chosen
  output device/space, with options to simulate paper white and black ink.
- **Gamut warning**: flag pixels that fall outside the proof destination's gamut.
- Expose the four ICC **rendering intents** — **perceptual**, **relative
  colorimetric**, **saturation**, **absolute colorimetric** — and **black-point
  compensation (BPC)** as an independent toggle.
- Convert input images **into** the working space on import (placed files, opened
  files, pasted pixels), and the working result **out** to a chosen profile on
  export/print ([file I/O](20-file-io.md), [printing & prepress](27-printing-prepress.md)).
- Handle **untagged** content with a configurable policy (assume sRGB, or prompt).

**Non-functional**

- `pe_core`, pure C++20, wrapping **lcms2** ([ADR-0004](../adr/0004-color-management.md));
  **no Qt** ([ADR-0006](../adr/0006-headless-core-separation.md)). The display
  profile is *read* from the OS by the [app shell](24-ui-workspace.md) and handed
  in; the engine never calls Qt or the windowing system.
- Color conversion is **per-tile and reentrant**: the compositor and exporters run
  transforms across the [worker pool](../01-master-architecture.md#threading-model)
  on disjoint tiles, so transform objects must be safe to share for read.
- Transform construction (lcms2 link) is comparatively expensive; the system
  **caches** built transforms keyed by `(src, dst, intent, bpc, depth)` so the
  hot path is a table lookup plus a buffered apply.
- The CPU/lcms2 path defines correctness; any GPU display-conversion shader must
  match it within the golden-image tolerance ([ADR-0002](../adr/0002-gpu-abstraction.md)).
- Higher [bit depth](../glossary.md) is the defense against banding and the
  enabler of HDR; the engine must carry 16-bit and 32-bit-float pixels end to end.

## Data model

Concrete, illustrative shapes in `namespace pe`. `ColorMode` and `BitDepth` are
the ones the [document](01-document-system.md) already declares; they live with the
document but are reproduced here for context. Real headers may differ in detail,
not in concept.

```cpp
namespace pe {

// (declared with the document; shown here for reference)
enum class ColorMode : uint8_t { RGB, CMYK, Gray, Lab, Indexed, Bitmap };
enum class BitDepth  : uint8_t { U8 = 8, U16 = 16, F32 = 32 };

// The four ICC rendering intents. The numeric values match lcms2 (INTENT_*).
enum class RenderingIntent : uint8_t {
    Perceptual          = 0,  // compress whole gamut pleasingly; photos
    RelativeColorimetric= 1,  // map in-gamut exactly, clip out-of-gamut; default + BPC
    Saturation          = 2,  // maximise saturation; charts/graphics
    AbsoluteColorimetric= 3,  // exact incl. white point; proofing paper white
};

// Whether colour is stored gamma-encoded (perceptual) or in linear light.
// The working space records this so math knows what it is operating on.
enum class Encoding : uint8_t { Linear, GammaEncoded };

// Wraps a loaded ICC profile (an lcms2 cmsHPROFILE under the hood). Immutable;
// shared by ref-counted handle so many transforms/documents reuse one profile.
class ColorProfile {
public:
    [[nodiscard]] static ColorProfileRef fromIccData(std::span<const std::byte>);
    [[nodiscard]] static ColorProfileRef fromFile(const std::filesystem::path&);
    [[nodiscard]] static ColorProfileRef builtin(BuiltinSpace);  // sRGB, P3, …

    [[nodiscard]] ColorMode    mode() const noexcept;      // RGB/CMYK/Gray/Lab
    [[nodiscard]] std::string  description() const;        // human label
    [[nodiscard]] Encoding     encoding() const noexcept;  // linear / gamma
    [[nodiscard]] std::span<const std::byte> iccBytes() const; // for embedding on save

    // Opaque lcms2 handle; only the ColorTransform builder touches it.
    [[nodiscard]] void* nativeHandle() const noexcept;
};

enum class BuiltinSpace : uint8_t {
    sRGB, DisplayP3, AdobeRGB1998, ProPhotoRGB,            // RGB working spaces
    sRGBLinear, ProPhotoLinear,                            // linear variants
    GrayGamma22, GrayDot15,                                // grayscale
    CmykSwop, CmykFogra39, CmykGracol,                     // stock CMYK
    LabD50,                                                // Lab connection-ish
};

// A built, cached colour transform: a baked pipeline from one space to another.
// Wraps an lcms2 cmsHTRANSFORM. Apply() is the per-tile hot path.
class ColorTransform {
public:
    // Transform a run of pixels in place / into a destination buffer. Formats are
    // chosen by src/dst mode+depth; premultiplication is handled by the caller.
    void apply(const void* src, void* dst, int pixelCount) const noexcept;

    [[nodiscard]] const TransformKey& key() const noexcept;
};

// The cache key: identical keys → the same baked transform is reused.
struct TransformKey {
    const ColorProfile* src = nullptr;
    const ColorProfile* dst = nullptr;
    RenderingIntent intent = RenderingIntent::RelativeColorimetric;
    bool   blackPointCompensation = true;
    BitDepth srcDepth = BitDepth::F32;
    BitDepth dstDepth = BitDepth::F32;
    const ColorProfile* proof = nullptr;   // non-null ⇒ soft-proof (device-link)
    bool   gamutCheck = false;             // mark out-of-gamut pixels
    bool   operator==(const TransformKey&) const = default;
};

// Owns the working-space choice and the transform cache. One per engine session.
class ColorEngine {
public:
    [[nodiscard]] ColorProfileRef workingSpace(ColorMode) const;   // current working profile
    void setWorkingSpace(ColorMode, ColorProfileRef);

    // Build-or-fetch a cached transform for a key (thread-safe; transforms are
    // immutable once built and safe to share for read across workers).
    [[nodiscard]] const ColorTransform& transform(const TransformKey&);

    // Convenience builders for the three pipeline legs:
    [[nodiscard]] const ColorTransform& toWorking(const ColorProfile& input);
    [[nodiscard]] const ColorTransform& toDisplay(const ColorProfile& monitor,
                                                  RenderingIntent, bool bpc);
    [[nodiscard]] const ColorTransform& toOutput(const ColorProfile& output,
                                                 RenderingIntent, bool bpc);
    // Soft-proof: working → (simulate output) → display, with optional gamut mark.
    [[nodiscard]] const ColorTransform& proofTransform(const ColorProfile& output,
                                                       const ColorProfile& monitor,
                                                       const ProofSettings&);
};

struct ProofSettings {
    ColorProfileRef   output;                 // device being simulated
    RenderingIntent   intent = RenderingIntent::RelativeColorimetric;
    bool simulatePaperWhite = false;          // absolute white simulation
    bool simulateBlackInk   = false;
    bool gamutWarning       = false;
    Rgbaf gamutWarningColor{0.5f, 0.5f, 0.5f, 1.0f};  // overlay for out-of-gamut
};

} // namespace pe
```

The document's working pixels remain `Rgbaf` (and 16-bit / 8-bit storage variants)
from [`Color.hpp`](../../src/core/include/pe/core/Color.hpp); color management
never replaces the pixel type, it gives the numbers a *profile* and converts them
at the seams.

## Behavior & algorithms

**Linear vs. gamma-encoded — and why blending should be in linear light.** Display
encodings (sRGB, most ICC RGB working spaces) are *gamma-encoded*: the stored
number is a perceptually-spaced code, not proportional to photons. That is right
for storage and for an 8-bit code budget, but **wrong for physics**. Operations
that model light — alpha compositing, blurs and other convolutions, downscaling,
additive blend modes, gradient interpolation — are only correct when the values
are *linear* (proportional to light intensity). Blend two semi-transparent layers
in gamma space and edges darken; blur in gamma space and bright detail crushes;
scale a checkerboard in gamma space and it shifts gray. PhotoEdit's convention:

> The working space is **linear-light RGB** for compositing, blur, and resampling.
> Pixels are decoded to linear on the way into a math pass and (where they leave to
> a display/export encoding) re-encoded on the way out. Tonal/UI operations that
> are *defined* perceptually (Curves, Levels, the histogram, "50% gray") operate on
> the gamma-encoded values, by design.

This is captured per-operation, not globally: the compositor and convolutions
demand linear; the curve/level math demands the editing encoding. `Encoding` on the
`ColorProfile` records which a given space is, and conversions between encodings are
a 1-D transfer-function apply (cheap, exact in float). The alternative — composite
everything in gamma space — is simpler and is what some legacy tools shipped; we
reject it as a correctness compromise but document the choice so the trade-off is
explicit.

**Why 8 vs. 16 vs. 32-bit float matters.** Eight bits per channel gives 256 levels;
after a few non-destructive adjustments stacked at composite time, the gaps between
levels become visible as **banding** in smooth gradients and skies, and repeated
round-trips quantize away detail. Sixteen-bit (≈65k levels) gives adjustments
enormous headroom before banding shows. Thirty-two-bit **float** removes the [0,1]
ceiling entirely: values above 1.0 represent highlights brighter than diffuse
white, which is exactly what **HDR** imagery and scene-linear compositing need. The
rule of thumb the UI should teach: *edit in the highest depth the work justifies;
8-bit is for final export, not for a long non-destructive stack.* Because math is
always done in float (`Rgbaf`), depth is a **storage** decision — it controls
banding/HDR and memory, not the math precision of any single operation.

**The three-leg pipeline.** Every pixel's journey is one of three transforms,
each a cached `ColorTransform`:

```
import(image, embeddedProfile):              # leg 1: input → working
    src = embeddedProfile ?? assumedProfile(policy)     # untagged → sRGB or prompt
    t   = colorEngine.toWorking(src)                    # to linear working space
    for tile in image: t.apply(tile, workingTile)       # store at doc bit depth

display(workingTile, monitorProfile):        # leg 2: working → display
    if softProofOn:
        t = colorEngine.proofTransform(proof.output, monitorProfile, proofSettings)
    else:
        t = colorEngine.toDisplay(monitorProfile, displayIntent, bpc)
    t.apply(workingTile, screenTile)                    # then upload via RHI

export(workingResult, outputProfile, intent, bpc):     # leg 3: working → output
    t = colorEngine.toOutput(outputProfile, intent, bpc)
    t.apply(flattened, encodedPixels)                   # embed outputProfile on save
```

The compositor itself never converts; it stays entirely in the working space. Color
conversion happens only at the **import boundary** (once, on the way in) and at the
**display/export boundary** (per dirty tile for screen, once for export). This is
why the [master architecture](../01-master-architecture.md#color-pipeline-where-it-sits)
can say "color is never bare RGB": the working buffer is always one known space, and
both ends of the pipe convert through profiles.

**Soft proofing** builds a *device-link*-style transform that goes
working → (proof output gamut) → monitor in one baked pipeline, so the screen shows
how the file will look on the press. **Simulate paper white** uses
`AbsoluteColorimetric` so the proof's white point (often a warm, dim paper) is
reproduced rather than mapped to monitor white. **Gamut warning** sets the
lcms2 gamut-check flag and paints flagged pixels with `gamutWarningColor`, telling
the user which colors the destination cannot reproduce *before* they commit.

**Rendering intents** choose how out-of-gamut colors are handled when the
destination is smaller than the source: **perceptual** compresses the whole gamut
to keep relationships pleasant (best for photographs), **relative colorimetric**
maps in-gamut colors exactly and clips the rest (best general default, paired with
**BPC** so deep shadows don't plug), **saturation** maximizes vividness (charts,
signage), and **absolute colorimetric** reproduces colors including the white point
(proofing). **Black-point compensation** independently maps source black to
destination black so detail survives the darkest tones; it is a toggle, not an
intent.

**Mode conversions** route through the connection space. RGB→CMYK, RGB→Gray, →Lab,
and →Indexed are all `ColorTransform`s (or, for Indexed/Bitmap, a transform plus a
quantization/dither step). These are reversible *as history steps* by snapshotting,
but lossy forward (a gamut or palette is discarded), so the
[document](01-document-system.md)'s `ConvertColorModeCommand` warns and the
[channels](19-channels.md) set is rewritten to match the new mode.

## Interactions

- **[Document](01-document-system.md):** owns `ColorMode`, `BitDepth`, and the
  document `ColorProfile`; mode/depth/profile changes are commands that call into
  this system. The working space is a property of the document's color setup.
- **[Canvas & rendering](02-canvas-rendering.md):** the sole consumer of the
  *working→display* leg; it asks for a (proof or plain) display transform per dirty
  tile and uploads the converted result through the RHI.
- **[Channels](19-channels.md):** color mode determines the channel set (R/G/B,
  C/M/Y/K, single Gray, L/a/b); spot channels carry their own colorant; this system
  and channels co-own the meaning of a component plane.
- **[Blend modes](04-blend-modes.md) / compositor:** blend and coverage math run in
  the linear working space; non-separable Hue/Sat/Color/Luminosity modes are
  defined in terms of working-space luminance.
- **[Camera Raw](16-camera-raw.md):** raw develop outputs into a chosen RGB working
  space (often ProPhoto-linear or a wide space) — its final stage is an
  *input→working* conversion this system defines.
- **[Printing & prepress](27-printing-prepress.md):** the *working→output* leg for
  CMYK separation, proof, gamut, spot colors, and PDF export builds on these
  transforms and intents.
- **[File I/O](20-file-io.md):** embeds the document/output profile on save, reads
  embedded profiles on open, and applies the untagged-content policy.
- **[Filter engine](12-filter-engine.md):** convolutions (blur, sharpen, resample)
  must run in linear light; the filter framework requests linearization around
  light-modeling kernels.
- **App shell:** reads each monitor's ICC profile from the OS and hands it in;
  presents the Color Settings, soft-proof, and gamut-warning UI; the engine itself
  is windowing-agnostic.

## Performance, threading & GPU

- **Transform cache.** Building an lcms2 transform (especially a multi-profile
  proof link) is the expensive step; `ColorEngine` caches by `TransformKey` so a
  pan/zoom or a hundred dirty tiles reuse one baked transform. Cache entries are
  immutable once built and shared for read across workers without locking; the
  cache map itself is guarded for the rare build.
- **Per-tile, parallel apply.** `ColorTransform::apply` runs on 256×256 tiles
  across the [worker pool](../01-master-architecture.md#threading-model); workers
  touch disjoint tiles so there is no aliasing. lcms2's buffered transforms are
  efficient over pixel runs.
- **Depth cost.** 16-bit doubles and 32-bit-float quadruples tile memory and
  bandwidth versus 8-bit; the [performance layer](22-performance.md)'s RAM budget
  and scratch paging absorb this, and previews may composite at reduced resolution
  while full-res computes behind.
- **GPU display conversion.** The hot *working→display* leg can be a shader: a 3-D
  LUT baked from the lcms2 transform (or an analytic matrix+TRC for matrix
  profiles) on the GPU, validated to match the lcms2 reference within tolerance.
  Soft-proof and gamut-warning bake into the same LUT. Export/print conversions stay
  on the lcms2 CPU path where exactness matters most.

## Edge cases & failure modes

- **Untagged image on import:** apply the configured policy (assume the working
  RGB, assume sRGB, or prompt). Never silently treat raw numbers as already in the
  working space without recording the assumption.
- **Corrupt / unparseable ICC:** lcms2 returns null; fall back to the assumed
  profile and surface a non-fatal warning — never crash or drop the pixels.
- **No display profile available:** assume sRGB for the screen so the image is at
  least plausible; warn that the monitor is unmanaged.
- **Out-of-gamut on export:** governed by the chosen intent; with gamut warning on,
  the user is shown the affected pixels first. CMYK conversion necessarily clips a
  large part of an RGB gamut — expected, not an error.
- **Assign vs. convert confusion:** assigning a wrong profile changes appearance
  without changing numbers (a recoverable mistake); converting bakes appearance into
  numbers (lossy if the destination is smaller). The UI must keep these visibly
  distinct; both are undoable.
- **32-bit values outside [0,1]:** legal and meaningful (HDR highlights). Display
  and 8/16-bit export must tone-map or clip explicitly; `clamp01`
  ([`Color.hpp`](../../src/core/include/pe/core/Color.hpp)) is applied only at an
  encoding boundary, never mid-composite.
- **Profile mismatch on paste between documents:** the pasted pixels are converted
  from the source document's working space into the destination's, not copied as raw
  numbers.
- **Indexed/Bitmap round-trip:** converting to Indexed then back to RGB cannot
  recover discarded palette entries; the forward conversion warns.

## Testing strategy

Headless `pe_core` unit tests (the dependency-free harness, with lcms2 linked in the
M6 lane):

- **Round-trip identity:** sRGB→working→sRGB is within tolerance; a no-op transform
  (same src/dst) is the identity.
- **Known-value conversions:** spot colors with published Lab/CMYK values convert
  within ΔE tolerance against lcms2 references (e.g. a pure sRGB primary into
  Adobe RGB / ProPhoto).
- **Intent behavior:** an out-of-gamut color clips under relative-colorimetric but
  shifts under perceptual; BPC on vs. off changes deep-shadow output as expected.
- **Linear-light correctness:** compositing 50% white over 50% black, and blurring a
  black/white edge, match the *linear* reference, not the gamma-space result
  (the canonical "blend should be in linear" golden cases).
- **Cache behavior:** identical `TransformKey`s return the same baked transform;
  differing keys do not collide; concurrent `transform()` from many threads is safe.
- **Depth & banding:** a smooth gradient adjusted repeatedly at 16/32-bit shows no
  banding where the 8-bit path does (histogram-comb / max-step assertions).

Golden-image tests:

- A reference image displayed through a known monitor profile, and soft-proofed to a
  known CMYK profile (with and without paper-white / gamut warning), matches
  committed references; GPU display LUT matches the lcms2 CPU path within tolerance.

## Phasing

Color management is *designed in from M1* (the working space exists the moment the
compositor does) and *fully realized in M6*, with a staged depth rollout:

- **M1–M5 (groundwork):** the document already carries `ColorMode`, `BitDepth`, and
  a `ColorProfile`; compositing is defined as happening in a working space, but the
  8-bit sRGB path is the only one exercised. No lcms2 yet.
- **M6 (this doc lands):** lcms2 integrated; `ColorProfile` / `ColorTransform` /
  `ColorEngine` and the transform cache; **RGB / Gray / Lab** managed, **CMYK
  groundwork**; the **16-bit** and then **32-bit-float** pixel paths threaded
  through storage, compositor, and filters; working-space (linear) compositing,
  display conversion, **soft proofing**, **gamut warning**, **rendering intents**,
  and **BPC**. [Channels](19-channels.md) surfaces alongside.
- **M7:** profiles embedded/read on save/open across formats
  ([file I/O](20-file-io.md)); untagged policy wired into import.
- **M9:** raw develop targets a wide working space ([camera raw](16-camera-raw.md)).
- **M10:** full **CMYK** separation, spot colors, and PDF/print color
  ([printing & prepress](27-printing-prepress.md)); optional GPU display LUT matured.

## Open questions

- **Default working space:** ship sRGB as the default RGB working space (familiar,
  safe) or a wider space like Display P3 for modern displays? Leaning sRGB default,
  P3/Adobe RGB/ProPhoto one click away, with a clear "edit in linear" note.
- **Display compositing encoding:** do we convert each composited tile to display
  encoding on the CPU, or keep tiles linear and let a GPU shader encode at present
  time? Leaning GPU-encode for the display leg once the RHI path is proven.
- **OCIO interop:** ADR-0004 leaves the door open to OpenColorIO for VFX/scene-linear
  pipelines. Add as an alternate config-driven path later, or stay ICC-only? Defer
  until a concrete VFX-interop need appears.
- **Per-monitor management on the fly:** dragging a window across monitors of
  different gamuts should re-pick the display transform; how aggressively do we
  re-bake vs. cache per monitor? Cache per monitor id; revisit if it stutters.
- **CMYK preview without a full mode switch:** soft-proof gives a CMYK *preview*;
  do we ever need true in-place CMYK *editing* before M10? Likely no; revisit with
  prepress.

## References

- [01 — Master Architecture](../01-master-architecture.md) — where the color
  pipeline sits relative to the compositor.
- [00 — Vision & Scope](../00-vision-and-scope.md) — "color is never just RGB";
  color correctness as a flagship property.
- [Glossary](../glossary.md) — Working space, ICC profile, Soft proofing, Bit depth,
  Premultiplied alpha.
- [ADR-0004 — color-managed pipeline on lcms2](../adr/0004-color-management.md) —
  the decision this document elaborates.
- Other ADRs: [0002 — GPU abstraction](../adr/0002-gpu-abstraction.md),
  [0006 — headless core](../adr/0006-headless-core-separation.md),
  [0003 — tile-based engine](../adr/0003-tile-based-engine.md).
- Sibling systems: [document](01-document-system.md),
  [canvas & rendering](02-canvas-rendering.md), [channels](19-channels.md),
  [blend modes](04-blend-modes.md), [camera raw](16-camera-raw.md),
  [filter engine](12-filter-engine.md), [printing & prepress](27-printing-prepress.md),
  [file I/O](20-file-io.md).
