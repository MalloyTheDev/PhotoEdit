# Contributing to PhotoEdit

Thanks for your interest. Please read the licensing section below before you
open a pull request: it is not boilerplate, and it is the one thing that cannot
be fixed after the fact.

## Contribution terms

PhotoEdit is distributed under the GNU General Public License, version 3 or
later, and the copyright is held by a single author. That combination is what
keeps two things simultaneously true: the program cannot be taken closed by
anyone, and the author can still offer it on separate commercial terms.

A contribution whose copyright is held by someone else breaks the second half.
The project cannot include such a contribution in a commercially licensed build
or grant exceptions covering it, and GPLv3 provides no mechanism to relicense
it later. To keep that option open, every contribution is accepted on these
terms:

By submitting a contribution, you certify that:

1. You wrote it, or you have the right to submit it under these terms.
2. You grant the project's copyright holder a perpetual, worldwide,
   non-exclusive, royalty-free, irrevocable license to use, reproduce, modify,
   prepare derivative works of, publicly display, sublicense and distribute your
   contribution and such derivative works, **under the GNU General Public
   License version 3 or later and under any other license terms the copyright
   holder chooses**, including commercial terms.
3. You retain your own copyright in your contribution. This grant is a license,
   not an assignment, and it does not limit what you may do with your own work
   elsewhere.
4. Your contribution is provided without warranty, to the extent permitted by
   law.

Record your agreement by signing off each commit:

```
git commit -s
```

which appends a `Signed-off-by` trailer. A pull request whose commits are not
signed off cannot be merged.

If your employer owns the copyright in work you do, get their permission before
contributing, or contribute only work they do not own.

## Before you open a pull request

The engineering expectations are documented, not implied. Read:

- [docs/04-coding-standards.md](docs/04-coding-standards.md) for the code bar,
  including the testing and golden-image rules.
- [docs/05-build-and-tooling.md](docs/05-build-and-tooling.md) for the build,
  the presets, and the local validation workflow.
- [docs/01-master-architecture.md](docs/01-master-architecture.md) for the layer
  boundaries. In particular, `src/core` must never include or link Qt.

Run the local gate before pushing. At minimum, on Linux:

```
cmake --preset linux-core-dev
cmake --build build/linux-core-dev
ctest --preset linux-core-dev
find src tests -type f \( -name '*.cpp' -o -name '*.hpp' \) \
  | sort | xargs clang-format --dry-run --Werror --style=file
```

The engine lanes build with `-Werror`, so a warning is a build failure. CI runs
the same gate plus a dependency-free build, sanitizers, and the Windows app
build.

## Scope

Small, focused pull requests get reviewed. A change that rewrites working code
to a different style, or bundles an unrelated refactor with a fix, generally does
not. If you are planning something substantial, open an issue first so the
design can be agreed before you spend the effort.
