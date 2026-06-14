# 14 — Generative AI System

> Milestone: M9 · Status: Spec

## Purpose

The generative system adds and removes content from text prompts and image
context: Generative Fill, Generative Expand, Generate Image, AI Remove,
Generative Upscale, and Harmonize. It is "a whole product inside the product."
PhotoEdit supplies the **pipeline and integration** — selection and context
extraction, masking, request orchestration, variation management, non-destructive
generative layers, and provenance — behind a **pluggable model provider** that
may run locally or in the [cloud](28-cloud-account.md). Per the
[vision](../00-vision-and-scope.md), we do not build a foundation model; we
integrate one.

## Requirements

**Functional**

- Generative Fill/Remove: from a [selection](07-selection-system.md) + prompt,
  produce content (or remove content) confined to the selection.
- Generative Expand: extend the [canvas](01-document-system.md) and fill the new
  area coherently with the existing image.
- Generate Image: create an image from a prompt (optionally reference images).
- Generative Upscale and Harmonize (color/light match of pasted content).
- Multiple **variations** per request; the user cycles and picks one.
- Results land as a **non-destructive generative layer** with a mask.
- **Content credentials / provenance** metadata recorded on generated content.
- Provider selection (on-device vs cloud), graceful fallback and error handling,
  cancellation.

**Non-functional**

- `pe_core` defines the provider interface and pipeline (no Qt); the prompt/
  variation UI lives in the [app shell](24-ui-workspace.md).
- Requests run **off the document thread**; the UI stays responsive with progress
  and cancel.
- Uploading content to a cloud provider requires explicit consent (privacy).

## Data model

```cpp
namespace pe {

struct GenerationRequest {
    enum Kind { Fill, Remove, Expand, GenerateImage, Upscale, Harmonize } kind;
    PixelBuffer contextRGBA;   // cropped image context around the target
    PixelBuffer mask;          // where to generate (white) vs keep (black)
    std::string prompt;
    std::vector<PixelBuffer> references;  // optional reference images
    int variationCount = 3;
    uint64_t seed = 0;         // for reproducibility/tests
    Size outputSize;
};

struct Variation { PixelBuffer rgba; ProvenanceInfo provenance; float score; };

struct GenerationResult {
    std::vector<Variation> variations;
    std::optional<Error> error;
};

// The pluggable boundary: local or cloud implementations satisfy this.
class GenerativeProvider {
public:
    virtual ~GenerativeProvider() = default;
    virtual std::string id() const = 0;            // e.g. "local.sd", "cloud.firefly"
    virtual bool supports(GenerationRequest::Kind) const = 0;
    virtual void generate(const GenerationRequest&,
                          ProgressSink&, Cancel&,
                          std::function<void(GenerationResult)> done) = 0;
};

// A Layer kind that remembers how it was generated.
struct GenerativeLayer { /* pixels + mask + request + chosenVariation + provenance */ };

} // namespace pe
```

## Behavior & algorithms

The canonical flow (Generative Fill):

```
1. User makes a selection and enters a prompt.
2. Engine crops CONTEXT: the selection bounds expanded by margin, clamped to
   canvas, plus the selection as the generation mask.
3. Build a GenerationRequest (context, mask, prompt, references, variations, seed).
4. Dispatch to the active GenerativeProvider on a worker; stream progress.
5. Provider returns N variations (each masked back into the context region).
6. Insert a GenerativeLayer above the target, masked to the selection, holding all
   variations; show the first.
7. User cycles variations / re-rolls (new seed) / edits the mask.
8. Provenance (model id, prompt, content credentials) is stamped on the layer and
   carried into export metadata.
```

The result is non-destructive: the underlying pixels are untouched, the generative
layer can be masked, blended, deleted, or regenerated. Expand first grows the
canvas, then fills the new margin with the same machine.

## Interactions

- [Selection](07-selection-system.md) & [masks](06-masks.md): define and confine
  the generated region.
- [Layer system](03-layer-system.md): output is a generative layer in the tree.
- [Retouching](13-retouching.md): AI Remove is the generative path of the Remove
  tool.
- [Cloud/account](28-cloud-account.md): routes cloud providers, auth, entitlement.
- [File I/O](20-file-io.md): persists provenance/content-credentials on export.
- [History](21-history-undo.md): applying/choosing a variation is a command.

## Performance, threading & GPU

- Inference is the cost; it runs in the provider (local [GPU](23-gpu-acceleration.md)
  or remote). The engine only does context crop, mask prep, and compositing.
- Context size is bounded to control latency/cost; large targets tile or upscale.
- All provider calls are async, cancellable, and retried/fallback on failure.

## Edge cases & failure modes

- Provider offline / quota exhausted → clear error, offer fallback provider or
  classical [content-aware](13-retouching.md).
- No selection for Fill → treat whole layer or prompt for one.
- Cloud consent declined → restrict to on-device providers.
- Non-deterministic output → tests pin a seed and a stub provider.
- Harmful-content / policy refusals from a provider → surface, do not crash.

## Testing strategy

- Unit (with a **stub provider**): context-crop bounds, mask construction, expand
  geometry, variation insertion as a masked layer, provenance stamping.
- Integration: cancellation mid-request leaves the document unchanged.
- The real model is never required in CI; tests use a deterministic fake.

## Phasing

- **M9 early**: provider interface + stub; Generative Fill/Remove and Expand
  pipeline; generative layer + variations + provenance.
- **M9 later**: Generate Image, Upscale, Harmonize, reference images, multiple
  provider backends (local + cloud).

## Open questions

- On-device model packaging/size and the local runtime (ONNX/Direct-ML?).
- Content-credentials standard to adopt (C2PA?) and how it survives export.
- Caching/My-policy for context uploads; redaction of sensitive regions.

## References (relative links)

- [00 — Vision & scope](../00-vision-and-scope.md) — AI is integrated, not built
  from scratch; non-goals.
- [01 — Master architecture](../01-master-architecture.md) — extensibility, `Command`.
- [Glossary](../glossary.md) — Non-destructive, Mask, Selection.
- Sibling systems: [07 — Selection](07-selection-system.md), [06 — Masks](06-masks.md),
  [13 — Retouching](13-retouching.md), [03 — Layer system](03-layer-system.md),
  [28 — Cloud/account](28-cloud-account.md), [20 — File I/O](20-file-io.md),
  [21 — History & undo](21-history-undo.md), [23 — GPU](23-gpu-acceleration.md).
