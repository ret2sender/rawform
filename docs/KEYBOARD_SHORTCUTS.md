# Keyboard shortcuts

All shortcuts rawform responds to, by where they apply. `Ctrl` is `Cmd` on
macOS throughout, with the exceptions noted in their rows.

Two scopes matter:

- **Main window**: fires anywhere in the main window, whichever pane has focus,
  as long as no menu or popup is open. These are the commands that also have a
  menu entry; the menu shows the same key next to the item.
- **Playlist**: fires only while the playlist table has keyboard focus (it does
  by default; a click in the table brings it back). These are the cursor,
  selection and reveal keys, which have no menu entry.

A text field that has focus (the tab rename field, a value editor) keeps its own
keys: Space types a space, Left/Right move the caret, Ctrl+A/C/V edit text.

## Playback

| Key | Action | Scope |
| --- | --- | --- |
| `Space` | Play / pause. Stopped: starts the active playlist from its selection, else its first row | Main window |
| `Ctrl+`` (also `Ctrl+~`) | Stop (rewinds; Play restarts the same track) | Main window |
| `Ctrl+<` (also `Ctrl+,`) | Previous track. Past 5 s into a track it restarts the current track instead, like the button | Main window |
| `Ctrl+>` (also `Ctrl+.`) | Next track: the queue head when the Playback Queue holds one, else the next row | Main window |
| `Q` | Add the selected tracks to the end of the Playback Queue | Main window |
| `Shift+Q` | Play Next: put the selected tracks at the front of the Playback Queue | Main window |
| `Ctrl+Shift+Q` | Show or hide the Playback Queue tab | Main window |
| `Right` | Seek forward 5 s | Playlist, seek bar |
| `Left` | Seek back 5 s | Playlist, seek bar |
| `Home` | Rewind to the start | Seek bar |
| `Enter` | Play the current (focus) row from the start, centering it if it is off screen. In a playlist this also clears the Playback Queue; in the queue tab it plays that entry now and drops the entries above it | Playlist |
| `Up` / `Right` and `Down` / `Left` | Volume up / down by 5 percent | Volume bar |
| `Home` / `End` | Volume to silence / to full | Volume bar |
| `Space` | Mute / unmute | Mute readout |

Seek keys repeat while held and compound: each press adds 5 s to the target,
the engine is asked at most every 150 ms, and a badge above the fill's tip
shows the running total ("+15") until it fades. Seeking works while paused and
leaves playback paused at the new spot. A press that moves nothing (Left at
0:00, Right at the end) does nothing.

The seek bar, the mute readout and the volume bar are Tab stops after the
playlist, in that order; the rows marked with them above apply while one of
them has keyboard focus (a focus ring shows which).

Media keys (Play/Pause, Previous, Next on a keyboard's function row) are not
handled yet. On KDE, GNOME and macOS the desktop grabs them and routes them
through MPRIS, which rawform does not expose yet; that is planned.

## Playlists and files

| Key | Action | Scope |
| --- | --- | --- |
| `Ctrl+N` | New playlist (a new tab) | Main window |
| `Ctrl+O` | Open playlist... | Main window |
| `Ctrl+S` | Save playlist... (save as) | Main window |
| `Ctrl+W` | Close the active playlist tab. Closing the last tab leaves a fresh empty one | Main window |
| `Ctrl+Tab` / `Ctrl+Shift+Tab` | Next / previous playlist tab, wrapping at both ends. On macOS this is `Control+Tab` / `Control+Shift+Tab` (the Control key, not Cmd: Cmd+Tab is the application switcher) | Main window |
| `Ctrl+Q` | Quit (Linux and Windows; on macOS, Cmd+Q from the application menu as usual) | Main window |
| `Ctrl+F` | Find... (search the active playlist) | Main window |
| `Ctrl+P` | Settings... (also `Cmd+P` on macOS, not `Cmd+,`) | Main window |
| `Alt+F` / `Alt+E` / `Alt+V` / `Alt+P` / `Alt+H` | Open (or close) the File / Edit / View / Playback / Help menu | Anywhere in the app |

## Playlist navigation and selection

The cursor keys form one model: each moves the current row and selects it
alone; holding `Shift` grows the range from the anchor (the row a Shift range
extends from, or the current row when there is none) instead.

| Key | Action |
| --- | --- |
| `Up` / `Down` | Move the current row by one |
| `PgUp` / `PgDn` | Move by one page (the fully visible rows less one) |
| `Home` / `End` | Move to the first / last row |
| `Shift` + any of the above | Extend the selection to the target. `Shift+Home` / `Shift+End` do nothing when no row is current |
| `Ctrl+A` | Select all |
| `Escape` | Clear the selection |
| `Delete` / `Backspace` | Remove the selected rows from the playlist (in the queue tab: from the queue) |
| `Ctrl+T` | Reveal the playing track: switch to its tab if needed and center it |
| `Ctrl+Shift+T` | Center the selection (the midpoint of its span; with nothing selected, the current row) |
| `Alt+Enter` | Open the Properties window for the selection |

All of these apply while the playlist has focus. A key that scrolls the view
shows the scrollbar the way a wheel scroll does.

## Properties window

Metadata tab, while the field list has focus (no editor open):

| Key | Action |
| --- | --- |
| `Up` / `Down` | Move the current field |
| `Enter` | Edit the current field (single row only) |
| `F2` | Edit the current field in place |
| `Delete` / `Backspace` | Remove the selected fields |
| `Ctrl+N` | Add a field |
| `Ctrl+A` | Select all fields |
| `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | Copy / cut / paste fields |
| `Escape` | Clear the field selection; with nothing selected, close the window |

ReplayGain tab: `Ctrl+A` selects all rows, `Escape` clears the selection or
closes the window. Inside a value editor, `Enter` commits and `Escape`
discards.

## Find dialog

| Key | Action |
| --- | --- |
| `Enter` / `Shift+Enter` | Step the current row to the next / previous match (from the String field) |
| `Escape` | Close the dialog; every row shows again |

## Other windows

| Key | Action |
| --- | --- |
| `Escape` | Settings: discard unapplied changes and close. About, Rename Files: close. Custom Columns: clear the selection, then close (edits there are live). Log console: hide it. Edit Value dialog: cancel |
| `Ctrl+A` / `Ctrl+C` / `Ctrl+V` | Edit Value dialog: select all / copy / paste in the value field |
| `Enter` | Settings, Properties (outside the Metadata field list) and Rename Files (also from the pattern field): run OK. Edit Value dialog and the ReplayGain settings fields: commit |
| `Tab` / `Shift+Tab` | Move between the dialog's controls: the footer buttons, the segmented selectors, the Properties tab strip, the Settings navigation. On macOS, Tab reaches buttons only with System Settings > Keyboard > Keyboard navigation turned on (the platform rule) |
| `Space` | Activate the focused button |
| `Left` / `Right`, `Home` / `End` | On a focused segmented selector: pick the previous / next, first / last choice. On the Properties tab strip: previous / next tab |
| `Up` / `Down` | On the Settings navigation: previous / next page |

## Mouse gestures

Not shortcuts, but the pointer equivalents of the keys above.

Playlist rows: click selects; `Ctrl`+click toggles a row; `Shift`+click selects
a range from the anchor; `Ctrl+Shift`+click adds a range; double-click plays the
row (clearing the Playback Queue; in the queue tab it jumps the line instead);
drag moves the selected rows; right-click opens the row menu (Add to Playback
Queue, Play Next, Reload info, Rename Files, Properties, ...). Header: drag
reorders columns, drag the edge resizes, right-click opens the column menu.

Tabs: click switches; double-click the title renames (not the Playback Queue
tab, whose title is fixed); drag reorders; middle-click closes; middle-click on
the empty strip creates a playlist; close glyph closes. Closing the Playback
Queue tab hides it with its contents intact. Dropping files on the strip makes
a new tab for them.

Player bar: click or drag the progress bar to seek (one seek, on release);
click the volume percentage to mute / unmute; wheel or two-finger scroll over
the volume slider adjusts it a step at a time.
