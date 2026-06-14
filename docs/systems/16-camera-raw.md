# 16 — Camera Raw / Photography Pipeline

> Milestone: M8 (groundwork) · M9 (full pipeline) · Status: Spec

## Purpose

A raw file is not an image — it is sensor data that must be *interpreted*. Where a
JPEG is already demosaiced, white-balanced, and tone-mapped, a raw file holds the
mosaiced sensor readout and needs a full develop pipeline to become RGB. Camera
Raw is effectively its own image-processing engine, and its key property is that
its settings remain **re-editable**: opening a raw as a [smart object](11-smart-objects.md)
lets the user revisit white balance, exposure, and every other control later,
even after downstream editing.

## Requirements

**Functional**

- Decode raw sensor data (Bayer / X-Trans) and develop it to the working
  [color space](15-color-management.md).
- Controls: white balance (temp/tint), exposure, contrast, highlights, shadows,
  whites, blacks, texture, clarity, dehaze; tone curve; HSL/color grading; lens
  correction; chromatic aberration; defringe; noise reduction; sharpening.
- Camera/creative profiles (DCP-style) selectable per image.
- Re-editable as a smart object: settings stored, re-applied on demand.
- Batch develop and copy/paste/sync of settings ([presets](25-presets-assets.md)).

**Non-functional**

- `pe_core`, no Qt. Develop in high precision (32-bit float, linear where
  appropriate) to preserve highlight/shadow latitude.
- Heavy; runs on [workers](22-performance.md) with previews; tile-aware.

## Data model

```cpp
namespace pe {

struct WhiteBalance { float temperatureK = 5500; float tint = 0; };

struct RawDevelopSettings {
    WhiteBalance wb;
    float exposureEV = 0, contrast = 0;
    float highlights = 0, shadows = 0, whites = 0, blacks = 0;
    float texture = 0, clarity = 0, dehaze = 0;
    ToneCurve curve;
    HslGrade hsl; ColorGrade grade;
    LensCorrection lens; float defringe = 0;
    NoiseReduction nr; Sharpening sharpen;
    std::string cameraProfileId;     // DCP / creative profile
};

struct RawSource {              // backs a smart object
    RawImage sensor;            // decoded mosaic + metadata (LibRaw)
    RawDevelopSettings settings;
    // develop() -> working-space PixelBuffer at a requested resolution
};

} // namespace pe
```

## Behavior & algorithms

The develop pipeline, in order:

```
raw sensor mosaic
  → black/white-level + linearization
  → demosaic (e.g. AHD/Markesteijn)        # reconstruct full RGB per pixel
  → white balance (camera-native multipliers + user temp/tint)
  → camera profile (sensor RGB → working RGB, via DCP/matrix + LUT)
  → exposure / tone (highlights, shadows, whites, blacks, curve) in linear
  → texture / clarity / dehaze (local contrast)
  → lens corrections (distortion, vignette, CA)
  → noise reduction + sharpening (detail)
  → encode to the working color space at document bit depth
```

Because the smart object stores `RawDevelopSettings`, changing any control
re-runs only the affected later stages where possible and re-rasterizes the
preview; the sensor data is never modified. A low-res proxy develops instantly for
interactive sliders; the full-res develop runs on commit/zoom.

## Interactions

- [Smart objects](11-smart-objects.md): raw is a smart-object source so settings
  stay live — the central integration.
- [Color management](15-color-management.md): camera profile + working-space encode.
- [Filter engine](12-filter-engine.md): "Camera Raw Filter" exposes the same
  pipeline as a [smart filter](12-filter-engine.md) on non-raw layers.
- [File I/O](20-file-io.md): raw formats are import-only; develop settings save in
  the native document (and sidecars where applicable).
- [Presets](25-presets-assets.md): develop settings are presettable/syncable.

## Performance, threading & GPU

- Demosaic and local-contrast stages are the costliest; run on workers, tile-aware
  with apron, and a [GPU](23-gpu-acceleration.md) path later.
- Proxy-resolution preview decouples slider latency from full develop.

## Edge cases & failure modes

- Unsupported camera/sensor → fall back to embedded JPEG preview + warn.
- Clipped highlights → highlight reconstruction; show clipping warning.
- X-Trans vs Bayer demosaic selection by sensor type.
- Extreme settings (huge exposure push) → noise; bounded ranges.

## Testing strategy

- Unit: tone/exposure math in linear; white-balance multiplier computation.
- Golden-image: develop a small synthetic mosaic with known settings to a known
  RGB result; settings round-trip through the native format unchanged.
- Determinism: fixed demosaic + settings produce identical output.

## Phasing

- **M8**: raw decode (LibRaw) + basic develop (WB, exposure, tone, profile) as a
  smart-object source; Camera Raw Filter shell.
- **M9**: clarity/texture/dehaze, lens corrections, NR/sharpen, HSL/color grading,
  batch + sync; GPU acceleration.

## Open questions

- Demosaic algorithm(s) to ship and quality/speed defaults.
- Sidecar strategy for non-destructive settings on the original raw file.
- How much of Camera Raw's UI to mirror vs simplify.

## References (relative links)

- [Glossary](../glossary.md) — Smart object, Working space, Bit depth.
- Sibling systems: [11 — Smart objects](11-smart-objects.md),
  [15 — Color management](15-color-management.md), [12 — Filter engine](12-filter-engine.md),
  [20 — File I/O](20-file-io.md), [25 — Presets](25-presets-assets.md),
  [22 — Performance](22-performance.md), [23 — GPU](23-gpu-acceleration.md).
- ADRs: [0004 — color management](../adr/0004-color-management.md).
