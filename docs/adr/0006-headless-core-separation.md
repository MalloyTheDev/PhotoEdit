# ADR-0006 — Headless engine core, UI as a client

**Status:** Accepted

## Context

Image-editing logic and UI logic have very different lifetimes, test strategies,
and portability needs. If they intertwine, the engine becomes untestable without
a display, un-scriptable, and bound to one UI toolkit and platform.

## Decision

Split the codebase into a **headless engine** (`pe_core`, pure C++, no Qt/UI/
network) and a **thin app shell** (`src/app`, Qt6) that is a *client* of the
engine. The dependency direction is strictly one-way: **app → core**. The core
must build and pass its full test suite with no Qt present and no display.

CI enforces this with a **Linux core lane** that compiles `pe_core` + tests with
no Qt installed and `-Werror`.

## Consequences

**Positive**
- The entire engine is **unit-testable headless**, deterministically, in every CI
  lane — the foundation of our correctness strategy.
- The engine is **scriptable and automatable** without a UI (actions, batch,
  server-side rendering).
- **Portability**: the hard, valuable code doesn't depend on the UI toolkit or OS,
  so a future macOS/Linux shell — or even a different toolkit — reuses it.
- De-risks the Qt dependency: if Qt ever had to change, the engine is unaffected.

**Negative / costs**
- A boundary to maintain: engine results must be converted to Qt types for
  display, and UI events to engine events. This glue is deliberate and small.
- Some duplication of small value types at the boundary (e.g. engine `Rgba8` vs
  `QColor`), by design.

## Alternatives considered

- **One integrated Qt application.** Faster to start; rejected because it would
  make the engine untestable headless, un-scriptable, and Qt/Windows-bound —
  contrary to the vision's "engine is headless" principle.
- **Core depends on Qt's non-GUI modules (QtCore) for convenience** (containers,
  strings, threading). Rejected: even QtCore would couple the engine to Qt's
  release cadence and types; the standard library suffices.

## Notes

This ADR is what makes [ADR-0001](0001-language-and-framework.md) (Qt) safe and
[ADR-0005](0005-command-history-model.md) (scripting/automation) natural. The
split is visible in the repo from M0: `src/core` has no Qt; `src/app` owns it.
