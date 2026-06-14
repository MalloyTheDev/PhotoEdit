# 26 — Automation: Actions, Batch & Scripting

> Milestone: M10 · Status: Spec

## Purpose

PhotoEdit is used in production pipelines, so it must record and replay work:
Actions, Batch processing, Droplets, an Image Processor, and a Scripting host,
plus variables/data-driven graphics. The key insight from
[ADR-0005](../adr/0005-command-history-model.md) is that **every edit is a
serializable [command](21-history-undo.md)**, so an Action is just a recorded list
of commands — which is why good undo architecture also delivers automation almost
for free. Because the engine is [headless](../adr/0006-headless-core-separation.md),
all of this runs without a UI.

## Requirements

**Functional**

- **Actions**: record a sequence of commands, replay on the current document, with
  per-step toggles and modal pauses; organize in sets.
- **Batch**: run an action over many files (open → run → save/export → close).
- **Droplets**: a batch packaged as a drag-and-drop executable.
- **Image Processor**: convert/resize/save many files in one pass.
- **Scripting host**: drive the engine's command API from a scripting language;
  external automation (COM on Windows; Apple Events on macOS later).
- **Variables / data-driven graphics**: bind document elements to a data source to
  generate many outputs from one template.

**Non-functional**

- `pe_core` runs actions/batch headlessly; the recorder/editor UI lives in the
  [app shell](24-ui-workspace.md).
- Deterministic replay; clear error handling per file in batch (continue-on-error).

## Data model

```cpp
namespace pe {

// A recorded, replayable step = a serialized command descriptor.
struct ActionStep {
    std::string commandId;        // matches a registered command factory
    Blob params;                  // serialized Command params
    bool enabled = true;
    bool modalPause = false;      // stop for user input on replay
};

struct Action { std::string name; std::vector<ActionStep> steps; };
struct ActionSet { std::string name; std::vector<Action> actions; };

struct BatchJob {
    Action action;
    std::vector<Path> inputs;     // or a folder + glob
    SaveOptions output;           // format, naming, destination
    bool continueOnError = true;
};

// Bind the engine's command/registry API to a scripting runtime.
class ScriptingHost {
public:
    virtual void run(const Script&, Document&, ResultSink&) = 0;
};

} // namespace pe
```

## Behavior & algorithms

**Recording** taps the same [history](21-history-undo.md) pipeline: as commands are
pushed, the recorder appends their `serialize()` output as `ActionStep`s.

**Replay** reconstructs each command from its `commandId` + params via a command
registry and executes it against the target document:

```
runAction(action, doc):
    for step in action.steps where step.enabled:
        cmd = registry.create(step.commandId, step.params)
        if step.modalPause: awaitUserAdjust(cmd)
        doc.history().push(cmd)         # executes; remains undoable
```

**Batch** loops files, each in a fresh document, headless:

```
for file in batch.inputs:
    doc = open(file)                    # off-thread I/O
    try: runAction(batch.action, doc)
    catch e: if !continueOnError: stop; else log(file, e); continue
    save/export(doc, batch.output); close(doc)
```

Example action *Resize and Export*: open → resize to 2048 px wide → unsharp mask →
convert to sRGB → export JPEG q85 → close.

## Interactions

- [History](21-history-undo.md): the source of serializable commands; the same
  command registry powers both.
- [File I/O](20-file-io.md): batch open/save/export; export presets from
  [presets](25-presets-assets.md).
- [Plugins](29-plugin-extension.md): can register new commands usable in actions.
- [Cloud/account](28-cloud-account.md): batch over cloud documents (later).

## Performance, threading & GPU

- Batch parallelizes across files (each its own document) bounded by the
  [worker pool](22-performance.md) and memory budget.
- Headless replay skips UI/preview overhead entirely.

## Edge cases & failure modes

- A recorded command whose target no longer exists (layer deleted) → skip with a
  clear log entry; don't abort the action unless configured.
- Non-deterministic commands (a generative call) → record parameters incl. seed;
  document that results may vary.
- Version drift: an action recorded on an older schema → migrate step params.
- Modal pauses in unattended batch → either auto-continue with defaults or fail
  per policy.

## Testing strategy

- Unit: record→serialize→deserialize→replay reproduces the same document state.
- Batch over a temp folder of synthetic files produces expected outputs and a
  correct per-file error log on an injected failure.
- Determinism: replaying an action twice yields identical results (seeded).

## Phasing

- **M10**: actions (record/play/sets), batch, image processor, droplets, command
  registry + serialization, COM/scripting host.
- **Post-M10**: variables/data-driven graphics; richer scripting API surface.

## Open questions

- Scripting language(s) to expose (JavaScript? Python?) and the API surface.
- Forward/backward compatibility guarantees for serialized action params.
- Sandboxing untrusted droplets/scripts (overlaps with [plugins](29-plugin-extension.md)).

## References (relative links)

- [01 — Master architecture](../01-master-architecture.md) — extensibility.
- [Glossary](../glossary.md) — Command, History.
- Sibling systems: [21 — History](21-history-undo.md), [20 — File I/O](20-file-io.md),
  [25 — Presets](25-presets-assets.md), [29 — Plugins](29-plugin-extension.md),
  [28 — Cloud/account](28-cloud-account.md).
- ADRs: [0005 — command/history model](../adr/0005-command-history-model.md),
  [0006 — headless core](../adr/0006-headless-core-separation.md).
