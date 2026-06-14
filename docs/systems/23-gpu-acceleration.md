# 23 — GPU Acceleration

> Milestone: M2 (display path) · M5+ (filters) · M9 (AI) · Status: Spec

## Purpose

The GPU accelerates the work that is data-parallel and latency-sensitive: canvas
display compositing, zoom/pan/rotate, blur and transform previews, color
conversion for display, and (later) neural/AI inference. The CPU keeps the work
that is branchy, sequential, or I/O-bound: file decode, some filters, layer/brush
logic, [history](21-history-undo.md), compression, and scripting. PhotoEdit owns a
thin **Render Hardware Interface (RHI)** over Direct3D 12 (Windows first), Vulkan
(next), and a software fallback, per
[ADR-0002](../adr/0002-gpu-abstraction.md). The CPU reference path always exists
and defines correctness; GPU output must match within tolerance.

## Requirements

**Functional**

- A backend-agnostic RHI: textures, buffers, command submission, graphics and
  compute pipelines, synchronization.
- Backends: **D3D12** (primary), **Vulkan** (portability), **software fallback**.
- Upload changed [tiles](22-performance.md) to GPU textures; composite and display.
- Compute pipelines for data-parallel [filters](12-filter-engine.md) and
  [color conversion](15-color-management.md).
- Never *require* a GPU: software fallback keeps the app usable.

**Non-functional**

- `pe_core`, no Qt; the RHI presents into the [app shell](24-ui-workspace.md)'s
  canvas surface via a platform window handle.
- GPU results match the CPU reference within a documented tolerance (golden tests).

## Data model

```cpp
namespace pe {

struct TextureDesc { int width, height; PixelFormat format; bool renderTarget; };

class Rhi {
public:
    virtual ~Rhi() = default;
    virtual TextureHandle createTexture(const TextureDesc&) = 0;
    virtual BufferHandle  createBuffer(size_t bytes, BufferUsage) = 0;
    virtual void updateTexture(TextureHandle, Rect region, const void* data) = 0;

    virtual PipelineHandle createComputePipeline(const ShaderBytecode&) = 0;
    virtual PipelineHandle createGraphicsPipeline(const GraphicsDesc&) = 0;

    virtual CommandList begin() = 0;
    virtual void dispatch(CommandList&, PipelineHandle, uint32_t gx,uint32_t gy,uint32_t gz) = 0;
    virtual void submit(CommandList&) = 0;
    virtual void present(SwapchainHandle) = 0;
    virtual Caps caps() const = 0;       // memory, feature levels, fallback flag
};

} // namespace pe
```

## Behavior & algorithms

**Display compositing path:**

```
CPU layer/tile data changes (dirty region)
  → upload only the dirty tiles to their GPU textures (updateTexture)
  → run the composite pipeline over the visible tiles (blend stack on GPU)
  → convert the composite through the display profile (LUT/compute)
  → draw to the swapchain and present
```

Keeping CPU and GPU **synchronized** is the hard part: the GPU tile-texture cache
mirrors the CPU [tile cache](22-performance.md); a tile's GPU copy is invalidated
and re-uploaded when its CPU data changes. Immutable published tiles make this
race-free.

**Compute filters:** separable blur, convolution, unsharp mask, and color
transforms map to compute shaders dispatched per tile (with an apron). The same
operation has a CPU reference; a golden-image test asserts parity.

**Fallback:** when no GPU/feature support is present (`caps().softwareFallback`),
the software backend executes the same pipelines on the CPU — slower but identical
in result.

## Interactions

- [Canvas/rendering](02-canvas-rendering.md): the display consumer of the RHI.
- [Performance](22-performance.md): the GPU tile cache mirrors the CPU tile cache.
- [Filters](12-filter-engine.md): compute-shader acceleration with CPU parity.
- [Color management](15-color-management.md): display/output conversion on GPU.
- [Blend modes](04-blend-modes.md): GPU composite must match the CPU reference.
- [Generative AI](14-generative-ai.md): local model inference may use the GPU.

## Performance, threading & GPU

- Upload only dirty tiles; batch uploads per frame.
- Double/triple-buffer command submission; avoid CPU↔GPU stalls.
- Prefer compute for compositing large tile counts; graphics pipeline for the
  final present and overlays.

## Edge cases & failure modes

- Device lost / driver reset → recreate resources, fall back if needed.
- GPU OOM on huge documents → tile streaming, never allocate the whole frame.
- Float precision differences CPU vs GPU → tolerance in parity tests; pin critical
  math (e.g. clamp/rounding) consistently.
- HDR/wide-gamut display surfaces → correct swapchain format + conversion.

## Testing strategy

- Golden-image parity: composite and each GPU filter vs the CPU reference within
  tolerance, on the software backend in CI (no real GPU needed).
- Unit: RHI resource lifecycle on the software backend; dirty-tile upload tracking.
- Device-loss simulation recovers without data loss.

## Phasing

- **M2**: RHI + software fallback + D3D12 display path; dirty-tile upload; GPU
  composite for display.
- **M5+**: compute-shader filters and color conversion with CPU parity.
- **Later**: Vulkan backend; AI inference acceleration.

## Open questions

- Own RHI vs adopt Qt RHI as a backup (see [ADR-0002](../adr/0002-gpu-abstraction.md)).
- Shader authoring/toolchain (HLSL → SPIR-V cross-compile) and bytecode caching.
- Compositing entirely on GPU vs hybrid for very deep layer stacks.

## References (relative links)

- [01 — Master architecture](../01-master-architecture.md) — the RHI contract.
- [Glossary](../glossary.md) — RHI, Tile, Working space.
- Sibling systems: [02 — Canvas/rendering](02-canvas-rendering.md),
  [22 — Performance](22-performance.md), [12 — Filter engine](12-filter-engine.md),
  [15 — Color management](15-color-management.md), [04 — Blend modes](04-blend-modes.md),
  [14 — Generative AI](14-generative-ai.md).
- ADRs: [0002 — GPU abstraction](../adr/0002-gpu-abstraction.md).
