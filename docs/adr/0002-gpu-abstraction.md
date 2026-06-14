# ADR-0002 — Own a thin RHI; Direct3D 12 first, Vulkan next

**Status:** Accepted

## Context

The editor needs the GPU for display compositing, zoom/pan/rotate, transform and
filter previews, color conversion for display, and (later) AI acceleration. We
target Windows first but must not foreclose macOS/Linux. We also must never make
a GPU *mandatory* — the app should run (slower) on weak hardware.

Options range from binding directly to one native API, to adopting a third-party
abstraction (Qt RHI, bgfx, Dawn/wgpu-native), to writing our own thin layer.

## Decision

Define and own a **thin Render Hardware Interface (RHI)** inside `pe_core`:
textures, buffers, command submission, and compute/shader dispatch — the minimum
the compositor and filters need. Implement backends in priority order:

1. **Direct3D 12** (Windows, primary).
2. **Software fallback** (correctness floor; also the CPU reference path).
3. **Vulkan** (portability, second platform wave).

The **CPU reference path defines correctness**; every GPU backend must match it
within a documented tolerance, enforced by golden-image tests.

## Consequences

**Positive**
- A small surface we fully control, tuned to our tile/compositor model rather than
  a general-purpose engine's assumptions.
- Windows users get a first-class D3D12 path; other platforms come via Vulkan
  without touching engine code above the RHI.
- The software fallback guarantees the app is always usable and gives us a golden
  reference.

**Negative / costs**
- We maintain GPU backends ourselves (synchronization, resource lifetimes,
  pipeline state) — real, ongoing complexity.
- Keeping CPU and GPU results within tolerance requires disciplined testing.

## Alternatives considered

- **Bind directly to D3D12 only.** Simplest short-term, but bakes Windows-only
  assumptions throughout and complicates the later port. Rejected.
- **Adopt Qt RHI.** Tempting since we already use Qt, and it abstracts D3D/Vulkan/
  Metal. Viable fallback, but couples our engine's GPU path to Qt (violating the
  headless-core rule) and to Qt's release cadence/abstractions. Kept as a backup
  if owning the RHI proves too costly.
- **wgpu-native / Dawn (WebGPU).** Clean, portable, modern. Heavier dependency and
  an extra translation layer; revisit if our hand-written backends become a
  maintenance burden.

## Notes

The RHI lives below the compositor and filter engine (see
[systems/23-gpu-acceleration.md](../systems/23-gpu-acceleration.md)). Engine code
above the RHI is GPU-API-agnostic.
