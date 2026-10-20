# Third-party notices

PhotoEdit itself is licensed under the GNU General Public License, version 3 or
later (see [LICENSE](LICENSE)). It links third-party libraries that carry their
own terms. This file records what they are and what each one obliges us to do
when we distribute a binary.

Nothing here overrides the libraries' own license texts. The authoritative text
for each package ships with it; see "Collecting the texts" below.

## Qt 6 (the one with real obligations)

| | |
| --- | --- |
| Modules | Qt Core, Qt Gui, Qt Widgets, Qt Svg |
| Terms we use | GNU Lesser General Public License, version 3 (LGPLv3) |
| Also offered as | GPL-3.0-only with the Qt GPL exception, or a commercial license from The Qt Company |

LGPLv3 is compatible with PhotoEdit's own GPLv3, and it permits distribution of
an application whose own code is under other terms, including proprietary terms.
It does, however, attach conditions to every binary we ship:

1. **Link Qt dynamically.** We already do: the Windows lane builds against the
   `x64-windows` (dynamic) vcpkg triplet and the app is deployed with
   `windeployqt`, so Qt arrives as separate DLLs. Switching to a static Qt
   triplet changes these obligations substantially and must not be done without
   revisiting this file.
2. **Ship the license text**, both LGPLv3 and the GPLv3 it incorporates by
   reference.
3. **Make the Qt source available**, either alongside the download or as a
   written offer, for the exact Qt version shipped.
4. **Let the user replace the Qt libraries** with their own compatible build.
   Dynamic linking satisfies this in practice; do not take steps that defeat it,
   such as signature-pinning the Qt DLLs.
5. **Honor the installation-information requirement** of LGPLv3 section 4 if the
   application is ever shipped on a locked-down device.

## Imaging and color libraries

All permissive. They require attribution and the reproduction of their license
text, and impose no copyleft obligation on PhotoEdit.

| Library | Role | Terms |
| --- | --- | --- |
| Little-CMS 2 (lcms2) | ICC color management | MIT |
| libpng | PNG import/export | PNG Reference Library license |
| libjpeg-turbo | JPEG import/export | A combination of IJG, BSD-3-Clause and Zlib terms, varying by component |
| libtiff | TIFF import/export | BSD-style libtiff license |
| libwebp | WebP import/export | BSD-3-Clause |
| zlib | Compression for PNG, PSD and the native format | Zlib license |

## Bundled assets

| Asset | Source | Terms |
| --- | --- | --- |
| Toolbar and panel icons | [Lucide](https://lucide.dev) | ISC, with portions from Feather under MIT |

The icon notice lives next to the files it covers, in
[`src/app/icons/LICENSE.txt`](src/app/icons/LICENSE.txt). Geometry is unmodified;
only the stroke color was changed for the dark theme.

## Planned dependencies, not yet linked

Recorded here so the licensing consequences are considered before the code
lands, not after.

- **LibRaw** (camera raw decode, M8+) is offered under LGPL-2.1 or CDDL-1.0, and
  its former commercial license no longer exists. Neither option is a drop-in
  for a GPLv3 project: CDDL-1.0 is GPL-incompatible, and LGPL-2.1 composes
  cleanly with GPLv3 only if the "or later" option is available. Confirm which
  terms the shipped version actually offers before adding it, and record the
  conclusion here.
- **HarfBuzz and FreeType** (text shaping, M8) are permissive, though FreeType
  is dual-licensed under the FreeType License or GPLv2 and we would be taking
  the former.

## Collecting the texts

vcpkg installs each port's authoritative copyright file, so the shipped notices
should be harvested rather than transcribed by hand:

```
%VCPKG_INSTALLATION_ROOT%/installed/x64-windows/share/<port>/copyright
```

Concatenate those into the notices file that accompanies a release, together
with the LGPLv3 and GPLv3 texts and the Qt source offer. Do not hand-maintain
copies of upstream license texts in this repository: they drift.
