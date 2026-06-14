# 29 — Plugin / Extension System

> Milestone: M10 · Status: Spec

## Purpose

A professional editor needs extension points so third parties can add filters,
file-format importers/exporters, panels, automation commands, AI tools, asset
browsers, and retouching tools. PhotoEdit's headless, command-based architecture
makes this natural: **plugins register against the same contracts the built-ins
use** — a plugin filter implements [`Filter`](12-filter-engine.md), a plugin tool
implements [`Tool`](09-tool-system.md), a plugin format implements
[`ImageImporter`/`ImageExporter`](20-file-io.md), a plugin command implements
[`Command`](21-history-undo.md). The hard parts are **API/ABI stability** (once
developers depend on it, you cannot break it) and **security** (sandboxing
untrusted code with capability-limited access).

## Requirements

**Functional**

- Register extensions of every built-in contract: filters, tools, file formats,
  commands/automation, panels, AI providers, asset sources.
- A discovery/manifest mechanism and an in-app extension registry.
- Versioned API with compatibility guarantees and capability negotiation.
- Access to documents and pixel buffers through a controlled, permissioned API.

**Non-functional**

- A **stable boundary**: a versioned C ABI (or a narrow, versioned C++ interface)
  so plugins built against one version keep working.
- **Security**: sandbox untrusted plugins; capability-limited document/pixel
  access; resource and time limits; clear failure isolation (a crashing plugin
  must not take down the app).
- Engine remains [headless](../adr/0006-headless-core-separation.md); panels
  integrate via the [app shell](24-ui-workspace.md)'s panel API.

## Data model

```cpp
namespace pe {

struct PluginManifest {
    Uuid id; std::string name; Version apiVersion;
    std::vector<ExtensionKind> provides;     // Filter, Tool, Format, Command, Panel, AIProvider...
    Permissions requested;                    // document read/write, network, fs, gpu
};

// The host hands the plugin a capability-scoped view, not raw engine internals.
class PluginHostApi {
public:
    void registerFilter(std::unique_ptr<Filter>);
    void registerTool(std::unique_ptr<Tool>);
    void registerFormat(std::unique_ptr<ImageImporter>, std::unique_ptr<ImageExporter>);
    void registerCommand(CommandFactory);
    void registerPanel(PanelFactory);
    void registerAIProvider(std::unique_ptr<GenerativeProvider>);
    // Permissioned access only:
    DocumentView document(Capability);        // throws/denies without capability
};

class Plugin {
public:
    virtual PluginManifest manifest() const = 0;
    virtual void onLoad(PluginHostApi&) = 0;
    virtual void onUnload() = 0;
};

} // namespace pe
```

## Behavior & algorithms

- **Discovery & load:** scan plugin directories, read manifests, check
  `apiVersion` compatibility, prompt for requested `Permissions`, then load and
  call `onLoad`, where the plugin registers its extensions in the host registry.
- **Dispatch:** the engine treats registered extensions identically to built-ins —
  a plugin filter appears in the filter list and runs through the same
  [filter engine](12-filter-engine.md) pipeline; a plugin command is replayable in
  [actions](26-automation.md).
- **Sandboxing:** untrusted plugins run with capability-limited access (no ambient
  document/network/fs); higher-risk plugins run out-of-process so a crash or hang
  is isolated and killable. Document/pixel access goes through `DocumentView`,
  which enforces the granted capabilities and tile-scoped reads/writes.
- **Stability:** the ABI boundary is versioned; new capabilities are additive;
  removals go through deprecation. A compatibility test suite guards the boundary.

## Interactions

- [Filter engine](12-filter-engine.md), [tools](09-tool-system.md),
  [file I/O](20-file-io.md), [automation](26-automation.md),
  [generative AI](14-generative-ai.md), [UI/workspace](24-ui-workspace.md): the
  contracts plugins extend.
- [History](21-history-undo.md): plugin commands are undoable like any other.

## Security & sandboxing

- Capability model: each permission (document-read, document-write, network, fs,
  gpu) is granted explicitly; the default is none.
- Out-of-process isolation for untrusted/native plugins; resource (memory/time)
  limits; signed plugins for a trusted tier.
- A misbehaving plugin is disabled with a clear report; the document is protected.

## Edge cases & failure modes

- API-version mismatch → refuse to load with guidance, never crash.
- Plugin crash/hang → isolate (out-of-process), terminate, disable, report.
- Permission escalation attempts → denied by the capability layer.
- Two plugins claiming the same format/id → deterministic precedence + warning.

## Testing strategy

- A reference sample plugin (a trivial filter + a format) loads, registers, runs,
  and unloads cleanly.
- ABI compatibility tests: a plugin built against vN loads on vN+patch.
- Sandbox tests: a plugin without a capability is denied that access; an
  out-of-process crash is contained.

## Phasing

- **M10**: extension registry + manifest, versioned boundary, in-process filter/
  format/command plugins, permission prompts, sample plugin + SDK docs.
- **Post-M10**: out-of-process sandboxing, signing/trust tiers, panel and AI-
  provider plugins, marketplace concerns.

## Open questions

- C ABI vs versioned C++ interface for the stable boundary (portability vs
  ergonomics).
- In-process vs always out-of-process default, and the IPC surface.
- Signing/trust model and distribution.

## References (relative links)

- [01 — Master architecture](../01-master-architecture.md) — extensibility; the
  built-in contracts plugins reuse.
- [Glossary](../glossary.md) — Command, App shell.
- Sibling systems: [12 — Filter engine](12-filter-engine.md),
  [09 — Tool system](09-tool-system.md), [20 — File I/O](20-file-io.md),
  [26 — Automation](26-automation.md), [14 — Generative AI](14-generative-ai.md),
  [24 — UI/workspace](24-ui-workspace.md).
- ADRs: [0006 — headless core](../adr/0006-headless-core-separation.md).
