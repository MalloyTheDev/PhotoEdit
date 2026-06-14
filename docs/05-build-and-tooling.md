# 05 — Build & Tooling

## Build system

PhotoEdit uses **CMake (≥ 3.24)** with **Ninja** and **CMake presets**. There is
no IDE-specific project file checked in; any IDE that understands CMake presets
(Visual Studio, VS Code, CLion, Qt Creator) works.

### Targets

| Target | Kind | Depends on | Builds Qt? |
| --- | --- | --- | --- |
| `pe_core` (`PhotoEdit::Core`) | static lib | — | no |
| `photoedit` | executable (app) | `pe_core`, Qt6 | yes |
| `pe_core_tests` | test executable | `pe_core` | no |

### Options

| Option | Default | Meaning |
| --- | --- | --- |
| `PHOTOEDIT_BUILD_APP` | `ON` | Build the Qt6 app shell |
| `PHOTOEDIT_BUILD_TESTS` | `ON` | Build the headless unit tests |
| `PHOTOEDIT_WERROR` | `OFF` | Treat warnings as errors (CI core lane turns this on) |

### Presets

- **`windows-msvc`** — the primary target. Uses the vcpkg toolchain (needs
  `VCPKG_ROOT`) and builds the full app + tests on MSVC.
- **`linux-core-dev`** — the engine fast-loop and CI gate. Builds `pe_core` +
  tests only, no Qt, with `-Werror`. This is the quickest way to develop and
  validate engine logic.

```bash
# Engine core (any platform, no Qt):
cmake --preset linux-core-dev
cmake --build build/linux-core-dev
ctest --preset linux-core-dev

# Full app (Windows):
cmake --preset windows-msvc
cmake --build --preset windows-msvc
ctest --preset windows-msvc
```

## Dependencies — vcpkg (manifest mode)

`vcpkg.json` pins the native dependencies. With `VCPKG_ROOT` set, the
`windows-msvc` preset wires in the toolchain automatically and vcpkg installs the
manifest on configure.

Policy: **add a dependency only when code uses it.** The manifest currently lists
the near-term set (Qt, lcms2, libpng, libjpeg-turbo, tiff, libwebp, zlib); we add
raw/text/etc. as those milestones arrive. This keeps CI fast and the graph honest.

## Continuous integration

`.github/workflows/ci.yml` runs two lanes on every push/PR:

1. **`core-linux`** — configures `linux-core-dev`, builds, runs `ctest`. Fast,
   `-Werror`, no Qt/GPU/display. **This is the merge gate.**
2. **`app-windows`** — sets up MSVC + Qt6 (aqtinstall), builds the full app +
   tests, runs `ctest`. Validates the primary platform.

Planned additions as the codebase grows:
- A **sanitizer lane** (ASan/UBSan) on the core.
- A **clang-format check** and a **clang-tidy** lane.
- **Golden-image** comparison artifacts uploaded on failure.

## Formatting & static analysis

- **`clang-format`** — the repo ships a `.clang-format`; it is authoritative.
  Run it before committing; CI will check it.
- **`clang-tidy`** — a curated rule set (bugprone, performance, modernize,
  readability) runs advisory first, then becomes enforced.
- **Warnings** — `/W4 /permissive-` (MSVC) and `-Wall -Wextra -Wpedantic`
  (GCC/Clang); the core lane promotes them to errors.

## Local developer workflow

1. Edit engine code in `src/core`.
2. `cmake --build build/linux-core-dev && ctest --preset linux-core-dev` for a
   sub-second feedback loop.
3. Add/extend tests in `tests/` alongside the change.
4. For UI work, build the `windows-msvc` preset (or a local Qt-enabled config).

## Directory layout

```
CMakeLists.txt          top-level build
CMakePresets.json       windows-msvc, linux-core-dev
vcpkg.json              native dependency manifest
.github/workflows/      CI
src/core/               pe_core: headless engine (CMakeLists, include/, src/)
src/app/                photoedit: Qt6 app shell
tests/                  pe_core_tests + pe_test.hpp harness
docs/                   this documentation set
```

## Versioning

`pe::Version` (`src/core/include/pe/core/Version.hpp`) is the single source of
truth, surfaced in the window title and stamped into saved documents. We follow
semantic-ish versioning for the app; the **document format** carries its own
independent version number (see the file-format and document system specs).
