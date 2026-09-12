# Changelog

All notable changes to rawform are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-09-12

Initial public release. rawform is a foobar2000-inspired desktop audio player
for Linux and macOS: a Qt 6 / QML interface on top of a Qt-free C++20 audio
engine, with native reference decoders first, FFmpeg as the long-tail
fallback, and bit-perfect device-rate switching on by default. This entry
summarizes what was built on the way to 1.0.0; the repository history begins
at this release.

### Audio engine

- Standalone, Qt-free engine (`rawform_audio`) built around a lock-free SPSC
  ring buffer between the decode and render threads.
- Content-sniffed decoder cascade: libFLAC, libmpg123, and the libsndfile
  family as native reference decoders, with FFmpeg catching the long tail
  (AAC, ALAC, WMA, AC3, DTS, ...). Native decoders are always tried first;
  the file's leading bytes pick the cascade and the extension only biases
  the order.
- Gapless playback, sample-accurate seeking, and a `playNow` primitive that
  makes playback follow the playlist.
- Bit-perfect rate management: the output device follows each track's native
  sample rate when the hardware supports it, with a nearest-best fallback,
  and the published status always reports what the device is actually doing.
- Live bitrate metering, a log-banded FFT spectrum analyzer tappable at the
  output or the source, and ReplayGain scanning (BS.1770 loudness via
  libebur128).

### Output backends

- CoreAudio / AudioUnit on macOS, with a single-transition device
  reconfigure and a crash-recovery rate ledger that restores the device rate
  on the next launch if a session died holding a rate change.
- PipeWire on Linux: the stream is a real graph node, with output device
  enumeration and selection (persisted), in-place reconfiguration on device
  changes, and last-mile verification of the driver's realized format so the
  bit-perfect indicator never overstates.

### Playlists

- Tabbed multi-playlist interface. Every tab is continuously persisted and
  the whole session (tabs, order, active tab, per-tab scroll position)
  restores on relaunch.
- `.rwfpl`, a chunked binary playlist format, with corruption-hardened
  bounded readers; playlists can also be exported to and imported from
  M3U / M3U8.
- Drag and drop of files, folders (walked recursively), `.m3u` / `.m3u8`
  playlists, and `.rwfpl` files; dropping on the tab bar opens new tabs.
- Column system with per-playlist layout persistence, a saveable default
  layout, and custom columns defined by a `%token%` pattern syntax.
- Direct manipulation throughout: drag-to-reorder, rubber-band selection,
  range and additive selection, keyboard navigation with reveal shortcuts
  for the playing and selected tracks.

### Metadata and Properties

- Multi-instance, non-modal Properties windows (metadata, ReplayGain, and
  Location panes) with tag editing written back through TagLib and
  cross-window write serialization so concurrent edits cannot interleave.
- Metadata transforms (capitalize, clean up, crop, automatic track
  numbering), clipboard support, and a Tools menu with Reload info, Rewrite
  tags, Remove tags, and an MP3-only Optimize size.
- ReplayGain scanning stages track and album gains for review before
  anything is written, with multi-album selections bucketed automatically;
  playback applies the gains with clip prevention.
- Rename To: file renaming driven by the same `%token%` pattern system, with
  saveable pattern presets.

### Interface

- Player bar with interactive seek scrubbing, custom SVG transport icons,
  master volume, and an honest status line: codec, bitrate, sample rate, and
  whether output is currently bit perfect or resampled.
- Platform-adaptive window chrome (macOS traffic lights; a Breeze/KWin
  profile on Linux), window geometry persistence including saved sizes for
  the tool windows, and adaptive context menus.
- Keyboard-friendly by design: two-stage Escape, Alt+Enter for Properties,
  and Ctrl+F / Ctrl+P / Ctrl+Enter playlist reveal shortcuts, among others.

### Platform and packaging

- Flatpak manifest on the KDE 6.11 runtime as the Linux release channel,
  with direct PipeWire socket access preserving bit-perfect playback inside
  the sandbox; a desktop-integration script covers plain dev builds.
- macOS app bundle and versioned `.dmg` via `macdeployqt`-based tooling.
- AppStream metainfo and `.desktop` entry; XDG-correct config storage with
  atomic writes.

### Quality

- Twelve unit-test binaries across the engine and the UI (ring buffer,
  decoders, engine conformance, rate management, ReplayGain, spectrum,
  pattern evaluation, rename sanitization, playlist file round-trip and
  corruption), on a dependency-free test harness.
- TSan and ASan+UBSan clean on Linux, with the suppression rationale
  documented in the tree.
- GPL-3.0-or-later, with SPDX headers on every file and full third-party
  notices for everything bundled or linked.

[1.0.0]: https://github.com/ret2sender/rawform/releases/tag/v1.0.0
