# Keyboard shortcuts

All shortcuts rawform responds to, by where they apply. `Ctrl` is `Cmd` on
macOS throughout, with the two exceptions noted in their rows.

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
| `Ctrl+>` (also `Ctrl+.`) | Next track | Main window |
| `Right` | Seek forward 5 s | Playlist |
| `Left` | Seek back 5 s | Playlist |
| `Enter` | Play the current (focus) row from the start, centering it if it is off screen | Playlist |

Seek keys repeat while held and compound: each press adds 5 s to the target,
the engine is asked at most every 150 ms, and a badge above the fill's tip
shows the running total ("+15") until it fades. Seeking works while paused and
leaves playback paused at the new spot. A press that moves nothing (Left at
0:00, Right at the end) does nothing.

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
| `Delete` / `Backspace` | Remove the selected rows from the playlist |
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

## Other windows

| Key | Action |
| --- | --- |
| `Escape` | Settings: discard unapplied changes and close. About, Rename Files: close. Custom Columns: clear the selection, then close (edits there are live). Log console: hide it. Edit Value dialog: cancel |
| `Ctrl+A` / `Ctrl+C` / `Ctrl+V` | Edit Value dialog: select all / copy / paste in the value field |
| `Enter` | Edit Value dialog and the ReplayGain settings fields: commit |

## Mouse gestures

Not shortcuts, but the pointer equivalents of the keys above.

Playlist rows: click selects; `Ctrl`+click toggles a row; `Shift`+click selects
a range from the anchor; `Ctrl+Shift`+click adds a range; double-click plays the
row; drag moves the selected rows; right-click opens the row menu (Reload info,
Rename files, Properties, ...). Header: drag reorders columns, drag the edge
resizes, right-click opens the column menu.

Tabs: click switches; double-click the title renames; drag reorders;
middle-click closes; middle-click on the empty strip creates a playlist;
close glyph closes. Dropping files on the strip makes a new tab for them.

Player bar: click or drag the progress bar to seek (one seek, on release);
click the volume percentage to mute / unmute; wheel over the volume slider
adjusts it.
