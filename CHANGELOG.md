# Changelog

All notable changes to rawform are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.2.0] - 2026-10-02

A search release: Edit > Find filters the active playlist in place as you
type, with saved Filter presets, and the reveal shortcuts move to make room
for it. Four fixes found on the way ride along, among them a crash on exit.

### Added

- Edit > Find (Ctrl+F): a live search over the active playlist. The String
  box is what to find (whitespace-separated terms, all required, matched as
  case- and accent-insensitive substrings); the Filter box is where to look
  (':'-separated column titles, custom column names, field ids or %token%
  patterns; empty searches the visible columns), with invalid entries
  underlined; Filter expressions save as named presets. The playlist hides
  every row that does not match while the dialog is open (closing it brings
  the rows back), and Enter / Shift+Enter step the current row through the
  hits. Row reorder by drag is unavailable while a search is in force; a
  drop appends.

### Changed

- The playlist reveal shortcuts moved to make room for Find: Ctrl+T reveals
  the playing track (was Ctrl+F) and Ctrl+Shift+T centers the selection
  (was Ctrl+Shift+F).
- The editable preset combo box (Rename Files) opens its dropdown from the
  arrow again; the text editor had been laid over the indicator and swallowed
  the click.
- The metadata pane aggregates once per selection gesture instead of twice.
  A click (and the select-on-finish after a scan) is a selection change and a
  current-row change, and each ran a full pass over the selected tracks; the
  two are now coalesced into one. About 50 ms saved per click on a 15k-row
  playlist.
- A closed Find dialog no longer rebuilds its search index as tracks are
  added to the playlist; the work is deferred until the dialog is shown.
- The tool dialogs' footer buttons and the themed text-edit context menu are
  shared components (ToolDialogButton, ThemedEditMenu) instead of per-window
  copies.

### Fixed

- Quitting after any menu had been opened could crash on the way out, on both
  platforms. The frosted popup backdrop kept one window reference too many on
  the main window's content (a one-sided count in Qt's ShaderEffectSource when
  the source is assigned before the effect has a window), so the content was
  torn down after the window's event delivery had already gone. The backdrop
  now attaches its source only once it is in a window itself.
- Properties: the OK label went invisible while an Apply was in flight (the
  disabled button kept the on-accent text color over the gray face).
- A playlist saved with Save As while Find was filtering recorded its focus
  row and scroll position as visible-row numbers; reopening it landed on the
  wrong tracks. Both are recorded as tracks now, as the live playlist files
  already did for the focus row.
- Switching away from a tab while Find was filtering it parked the scroll
  position as a visible-row number, so switching back under a different
  filter (or none) landed elsewhere; the parked position is now the track,
  and restoring under a filter lands on the first visible track at or after
  it.

[1.2.0]: https://github.com/ret2sender/rawform/releases/tag/v1.2.0

## [1.1.0] - 2026-09-26

A keyboard and polish release: transport and playlist commands get menu
entries and shortcuts, seeking works from the keyboard, the tool windows
behave more consistently, and two long-session memory problems on Linux are
fixed.

### Added

- Playback menu with Play / Pause (Space), Stop (Ctrl+`), Previous (Ctrl+<)
  and Next (Ctrl+>), wired to the same controller calls as the player bar
  buttons so the menu, its shortcuts, and the buttons cannot diverge.
- Shortcuts for the playlist file commands: New Playlist (Ctrl+N), Open
  playlist (Ctrl+O), Save playlist (Ctrl+S), and a new Close playlist entry
  (Ctrl+W); Settings opens with Ctrl+P. Every shortcut is declared on its
  menu item and drawn as a right-aligned hint in the platform's native
  spelling (Cmd on macOS).
- Keyboard seeking: Left / Right in the playlist step playback by 5 seconds,
  while playing or paused. A held key accumulates into one throttled seek
  per window instead of flushing the engine on every auto-repeat, and the
  step measures from the seek in flight rather than from the stale reported
  position. A no-op while stopped or on an unseekable source.
- A step badge above the seek bar's fill tip shows what each keyboard seek
  actually did ("+5", "-5", or "+3" against the end of the track), compounds
  across a run of presses ("+15"), and fades away after the last one.
- Page Up / Page Down and Home / End in the playlist, joining Up / Down as
  cursor keys; Shift on any of them grows the selection from the anchor row.
- Double-click on the title bar maximizes or restores the window. On macOS
  this follows the system's "Double-click a window's title bar to" setting
  (zoom, minimize, or none) and picks up a change in System Settings without
  a restart.
- The Settings and Custom Playlist Columns windows are resizable, with edge
  grips like the other tool windows; their sizes persist in `window.yaml`.

### Changed

- Playlist reveal shortcuts: Ctrl+F now reveals the playing track, switching
  to its tab when needed (was Ctrl+P), and Ctrl+Shift+F centers the selected
  tracks (was Ctrl+F). Ctrl+P is now Settings.
- Properties window: every transient message (Tools menu and context menu
  hover hints, the busy label during a write or reload pass, status and
  failure messages) lands in one log bar between the tab strip and the panes
  instead of being spread across the panes. The bar sits above the panes so
  no popup ever covers it.
- Settings window: larger default and minimum size (720 x 480), and the
  panes rebuilt on shared row, switch, and combo box components. Combo box
  dropdowns now carry the same frosted backdrop as the menus.
- The close "x" of the frameless tool dialogs (Settings, About, Properties,
  Custom Columns, Rename Files, Edit Value) is one shared component with a
  single look and hover treatment.
- Documentation and comment conformance pass over the audio engine and the
  UI sources; no behavior change.
- README: the TagLib section notes that Fedora 44 and later ship a
  qualifying 2.x `taglib-devel`, keeping the pinned source build for older
  releases; LICENSE is linked from README and THIRD-PARTY-NOTICES; project
  logo added.

### Fixed

- Playlist vertical scrollbar kept its old extent after removing many tracks
  (thousands of rows deleted, scrollbar still sized for the original count
  until a page-sized scroll). The view now re-anchors its viewport once,
  synchronously, at the end of a bulk removal.
- Album art memory growth on Linux while browsing across albums. Covers
  decode straight to the display bound instead of at full size (a 3000x3000
  embedded JPEG is 36 MB as ARGB32), the art frame holds exactly one decoded
  cover (pixmap cache off), glibc's mmap threshold is pinned so image-sized
  transients return to the OS, and the heap is trimmed after each scan so a
  large library load no longer leaves its peak resident. macOS was not
  affected.
- Build: qmlcachegen's generated sources no longer inherit `-Werror`. Their
  content varies with the Qt version that produced them, and a CI image on a
  different Qt than the workstation failed on code nobody in the tree wrote.
- Malformed copyright header in `AppInfo.cpp` / `AppInfo.h`.

[1.1.0]: https://github.com/ret2sender/rawform/releases/tag/v1.1.0

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
