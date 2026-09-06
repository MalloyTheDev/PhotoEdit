# Security Policy

## Reporting a vulnerability

Report privately through GitHub's
[private vulnerability reporting](https://github.com/MalloyTheDev/PhotoEdit/security/advisories/new)
rather than by opening a public issue. Please include the file that triggers it where
you can: a decoder bug is usually reproducible only from the exact bytes.

## What is in scope

The decoders are the deliberate attack surface. PhotoEdit reads PNG, JPEG, TIFF, WebP,
PSD and its own `.pedoc` format, and every one of those is untrusted input: a malicious
file should be rejected, never crash the process or execute anything.

In scope:

- memory safety in any decoder (`src/core/src/`: `Png.cpp`, `Jpeg.cpp`, `Tiff.cpp`,
  `WebP.cpp`, `Psd.cpp`, `NativeFormat.cpp`) reached from a crafted file;
- unbounded allocation from attacker-controlled dimension or length fields;
- integer overflow in a size or offset computation that leads to an out-of-bounds
  access;
- path traversal or unintended writes from a document's own contents.

Out of scope:

- crashes reached only through the public C++ API with arguments a file could not
  produce;
- resource exhaustion from a file the user deliberately opened at a size the engine
  documents as supported;
- findings in a third-party library that are already public upstream, unless PhotoEdit
  uses it in a way that makes the impact worse.

## Dependencies

The native libraries that touch untrusted bytes are pinned through
[`vcpkg.json`](vcpkg.json): `libpng`, `libjpeg-turbo`, `tiff`, `libwebp`, `lcms` and
`zlib`. Their versions come from the `builtin-baseline` commit in that file, which is
the single source of truth. Do not edit it by hand:

```bash
vcpkg x-update-baseline
```

That moves every dependency to the pinned vcpkg registry commit at once. Run it when
an advisory lands, then check the resulting version bumps against the manifest with
`vcpkg install --dry-run`.

GitHub's dependency graph does not parse `vcpkg.json`, so these libraries are not
watched automatically. What is automated:

- **GitHub Actions versions** are watched by Dependabot
  ([`.github/dependabot.yml`](.github/dependabot.yml)).
- **CI runs weekly on a schedule**, not only on push, so a toolchain or upstream
  package regression surfaces without waiting for someone to commit.
- **Static analysis** (`clang-tidy` with `clang-analyzer-*`) runs over the engine and
  the shell on every run.
- **ASan and UBSan** run over the engine and the shell on every run.

For the native libraries themselves, watch the upstream advisory feeds directly:
[libpng](https://github.com/pnggroup/libpng/security),
[libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo/security),
[libtiff](https://gitlab.com/libtiff/libtiff/-/issues),
[libwebp](https://github.com/webmproject/libwebp/security),
[Little CMS](https://github.com/mm2/Little-CMS/security) and
[zlib](https://github.com/madler/zlib/security).

## Supported versions

The project is pre-1.0 and ships no releases yet, so fixes land on `main` only.
