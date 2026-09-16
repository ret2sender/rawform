# Third-Party Notices

Rawform is distributed as a whole under the **GNU General Public License
v3.0 or later** (see [`LICENSE`](LICENSE)). This applies to both components
of the repository: the Qt / QML application (`ui/`) and the standalone audio
engine (`audio/`).

This file lists the third-party components bundled with or linked by the
project, grouped by which component depends on them, followed by a section
per packaged distribution describing what each one actually ships. Every
listed open-source license is compatible with GPLv3; the one proprietary item
(Segoe Fluent Icons, below) is not bundled and is called out separately.

---

## Application (`ui/`)

### Linked libraries

| Component | License | Project |
|-----------|---------|---------|
| Qt 6 (Core, Gui, Quick, QuickControls2, Svg, Concurrent) | LGPL v3 / GPL v3 | https://www.qt.io |
| TagLib 2.x | LGPL v2.1 / MPL 1.1 (dual) | https://taglib.org |
| yaml-cpp | MIT | https://github.com/jbeder/yaml-cpp |

Qt is used under the LGPLv3 option and linked dynamically; distributing the
application under GPLv3 satisfies those terms. Qt Svg is linked explicitly so
the deployment tools ship the SVG image plugin the icons depend on. TagLib is
used under its LGPL v2.1 option, likewise subsumed by GPLv3 distribution.

### Bundled fonts

The full license text for each font ships alongside the font files in
`ui/Sources/fonts/`.

- **Inter** - `ui/Sources/fonts/Inter-*.ttf` (variable fonts, upright and
  italic)
  Copyright (c) 2016 The Inter Project Authors (https://github.com/rsms/inter)
  SIL Open Font License, Version 1.1 - see `ui/Sources/fonts/Inter_OFL.txt`.

- **JetBrains Mono** - `ui/Sources/fonts/JetBrainsMono-*.ttf` (variable
  fonts, upright and italic)
  Copyright 2020 The JetBrains Mono Project Authors
  (https://github.com/JetBrains/JetBrainsMono)
  SIL Open Font License, Version 1.1 - see
  `ui/Sources/fonts/JetBrainsMono_OFL.txt`.

### Caption glyphs transcribed from the Breeze window decoration

The caption glyphs under `ui/Sources/icons/breeze/` (close, minimize,
maximize, restore, and their `_hover` variants) are a one-to-one
transcription of the geometry the **Breeze window decoration** paints in
`kdecoration/breezebutton.cpp` (https://invent.kde.org/plasma/breeze),
used under the **GPL v2.0 or later**. The "or later" option makes these
files compatible with distribution of the application under GPLv3. As
shipped they are expressed as SVG stroke primitives on an 18x18 canvas
(1.01px pen, round caps, no fill), with the stroke color set to the
application palette (rest and hover variants); each file's header comment
records the derivation. The SVG files are their own corresponding source.

Copyright: 2014 Martin Graesslin, 2014 Hugo Pereira Da Costa, and the KDE
Breeze contributors.

### Icon artwork derived from Breeze icons

The menu arrow `ui/Sources/icons/app/dialogs/menu_arrow.svg` comes from
**Breeze icons** by the KDE community
(https://develop.kde.org/frameworks/breeze-icons/), used under the
**LGPL v3.0 or later**. The SVG file as shipped is modified from its Breeze
original: geometry sanitized and re-expressed and colors adapted to the
application palette (the file's header comment records its specific
derivation). The SVG file is its own corresponding source. Distribution of
the application under GPLv3 satisfies the LGPLv3 terms for this file.

Copyright: The Breeze icons authors, KDE community.

### Window-control glyphs (Segoe Fluent Icons)

The window-control glyphs in `ui/Sources/icons/windows/` reproduce the
window-chrome glyphs of Microsoft's **Segoe Fluent Icons**
(Copyright Microsoft Corporation,
https://learn.microsoft.com/en-us/windows/apps/design/style/segoe-fluent-icons-font)
as vector outlines, so that the custom title bar matches native Windows
chrome. The font itself is not bundled with Rawform; the shipped SVG files
contain only the glyph geometry (minimize bar, maximize square, restore
squares, close cross) re-expressed as project SVG paths. Segoe Fluent Icons
is distributed by Microsoft under its own license terms rather than an
open-source license; this entry documents provenance.

### Logo wordmark

The "rawform" wordmark is a vector outline set in
**Bricolage Grotesque** (Copyright 2019 The Bricolage Grotesque Project
Authors, https://github.com/ateliertriay/bricolage), licensed under the
SIL Open Font License, Version 1.1. The OFL permits creating logos and
artwork with a licensed font and does not extend its terms to the rendered
outlines, so the wordmark itself is covered by the project license and
carries no third-party obligation. This entry is provenance documentation,
not a license requirement. The font itself is not bundled with Rawform.

### Original assets

The following are original artwork, covered by the project license, and
carry no third-party obligation:

- the caption-control icons in `ui/Sources/icons/default/` (an original
  variation of the maximize / minimize / close / restore set);
- the tool-dialog close glyph
  `ui/Sources/icons/app/dialogs/tool_dialog_close_x.svg`;
- the transport-control glyphs in `ui/Sources/icons/app/track_controls/`;
- the application logo mark (the waveform glyph), shipped as
  `ui/Sources/icons/app/rawform.svg`, the derived `rawform.icns` and
  `rawform.icon/` bundle, and the raster renderings in `ui/Sources/images/`.

### Spectrum analysis

The spectrum view is driven by a hand-rolled Hann window and radix-2 FFT in
`audio/src/SpectrumAnalyzer.cpp`. It links no FFT library (no FFTW, no
KISS FFT) and carries no third-party obligation.

---

## Audio engine (`audio/`)

### Linked libraries

| Component | License | Project |
|-----------|---------|---------|
| libsndfile | LGPL v2.1 or later | https://libsndfile.github.io/libsndfile/ |
| libFLAC | BSD 3-Clause | https://xiph.org/flac/ |
| libmpg123 | LGPL v2.1 | https://www.mpg123.de |
| libebur128 | MIT | https://github.com/jiixyj/libebur128 |
| PipeWire client library (`libpipewire-0.3`, Linux only) | MIT | https://pipewire.org |
| FFmpeg (optional; see below) | LGPL v2.1+ / GPL, build-dependent | https://ffmpeg.org |

Note on FLAC: the **libFLAC library**, which is what Rawform links, is
BSD 3-Clause. The `flac` command-line tools distributed by the same project
are GPL, but Rawform does not link or ship them.

### Platform audio backends

On Linux the engine's output sink is PipeWire. It links `libpipewire-0.3`
(MIT) when the development package is present at configure time, and builds
without an audio output otherwise. The library is a system component on
every supported Linux target and inside the Flatpak runtime; Rawform does
not bundle it.

On macOS the engine links the CoreAudio, AudioToolbox, AudioUnit and
CoreFoundation system frameworks. These are system libraries in the GPLv3
sense and carry no notice obligation.

### FFmpeg

FFmpeg is an **optional** dependency, controlled by the `RAWFORM_FFMPEG`
CMake option (`auto` / `on` / `off`). When enabled, the engine links four
libraries: libavformat, libavcodec, libavutil and libswresample.

FFmpeg's own license depends on how it was configured. It is LGPL v2.1 or
later by default, but building with `--enable-gpl` makes the result GPL, and
`--enable-version3` raises the (L)GPL version floor to 3. Both packaged
builds of Rawform (the macOS bundle and the Linux Flatpak, detailed below)
use FFmpeg configured with `--enable-gpl --enable-version3`, so the linked
libraries are effectively **GPL v3**. This is compatible with, and one of
the reasons for, Rawform's own GPLv3 license.

A build configured with `RAWFORM_FFMPEG=off` links and bundles no FFmpeg at
all, and the FFmpeg parts of the distribution sections below do not apply
to it.

### Codec patents

Software licenses and codec patents are separate matters. MP3 decoding
(libmpg123) is unencumbered; the relevant patents expired in 2017. Some
formats reachable only through the optional FFmpeg fallback (for example AAC)
have historically been patent-encumbered; nothing in this file makes any
claim about patent status, which is jurisdiction- and time-dependent.

### Test fixtures

The embedded audio fixtures under `audio/tests/fixtures/` (`*Blob.h`) are
synthetic signals (sine tones and silence) generated for this project with
FFmpeg; the generating commands are recorded in each header. They are
original project content, contain no third-party recordings, and are covered
by the project license.

---

## Packaged distributions

The linked-library tables above describe what the source builds against.
This section describes what each packaged release actually redistributes,
since the corresponding-source obligations of the GPL and LGPL attach to
those binaries.

### macOS bundle (`rawform.app`, `.dmg`)

Produced by `scripts/macdeploy.sh`, which copies the Qt frameworks and QML
modules, the engine's third-party dylibs (libsndfile, libFLAC, libmpg123,
libebur128, TagLib, yaml-cpp) and, when enabled, the four FFmpeg libraries
into the bundle.

The FFmpeg build used is **Homebrew's**, configured with both
`--enable-gpl` and `--enable-version3` (it includes x264 and x265). The
corresponding source is available from:

- FFmpeg upstream releases: https://ffmpeg.org/download.html
- The Homebrew formula that produced the build (configure flags and source
  URL): https://github.com/Homebrew/homebrew-core/blob/master/Formula/f/ffmpeg.rb

The exact FFmpeg version bundled in a given release is printed by
`otool -L rawform.app/Contents/MacOS/rawform | grep libav`, and the formula
builds the unmodified upstream release tarball of that version.

**Transitively bundled codec libraries.** The Homebrew FFmpeg build depends
on further codec libraries, and deploying the app copies them into the
bundle alongside the libav* dylibs. Rawform does not link any of these
directly; they ship only as dependencies of FFmpeg itself (verified from the
deployed bundle's `Contents/Frameworks`):

| Component | License | Project |
|-----------|---------|---------|
| x264 | GPL v2 or later | https://www.videolan.org/developers/x264.html |
| x265 | GPL v2 or later | https://www.videolan.org/developers/x265.html |
| SVT-AV1 | BSD 3-Clause Clear | https://gitlab.com/AOMediaCodec/SVT-AV1 |
| libvpx | BSD 3-Clause | https://www.webmproject.org |
| dav1d | BSD 2-Clause | https://code.videolan.org/videolan/dav1d |
| Opus | BSD 3-Clause | https://opus-codec.org |
| libvorbis / libvorbisenc | BSD 3-Clause | https://xiph.org/vorbis/ |
| libogg | BSD 3-Clause | https://xiph.org/ogg/ |
| LAME (libmp3lame) | LGPL v2 or later | https://lame.sourceforge.io |
| OpenSSL 3 | Apache License 2.0 | https://www.openssl.org |

OpenSSL also serves as the backend of Qt's TLS plugin. The corresponding
source for all of the above is available through the same Homebrew formula
chain referenced above; each is built from its unmodified upstream release.

### Linux Flatpak (`com.rawform.app`)

Produced from `packaging/flatpak/com.rawform.app.yml` on the
`org.kde.Platform` runtime. That manifest is the authoritative record of
what the Flatpak bundles, with every source pinned to a release tag or a
checksummed tarball; the summary here reflects the manifest at the time of
writing.

**Supplied by the runtime, not bundled.** Qt 6, the PipeWire client
libraries, ALSA, libogg and libvorbis, and the graphics stack come from the
KDE and freedesktop runtimes and are redistributed by Flathub under their
own terms.

**Built into the Flatpak as modules** (all from unmodified upstream
sources unless noted):

| Component | Pinned version | License | Source |
|-----------|----------------|---------|--------|
| libFLAC | 1.5.0 | BSD 3-Clause | https://github.com/xiph/flac |
| libmpg123 | 1.33.6 | LGPL v2.1 | https://downloads.sourceforge.net/mpg123/ |
| libsndfile | 1.2.2 | LGPL v2.1 or later | https://github.com/libsndfile/libsndfile |
| libebur128 | 1.2.6 | MIT | https://github.com/jiixyj/libebur128 |
| TagLib | 2.1 (with its utfcpp submodule, BSL-1.0) | LGPL v2.1 / MPL 1.1 (dual) | https://github.com/taglib/taglib |
| yaml-cpp | 0.8.0 (modified, see below) | MIT | https://github.com/jbeder/yaml-cpp |
| FFmpeg | n8.0 | GPL v3 (as configured) | https://github.com/FFmpeg/FFmpeg |

Notes:

- **yaml-cpp is modified.** The manifest inserts `#include <cstdint>` at the
  top of every `src/*.cpp` before building, to compile under the SDK's GCC.
  The modification is that one shell step in the manifest, which is
  therefore part of the corresponding source for this component.
- **FFmpeg is built only with its internal codecs.** The Flatpak configures
  FFmpeg with `--enable-gpl --enable-version3 --enable-shared
  --disable-programs --disable-doc` and no external codec libraries, so
  unlike the macOS bundle it carries no x264, x265, SVT-AV1, libvpx, dav1d,
  Opus or LAME. The GPL flags are kept for parity with the macOS posture and
  the project's outbound license.
- **libmpg123 ships without its command-line tools.** The `mpg123`,
  `mpg123-id3dump`, `mpg123-strip` and `out123` binaries are removed at
  cleanup; only the library is redistributed.
- **libsndfile** is built with `ENABLE_MPEG=OFF` (Rawform decodes MPEG
  through libmpg123 directly), so no LAME dependency exists in this build,
  and with external libs enabled so it links the bundled libFLAC and the
  runtime's ogg/vorbis.
- The Flatpak also installs the engine's diagnostic CLI,
  `rawform_audio_cli`, which is project code under the project license.

---

## License texts

- Apache-2.0 - https://www.apache.org/licenses/LICENSE-2.0
- BSD 2-Clause - https://opensource.org/license/bsd-2-clause
- BSD 3-Clause - https://opensource.org/license/bsd-3-clause
- BSD 3-Clause Clear - https://spdx.org/licenses/BSD-3-Clause-Clear.html
- BSL-1.0 - https://www.boost.org/LICENSE_1_0.txt
- LGPL-2.1 - https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html
- LGPL-3.0 - https://www.gnu.org/licenses/lgpl-3.0.html
- MIT - https://opensource.org/license/mit
- MPL-1.1 - https://www.mozilla.org/en-US/MPL/1.1/
- OFL-1.1 - https://openfontlicense.org
- GPL-2.0-or-later - https://www.gnu.org/licenses/old-licenses/gpl-2.0.html
- GPL-3.0 - see [`LICENSE`](LICENSE)
