# ADR-0008: App shell as a library with a thin main

**Status:** Accepted

## Context

[ADR-0006](0006-headless-core-separation.md) split the codebase into a headless
`pe_core` engine and a Qt shell in `src/app`, and it made the engine fully
testable without a display. It said nothing about testing the shell.

In practice the shell was built as a bare `qt_add_executable`. Nothing in
`src/app` was linkable from any other target, so none of its sixteen translation
units had a single test. That was tolerable while the shell was thin glue, but it
stopped being tolerable once real logic moved in:

- `Theme` carries the palette that decides whether keyboard focus, the active
  tool, the selected row and the active tab are perceivable at all. Those ratios
  were computed by hand and had no regression guard.
- `buildStyleSheet` substitutes `@name@` placeholders from a hand-maintained
  table. Adding a color role without adding its row leaves a literal token in the
  stylesheet, which Qt silently ignores: the rule simply stops applying.
- `themeFromInt` folds an untrusted persisted setting.
- `IconUtil` and `TextRender` are pure enough to test directly.

The engine test target already proved the pattern works: `pe_core_tests` links
`PhotoEdit::Core` and runs in every lane. The shell needed the same affordance.

## Decision

Split `src/app` into a `pe_app` static library (aliased `PhotoEdit::App`) holding
every source except `main.cpp`, plus a thin `photoedit` executable whose only job
is to construct the application object and the main window.

Add a `pe_app_tests` target beside `pe_core_tests`, using the same
zero-dependency `pe_test.hpp` harness, registered with `add_test` and gated on
`PHOTOEDIT_BUILD_APP`.

This does not change the dependency direction. `app -> core` still holds, and
ADR-0006's rule that the engine must build with no Qt present is untouched: the
two headless CI lanes configure with `PHOTOEDIT_BUILD_APP=OFF`, where `pe_app`
does not exist and the gate skips the target.

## Consequences

**Positive**

- Everything in the shell is now reachable from a test. The first batch pins the
  WCAG contrast ratios for every theme, asserts the stylesheet leaves no
  unsubstituted token, and checks that an unknown persisted theme id folds.
- No CI change was required. The Windows lane already runs `ctest`, which picks
  up any registered target, so the shell tests execute there automatically.
- The shell gains a real public surface. `target_include_directories(pe_app
  PUBLIC ...)` makes the headers the contract, which discourages the shell from
  growing accidental cross-file coupling.
- Offscreen widget tests become possible later without further restructuring.

**Negative / costs**

- Qt resources compiled into a static library are dropped by the linker unless a
  linked translation unit references them. This bit immediately: after the split,
  `photoedit.exe` no longer contained the icon resources at all and the tool
  strip would have rendered as blank squares. The fix is an explicit
  `pe::app::initIconResources()` that `main` calls, guarded by a test that opens
  three bundled glyphs. The cost is one required call at startup.
- One more build target, and a slightly longer link for the executable.
- Static-library linkage means the shell tests link the whole shell even when a
  test only touches `Theme`.

## Alternatives considered

- **Move testable logic into `pe_core`.** Rejected: `Theme` is built from
  `QColor`, and ADR-0006 forbids Qt in the engine. Reproducing the palette in
  engine types would duplicate the values and let them drift, which is precisely
  the failure the tests exist to prevent.
- **A second test target that re-lists the shell sources.** Works with no
  restructuring, but the source list then lives in two places and drifts. This
  was tried informally twice while producing review screenshots and was annoying
  both times.
- **Golden-image comparison of rendered windows.** Catches more, but is brittle
  across Qt patch versions and display scaling, and it still needs the shell to
  be linkable first. Structural assertions were preferred; golden images remain
  available later.
- **Leave it untested and keep verifying by hand.** This is what happened for the
  palette work, and it does not scale across the remaining interface commits.
