/*
 * This file is part of rawform.
 * Copyright (C) 2026 Etienne Fleurant
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// qmllint disable unqualified
// This file is the app's wiring layer: it deliberately reaches the C++
// context properties (audioController, playlistTabs), which qmllint cannot
// see, so the unqualified-access category is disabled file-wide. Under the
// Bound pragma this directive covers context properties ONLY; the tab
// delegate declares its injected names and every other capture is
// statically checked under the pragma.
// Components stay fully linted; keep global wiring HERE so they can.
// Cost: a typo'd global name in this file surfaces at runtime, not lint.

// Bound component behavior: nested components and delegates resolve outer
// document ids statically instead of through dynamic context lookup. The
// tab-strip delegate already declares `index` and `modelData` (typed
// string: the model is a plain string array) and self-qualifies through
// its `tab` id; the captures of `propertiesWindow` there and inside
// FooterButton are exactly what the pragma makes statically valid.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic  // MenuSeparator in the Tools menu
import QtQuick.Effects
import QtQuick.Layouts
import QtQuick.Window


// =============================================================================
// PropertiesWindow.qml
//
// The track Properties window. Frameless, NON-MODAL,
// our chrome, MULTIPLE INSTANCES: PlaylistView instantiates a fresh window per
// "Properties" invocation (foobar2000 style) and each instance destroys itself
// on close. There is no hide/reuse and therefore no stale-backing-store reopen
// flash to fight: a new instance has never painted anything. All close paths
// (Cancel, the titlebar X, OK on success) funnel through Window.close(); the
// onClosing handler destroys the instance, taking its per-window editors,
// models, and scan controller with it.
//
// The window snapshots the selection on open and stages edits in the panes;
// Apply/OK commit through MetadataEditor and ReplayGainEditor (off-thread
// TagLib writes), then the
// OWNING TAB's MetadataReloader (captured at creation, deliberately not a live
// binding: the active tab can change under a non-modal window) force-reloads
// the written files so the model, metadata pane, and (via the controller's
// dataRevision subscription) the playing track's RG factor all refresh, and the
// pane re-pulls fresh values.
//
// Track identity is PATH-BASED throughout (plus subsong for the re-pull keys):
// every write and scan targets the file captured at open, and the re-pull remaps
// the open-time keys to CURRENT rows via rowsForKeys. Playlist row numbers are
// positions, not identities; a reorder, sort, or removal between opening this
// window and hitting Apply must never redirect a write to a different file.
//
// Lifetime of the captured world: _model and reloader belong to the owning tab,
// which can close while this window is open (the window itself is parented to
// the long-lived PlaylistView, so it survives). A QML var wrapping a destroyed
// QObject is NOT dependably null/falsy, so every post-open touch guards through
// _alive (a C++ probe via playlistTabs.objectAlive); a dead model or reloader
// skips its phase, and a reload pass orphaned by a dying reloader is completed
// by the reloader's destructor token flush. The file writes are path-based and
// land regardless; when nothing is left to re-pull from, the panes adopt the
// staged values as their new baseline instead (see _finishApply).
//
// Cross-window semantics (multiple windows over the same tracks): each window
// snapshots at open and there is no cross-window sync; last write wins,
// matching foobar2000. collectEdits diffs against the open-time baseline, so a
// stale window only rewrites fields it actually staged; two windows clobber
// each other only when both staged the same field.
//
//   OK     : apply, then close (stays open if any file failed to write)
//   Apply  : apply, stay open
//   Cancel : discard staged edits, close
//
// Sequence: _apply -> [meta write] -> [rg write] -> reloader.reloadPaths of the
// TOUCHED paths only (the union of what the two editors actually targeted) ->
// pathsReloaded(our token) -> _finishApply (re-pull + maybe close). The token
// matters: refreshed() only fires when something changed and can belong to an
// unrelated conditional pass, so waiting on it could hang (written tracks all
// left the playlist: empty pass, no signal) or finish early (a revalidation
// pass completing first). _applying gates the footer AND the titlebar X so a
// pass cannot be double-fired and the instance cannot be destroyed mid-write
// from the UI (the editors' destructors wait out an in-flight pass as the
// backstop for OS-initiated closes).
//
// Scanning: the pane's ReplayGain right-click menu also GENERATES values. Its
// scanRequested signal drives ReplayGainScanController off-thread (a progress
// overlay covers the window meanwhile, with Cancel); on completion the measured
// values are staged back into the pane via stageScanResults, exactly like a typed
// edit, so the user reviews them and commits through the same Apply / OK path. The
// scan never writes on its own. A canceled scan stages nothing.
//
// Tools menu (footer left, foobar's bottom-left Tools button): window-scoped
// ReplayGain and file actions. The scan / clear entries reuse the pane's
// staging machinery but act over EVERY row (requestScanAll / clearAllReplayGain),
// ignoring the pane's row sub-selection; the pane's own right-click menu stays
// selection-scoped. "Reload info" force-re-reads the selection's files from
// disk through the owning tab's reloader and re-pulls all three tabs,
// DISCARDING staged edits by design (it is the take-disk-truth action). It
// runs under the same _applying gate as Apply but with its own token
// (_infoReloadToken), so the apply and reload sequencers can never complete
// each other through the shared pathsReloaded handler. Menu hover hints
// surface in the footer's left text slot while the menu is open.
//
// Menu layout: metadata actions first (Add new field, Auto track number,
// Remove tags), then a ReplayGain submenu holding all five RG entries, then
// Reload info. Remove tags is STAGED like everything else in this window:
// it blanks every metadata field row (removeAllFields), stages RG clears, and
// raises _stripAllStaged; Apply / OK then hands the selection's file paths to
// MetadataEditor as stripAllPaths, whose write pass structurally strips MP3
// tag blocks (deliberately overriding the ID3v1 / APE Preserve settings) and,
// on other formats, replaces the whole property map and clears unsupported
// and complex properties (embedded pictures included). The flag clears when
// the pass that carried it finishes, success or not (re-invoke Remove tags to
// retry a failure), and on Reload info, the discard-staging action.
// =============================================================================
Window {
    id: propertiesWindow

    // UI family for this window's own labels; children resolve Theme on
    // their own, nothing is forwarded. Kept as a property purely as an
    // override surface.
    property string uiFont: Theme.uiFont

    // The owning tab's MetadataReloader, passed by the host as an initial value
    // at createObject time. A VALUE capture on purpose, never a live binding:
    // the host's root.reloader tracks the ACTIVE tab, and a tab switch under
    // this non-modal window must not silently retarget the post-apply reload.
    // Nulls out in QML if the tab (and with it the reloader) is destroyed.
    property var reloader: null

    property Item menuBlurSource: windowBody
    property int currentTab: 0   // Metadata is the default tab

    // Apply state.
    property var _model: null
    property var _rows: []
    // Durable { path, subsong } identity keys for the open-time selection (see
    // PlaylistModel::trackKeys). _rows is only a convenience mirror of their
    // CURRENT positions; _keys is what survives a playlist mutation.
    property var _keys: []
    // Unique file paths the in-flight apply actually writes (union of the two
    // editors' edit targets), computed in _apply. The reload phase re-reads
    // exactly these, not the whole selection: with path identity we know
    // precisely which files changed, so a 500-track selection with one edited
    // file re-parses one file.
    property var _touchedPaths: []
    // Token of our pending reloadPaths pass; _finishApply runs only when the
    // reloader reports THIS token done (see the banner on why not refreshed()).
    property int _reloadToken: -1
    property bool _applying: false
    property bool _closeAfter: false
    property string _statusMsg: ""
    // What the footer's busy slot reads while _applying is up: "Applying..."
    // for the Apply / OK sequencer, "Reloading..." for the Tools reload.
    // _applying itself stays the single any-file-operation gate for both.
    property string _busyText: ""
    // Token of a pending Tools "Reload info" pass. Kept separate from
    // _reloadToken so the shared onPathsReloaded handler routes each pass to
    // its own finisher and the two sequencers can never complete each other.
    property int _infoReloadToken: -1
    // Hover hint of the highlighted Tools menu item, shown in the footer's
    // left text slot while non-empty; the menu clears it on close.
    property string toolsHint: ""

    // Edit Value dialog (multi-track editing) overlay visibility.
    property bool _editDialogActive: false

    title: "Properties"
    // Size restores from window.yaml's keyed store, else the built-in
    // default; clamped against the minimums here because the store does not
    // know them. Initial values, not live bindings: invokable calls are not
    // tracked as dependencies, which is exactly right for a startup-only
    // read. Position is NOT restored: the host cascades each instance.
    width: Math.max(minimumWidth, windowGeometry.savedWidth("properties", 720))
    height: Math.max(minimumHeight, windowGeometry.savedHeight("properties", 560))
    minimumWidth: 560
    minimumHeight: 360
    color: "transparent"
    // Qt.Tool, not Qt.Window: a tool window floats above its transient parent
    // (the main window, resolved automatically from the QML visual parent), so
    // clicking the playlist never buries an open Properties window; foobar2000
    // on Windows behaves this way via owned windows, and macOS does not give
    // plain sibling windows that relationship. The standard utility-window
    // trade-off applies: on macOS the tool windows hide while the app is
    // deactivated and reappear with it (arguably the desired citizenship), and
    // they do not appear in the Dock / cmd-tab (moot: frameless anyway).
    flags: Qt.Tool | Qt.FramelessWindowHint
    modality: Qt.NonModal

    // Fresh instance per open: closing IS destruction, so there is no deferred
    // hide, no transparent close paint, and no reopen-flash dance to manage: a
    // window that is never re-shown has no stale backing store to flash.
    // destroy() defers actual deletion to the
    // event loop, so finishing this handler (and anything else on the current
    // stack) is safe. The per-window editors' C++ destructors wait out an
    // in-flight write pass, so even an OS-initiated close mid-apply cannot
    // strand a pool worker (it just blocks the GUI for the tail of the write).
    onClosing: {
        // Persist the size for the next instance, windowed frames only
        // (the visibility guard, same as the main window). Several instances closing
        // just means last write wins. Then destruction, as before.
        if (propertiesWindow.visibility === Window.Windowed)
            windowGeometry.saveSize("properties",
                                    propertiesWindow.width,
                                    propertiesWindow.height)
        propertiesWindow.destroy()
    }

    function _closeWindow() {
        propertiesWindow.close() // -> onClosing -> destroy
    }

    // Reliable liveness probe for the captured tab references (_model, reloader),
    // whose owner can die while this non-modal window is open. A QML var holding
    // a destroyed QObject is NOT dependably null/falsy in every JS context: the
    // dangling wrapper can stay truthy and then throw on the method call, which
    // would abort the apply sequencer mid-flight and strand the window in
    // Applying. Marshaling the wrapper to a C++ QObject* parameter IS reliable
    // (the engine passes nullptr for a dead object), so route the check through
    // playlistTabs.objectAlive.
    function _alive(obj) {
        if (obj === null || obj === undefined)
            return false
        if (typeof playlistTabs !== "undefined" && playlistTabs)
            return playlistTabs.objectAlive(obj)
        return true // no probe available; trust the reference (test harness case)
    }

    // Open the Edit Value dialog for a field row: any multi-track edit, the
    // pane's explicit single-track "Edit" action, or the multiline routing
    // from an edit-in-place entry point. Feeds the dialog from the model,
    // then reveals the overlay; for a single track the field is trivially
    // uniform, so the dialog opens on Single Value.
    function _openEditDialog(row) {
        editDialog.addMode = false
        editDialog.keyError = ""
        editDialog.fieldLabel = metaModel.fieldLabel(row)
        editDialog.fieldKey = metaModel.fieldKey(row)
        editDialog.validatorPattern = metaModel.editValidatorPattern(row)
        editDialog.singleSeed = metaModel.sharedEditValue(row)
        editDialog.startUniform = metaModel.fieldUniform(row)
        editDialog.perTrackValues = metaModel.perTrackEditValues(row)
        editDialog.trackLabels = metaModel.trackLabels()
        editDialog.reopen()
        propertiesWindow._editDialogActive = true
    }

    // Open the dialog in add mode for the "+ add new" row: an editable, empty name
    // box and an empty Single Value tab over the whole selection.
    function _openAddDialog() {
        editDialog.addMode = true
        editDialog.keyError = ""
        editDialog.fieldLabel = ""
        editDialog.fieldKey = ""
        editDialog.validatorPattern = "^.*$"   // custom keys are free-text list fields
        editDialog.singleSeed = ""
        editDialog.startUniform = true
        editDialog.perTrackValues = metaModel.blankPerTrack()
        editDialog.trackLabels = metaModel.trackLabels()
        editDialog.reopen()
        propertiesWindow._editDialogActive = true
    }

    // Feed the fresh instance its selection and show it. Called by the host
    // exactly once, right after createObject; the defaults of a new instance
    // cover everything a reused window would have to reset by hand (tab,
    // staging, status, apply bookkeeping, half-open editors).
    function openFor(model, rows) {
        if (!model || !rows || rows.length === 0) {
            propertiesWindow.destroy() // never shown; do not leak the instance
            return
        }
        propertiesWindow._model = model
        propertiesWindow._rows = rows.slice()
        // The durable identity snapshot: everything after this moment addresses
        // these tracks by (path, subsong), never by playlist position.
        propertiesWindow._keys = model.trackKeys(rows)
        // The RG model ingests through replayGainRows C++-to-C++: one
        // reset, no per-row JS, snapshot semantics identical to before.
        rgModel.setSelection(model, rows)
        // Feed the read-only tabs their selection snapshots. Both models copy the
        // TrackData out, so they stay valid even if the playlist selection changes
        // while the window is open (the RG snapshot above works the same way).
        metaModel.setSelection(model, rows)
        detailsModel.setSelection(model, rows)
        show()
        raise()
        requestActivate()
        propertiesWindow._focusCurrentTab() // Seed keyboard focus
    }

    // Escape clears the visible pane's row selection, through the Shortcut
    // phase because Keys-based Escape delivery was observed dead on macOS (the
    // panes' Keys branches remain as fallback; shortcuts match first where they
    // do fire, so the action never runs twice). The enabled gate hands Escape
    // back to everything that owns it more locally: the Edit Value / Add Field
    // overlay (its root cancels on Escape), the metadata inline editor
    // (editingRow), a ReplayGain cell editor (keysFocused is false while one
    // holds focus), and the apply pass. Window context (the default), so each
    // Properties instance carries its own map. Context alone does NOT prevent
    // ambiguity, though: a still-enabled
    // shortcut in a hidden or unfocused window keeps participating in Qt's
    // shortcut map, and two enabled matches on one press fire NEITHER
    // onActivated (Qt rotates activatedAmbiguously instead). Hence the
    // strict-focus terms below. NOT Window.active: that is
    // QWindow::isActive(), a transient-GROUP activation, so every visible
    // secondary window reports active together and that gate left
    // two-open-window presses ambiguous (app-wide dead Escape).
    // WindowFocus.focusWindow is QGuiApplication::focusWindow(), singular by
    // definition: at most one secondary-window Escape shortcut is grabbed at
    // any instant, so several open Properties instances can never go
    // ambiguous with each other, with the reused Settings / About / Custom
    // Columns instances, or with the main window. A window that can receive
    // the Escape key IS the focus window, so the gate never starves a
    // legitimate press.
    //
    // The action is two-stage. A visible selection is cleared first; with
    // nothing to clear, Escape CLOSES the
    // window through _cancel(), byte-identical to the Cancel button (staged
    // edits die with the instance). The probes are the exact complements of
    // the clear actions: hasRowSelection() is true iff clearRowSelection()
    // would change something, hasSelection likewise for clearSelection(), so
    // the two stages can never both fire on one press and the second press
    // always closes. The gate includes the Details tab (currentTab 1), which
    // has no selection concept: it closes on the first press.
    Shortcut {
        sequences: [StandardKey.Cancel]
        enabled: propertiesWindow.visible
                 && WindowFocus.focusWindow === propertiesWindow
                 && !propertiesWindow._applying
                 && !propertiesWindow._editDialogActive
                 && ((propertiesWindow.currentTab === 0
                      && metaPane.editingRow < 0)
                     || propertiesWindow.currentTab === 1
                     || (propertiesWindow.currentTab === 2
                         && rgPane.keysFocused))
        onActivated: {
            if (propertiesWindow.currentTab === 0) {
                if (metaPane.hasRowSelection())
                    metaPane.clearRowSelection()
                else
                    propertiesWindow._cancel()
            } else if (propertiesWindow.currentTab === 2) {
                if (rgModel.hasSelection)
                    rgPane.clearSelection()
                else
                    propertiesWindow._cancel()
            } else {
                propertiesWindow._cancel()
            }
        }
    }

    // Switch tabs, first closing any editor open on the tab being left so edit mode
    // never persists across a tab change. Both discard the in-progress edit (like
    // Cancel): Metadata clears its inline editor, ReplayGain hides its cell editor.
    // The keyboard focus moves WITH the tab: without the handoff the leaving
    // tab's focus item kept receiving shortcuts, so Ctrl+A pressed on the visible
    // tab select-all'd the hidden one.
    function _switchTab(idx) {
        if (idx === propertiesWindow.currentTab)
            return
        if (propertiesWindow.currentTab === 0)
            metaPane.closeEdit()
        else if (propertiesWindow.currentTab === 2)
            rgPane.closeAnyEditor()
        propertiesWindow.currentTab = idx
        propertiesWindow._focusCurrentTab()
    }

    // Park keyboard focus on the current tab's key surface; the Details tab
    // has no shortcuts of its own, but taking focus there is exactly the point,
    // it keeps the hidden panes' handlers out of the delivery path.
    function _focusCurrentTab() {
        if (propertiesWindow.currentTab === 0)
            metaPane.focusKeys()
        else if (propertiesWindow.currentTab === 2)
            rgPane.focusKeys()
        else
            detailsView.forceActiveFocus()
    }

    // Apply / OK commit BOTH tabs in one pass: metadata edits first, then
    // ReplayGain, then a single reload + re-pull. The two editors are async, so
    // this runs as a small phase sequencer chained off their applied() signals:
    //   _apply -> [meta phase] -> _applyRgPhase -> [rg phase] -> _reloadPhase ->
    //   reloader.pathsReloaded(_reloadToken) -> _finishApply.
    // _anyWrote gates the reload; per-editor failures accumulate for the footer.
    property var _pendingRgEdits: []
    property var _metaFailed: []
    property var _rgFailed: []
    property bool _anyWrote: false
    // Remove-tags staging. _stripAllStaged is the user's staged intent
    // (raised by Tools > Remove tags, an Apply-enabler in its own right, since
    // blanking already-blank fields stages nothing in the model); it clears
    // when the pass that carried it finishes, and on Reload info.
    // _pendingStripPaths is the payload of the in-flight pass, captured in
    // _apply so _finishApply knows whether this pass carried the strip.
    property bool _stripAllStaged: false
    property var _pendingStripPaths: []

    // Unique file paths of the open-time selection, from the durable identity
    // keys (subsong siblings share one physical file, so dedupe). Feeds the
    // Remove-tags strip payload and the Tools reload.
    function _selectionPaths() {
        var seen = ({})
        var paths = []
        for (var i = 0; i < propertiesWindow._keys.length; i++) {
            var p = propertiesWindow._keys[i].path
            if (p && seen[p] !== true) {
                seen[p] = true
                paths.push(p)
            }
        }
        return paths
    }

    // Tools > Remove tags: stage the whole removal for review. Blanks
    // every metadata field row (session-only customs drop), stages RG clears
    // across all rows, and raises the strip flag that _apply converts into
    // MetadataEditor's stripAllPaths. Nothing touches disk until Apply / OK;
    // Cancel closes the window with it, Reload info discards it.
    function _removeAllTags() {
        metaPane.cancelEdit()     // discard any open inline editor; the row set
                                  // may shift as session-only customs drop
        rgPane.closeAnyEditor()
        metaModel.removeAllFields()
        rgModel.clearAllReplayGain()
        propertiesWindow._stripAllStaged = true
        propertiesWindow._statusMsg = "Remove tags staged; Apply / OK writes it."
    }

    function _apply(closeAfter) {
        if (propertiesWindow._applying)
            return
        metaPane.commitOpenEditor() // stage an edit the user left open in the cell
        var metaEdits = metaModel.collectEdits()
        var rgEdits = rgModel.collectEdits()
        // The staged Remove tags resolves to the selection's unique paths
        // HERE, at commit time, so the strip and the reload see the same set.
        var stripPaths = propertiesWindow._stripAllStaged
                ? propertiesWindow._selectionPaths() : []
        if (metaEdits.length === 0 && rgEdits.length === 0
                && stripPaths.length === 0) { // nothing staged anywhere
            if (closeAfter)
                propertiesWindow._closeWindow()
            return
        }
        // The union of file paths this pass will actually write: both editors'
        // edit maps carry the target path, and every strip path is written by
        // definition, so the reload phase can re-read exactly what changed
        // instead of force-parsing the whole selection.
        var touched = ({})
        var i
        for (i = 0; i < metaEdits.length; i++)
            touched[metaEdits[i].path] = true
        for (i = 0; i < rgEdits.length; i++)
            touched[rgEdits[i].path] = true
        for (i = 0; i < stripPaths.length; i++)
            touched[stripPaths[i]] = true
        propertiesWindow._touchedPaths = Object.keys(touched)

        propertiesWindow._closeAfter = closeAfter
        propertiesWindow._statusMsg = ""
        propertiesWindow._busyText = "Applying\u2026"
        propertiesWindow._applying = true
        propertiesWindow._metaFailed = []
        propertiesWindow._rgFailed = []
        propertiesWindow._anyWrote = false
        propertiesWindow._pendingRgEdits = rgEdits
        propertiesWindow._pendingStripPaths = stripPaths
        if (metaEdits.length > 0 || stripPaths.length > 0)
            metaEditor.apply(metaEdits, stripPaths) // -> onApplied -> _applyRgPhase
        else
            propertiesWindow._applyRgPhase()
    }

    function _applyRgPhase() {
        if (propertiesWindow._pendingRgEdits.length > 0)
            rgEditor.apply(propertiesWindow._pendingRgEdits) // -> _reloadPhase
        else
            propertiesWindow._reloadPhase()
    }

    function _reloadPhase() {
        // Re-read only the files this pass wrote. reloadPaths hands back a
        // token and signals pathsReloaded(token) when exactly that pass is done
        // (an empty resolution still runs as an empty pass), so the sequencer
        // can neither hang on a pass that never runs nor finish early on an
        // unrelated pass's refreshed(). _alive, not a truthiness check: the
        // owning tab (and its reloader) may have died since open, and calling
        // into a dangling wrapper would throw and strand the sequencer.
        if (propertiesWindow._anyWrote && propertiesWindow._alive(propertiesWindow.reloader))
            propertiesWindow._reloadToken =
                propertiesWindow.reloader.reloadPaths(propertiesWindow._touchedPaths)
        else
            propertiesWindow._finishApply()
    }

    function _finishApply() {
        propertiesWindow._reloadToken = -1
        // Re-pull fresh on-disk values into all three tabs ONLY when something
        // actually landed. setSelection clears the metadata staging, so on a total
        // write failure we deliberately skip it: the staged edits survive and the
        // user can retry. (On partial success the written files refresh and the
        // failed ones keep their prior values; the footer reports the count.)
        if (propertiesWindow._anyWrote) {
            // Remap the open-time identity keys to CURRENT rows: positions may
            // have shifted since open, and the model itself may be dead (the
            // owning tab closed; _alive, not truthiness, for the same dangling-
            // wrapper reason as _reloadPhase). Tracks that left the playlist
            // drop out of the mapping.
            var modelAlive = propertiesWindow._alive(propertiesWindow._model)
            var cur = modelAlive
                    ? propertiesWindow._model.rowsForKeys(propertiesWindow._keys)
                    : []
            if (cur.length > 0) {
                propertiesWindow._rows = cur
                propertiesWindow._keys = propertiesWindow._model.trackKeys(cur)
                rgModel.setSelection(propertiesWindow._model, cur)
                metaModel.setSelection(propertiesWindow._model, cur)
                detailsModel.setSelection(propertiesWindow._model, cur)
            } else {
                // Nothing left to re-pull from (the tracks, or the whole tab,
                // are gone), but the writes landed on disk regardless. Fold the
                // staged values into the panes' baselines so the dirty markers
                // clear and the window shows what is on disk now; without this,
                // Apply looks like it did nothing (staging dots persist) even
                // though the files were written. Failed paths revert instead,
                // matching what the re-pull path shows for a failed file.
                metaModel.adoptEdits(propertiesWindow._metaFailed)
                rgModel.adoptEdits(propertiesWindow._rgFailed)
            }
            // A write marks the playlist tab dirty (dataChanged -> markDirty ->
            // debounced autosave). Force that write now so the .rwfpl cache
            // reflects the edit immediately, not just on the debounce.
            if (typeof playlistTabs !== "undefined" && playlistTabs)
                playlistTabs.flushPendingWrites()
        }

        var failed = propertiesWindow._metaFailed.concat(propertiesWindow._rgFailed)
        if (failed.length > 0)
            propertiesWindow._statusMsg = failed.length + " file(s) could not be written."
        // The strip intent is consumed by the pass that carried it,
        // success or not; a failure is reported above and Remove tags can be
        // re-invoked. (The METADATA staging's survival on total failure is a
        // separate mechanism and unchanged: setSelection was skipped above.)
        if (propertiesWindow._pendingStripPaths.length > 0) {
            propertiesWindow._stripAllStaged = false
            propertiesWindow._pendingStripPaths = []
        }
        propertiesWindow._anyWrote = false
        propertiesWindow._applying = false
        if (propertiesWindow._closeAfter && failed.length === 0)
            propertiesWindow._closeWindow()
        propertiesWindow._closeAfter = false
    }

    function _cancel() {
        // No staging to revert on the way out: the instance dies with its
        // staged edits, its models, and its editors.
        propertiesWindow._closeWindow()
    }

    // Tools "Reload info": force-re-read the selection's files from disk and
    // re-pull all three tabs. DISCARDS staged edits by design: setSelection
    // clears the metadata staging and resets the RG model the same way, so
    // what the window shows afterwards is exactly what is on disk.
    // Runs under the apply gate (_applying), so the footer, titlebar X, and
    // panes lock identically to an Apply pass, but completes through its own
    // token (see _infoReloadToken). A reloader dying mid-pass is covered the
    // same way as in the apply path: its destructor flushes the pending token
    // through onPathsReloaded, so this sequencer always finishes too.
    function _reloadInfo() {
        if (propertiesWindow._applying || scanController.busy)
            return
        if (!propertiesWindow._alive(propertiesWindow.reloader)
                || !propertiesWindow._alive(propertiesWindow._model)) {
            propertiesWindow._statusMsg = "Owning playlist is gone; nothing to reload."
            return
        }
        // Unique file paths from the durable identity keys (subsong siblings
        // share one physical file; the reloader is path-directed, so dedupe).
        var paths = propertiesWindow._selectionPaths()
        if (paths.length === 0)
            return
        // Discard any half-open cell editor first, like a tab switch does;
        // the data underneath is about to be replaced. The staged Remove tags
        // is discarded with the rest of the staging: reload is the
        // take-disk-truth action.
        metaPane.closeEdit()
        rgPane.closeAnyEditor()
        propertiesWindow._stripAllStaged = false
        propertiesWindow._statusMsg = ""
        propertiesWindow._busyText = "Reloading\u2026"
        propertiesWindow._applying = true
        propertiesWindow._infoReloadToken =
                propertiesWindow.reloader.reloadPaths(paths)
    }

    function _finishReload() {
        propertiesWindow._infoReloadToken = -1
        // Same remap as _finishApply: positions may have shifted since open,
        // and the model may have died while the pass ran (_alive, not
        // truthiness, for the dangling-wrapper reason documented there).
        var cur = propertiesWindow._alive(propertiesWindow._model)
                ? propertiesWindow._model.rowsForKeys(propertiesWindow._keys)
                : []
        if (cur.length > 0) {
            propertiesWindow._rows = cur
            propertiesWindow._keys = propertiesWindow._model.trackKeys(cur)
            rgModel.setSelection(propertiesWindow._model, cur)
            metaModel.setSelection(propertiesWindow._model, cur)
            detailsModel.setSelection(propertiesWindow._model, cur)
        } else {
            // Nothing left to re-pull from; the panes keep showing the
            // open-time snapshot, staged edits included.
            propertiesWindow._statusMsg = "Tracks are no longer in the playlist."
        }
        propertiesWindow._applying = false
    }

    // Both write editors get the playback link so apply() can stop the
    // engine before rewriting the loaded track's file (the tag-write interlock;
    // see AudioController::stopIfPlayingAny). audioController is the root
    // context property, reachable from any window.
    MetadataEditor { id: metaEditor; audio: audioController }
    ReplayGainEditor { id: rgEditor; audio: audioController }

    // Backing models for the two read-only tabs. Both are fed in openFor and copy
    // their TrackData, so they survive selection changes while the window is open.
    //  - metaModel:    the editable tag schema (the base model renders it read-only).
    //  - detailsModel: a MetadataModel in Details mode (Location + General only,
    //                  no "Metadata" section and no "Items Selected" row, since the
    //                  window title already carries the count).
    PropertiesMetadataModel { id: metaModel }
    MetadataModel { id: detailsModel; detailsMode: true }
    // The RG tab's backing model: rows, selection, staging, scan scope,
    // and the Summary in C++. Fed in openFor and on every re-pull, exactly
    // like the two above; the pane only renders it and forwards gestures.
    ReplayGainRowsModel { id: rgModel }

    // Off-thread RG scanner. Driven by the pane's scanRequested; on completion it
    // stages the measured values back into the pane (so they commit through the
    // normal Apply / OK), or, if canceled, stages nothing. Failures and the noop
    // case surface in the footer status line.
    ReplayGainScanController { id: scanController }

    Connections {
        target: scanController
        function onFinished(results, canceled, failedPaths) {
            if (canceled)
                return  // discard the whole batch; an album needs every track
            if (results.length > 0)
                rgModel.stageScanResults(results)
            if (failedPaths.length > 0)
                propertiesWindow._statusMsg =
                    failedPaths.length + " file(s) could not be scanned."
        }
    }

    Connections {
        target: metaEditor
        function onApplied(okCount, failedPaths) {
            propertiesWindow._metaFailed = failedPaths
            if (okCount > 0)
                propertiesWindow._anyWrote = true
            propertiesWindow._applyRgPhase()
        }
    }

    Connections {
        target: rgEditor
        function onApplied(okCount, failedPaths) {
            propertiesWindow._rgFailed = failedPaths
            if (okCount > 0)
                propertiesWindow._anyWrote = true
            propertiesWindow._reloadPhase()
        }
    }

    Connections {
        target: propertiesWindow.reloader
        enabled: propertiesWindow.reloader !== null
        ignoreUnknownSignals: true
        function onPathsReloaded(token) {
            // Only OUR pass completes a sequencer, and each token routes to
            // its own finisher (apply vs Tools reload; the tokens come from
            // the same reloader counter, so they can never collide).
            // refreshed() is not used here on purpose: it fires for any pass
            // that changed something and stays silent for one that did not
            // (see the banner). If the reloader dies while we wait, its
            // DESTRUCTOR flushes the pending tokens through this same handler
            // (QObject::destroyed cannot be relied on from QML Connections),
            // so both sequencers always finish.
            if (propertiesWindow._applying && token === propertiesWindow._reloadToken)
                propertiesWindow._finishApply()
            else if (propertiesWindow._applying && token === propertiesWindow._infoReloadToken)
                propertiesWindow._finishReload()
        }
    }

    Rectangle {
        id: windowBody
        anchors.fill: parent
        color: Theme.surfacePage
        radius: 8
        border.color: Theme.separatorStrong
        border.width: 1

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 1
            spacing: 0

            // ----- title bar -------------------------------------------------
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 40

                MouseArea {
                    anchors.fill: parent
                    onPressed: propertiesWindow.startSystemMove()
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: {
                        var n = propertiesWindow._rows.length
                        return n > 0 ? ("Properties - " + n + (n === 1 ? " item" : " items"))
                                     : "Properties"
                    }
                    color: Theme.textPrimary
                    font.family: propertiesWindow.uiFont
                    font.pixelSize: 13
                    font.weight: Font.Bold
                }

                ToolDialogCloseButton {
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    enabled: !propertiesWindow._applying
                    onClicked: propertiesWindow._cancel()
                }
            }

            // ----- tab strip -------------------------------------------------
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 30
                Layout.leftMargin: 12
                Layout.rightMargin: 12

                Row {
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    spacing: 4
                    Repeater {
                        model: ["Metadata", "Details", "ReplayGain"]
                        delegate: Rectangle {
                            id: tab
                            required property int index
                            required property string modelData
                            width: tabLabel.implicitWidth + 28
                            height: 28
                            radius: 2
                            color: propertiesWindow.currentTab === tab.index ? Theme.surfaceSelected : Theme.surfaceTabIdle
                            Text {
                                id: tabLabel
                                anchors.centerIn: parent
                                text: tab.modelData
                                color: propertiesWindow.currentTab === tab.index ? Theme.textPrimary : Theme.textDim
                                font.family: propertiesWindow.uiFont
                                font.pixelSize: 12
                            }
                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                height: 2
                                color: propertiesWindow.currentTab === tab.index ? Theme.accent : "transparent"
                            }
                            TapHandler { onTapped: propertiesWindow._switchTab(tab.index) }
                        }
                    }
                }
            }

            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.separatorStrong }

            // ----- tab content -----------------------------------------------
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                // Tab 0: editable tag schema (read-only at the base-model level).
                MetadataPropertiesPane {
                    id: metaPane
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    anchors.topMargin: 10
                    anchors.bottomMargin: 6
                    visible: propertiesWindow.currentTab === 0
                    enabled: !propertiesWindow._applying
                    model: metaModel
                    onEditFieldRequested: function (row) { propertiesWindow._openEditDialog(row) }
                    onAddFieldRequested: function () { propertiesWindow._openAddDialog() }
                }

                // Tab 1: read-only Location + General details, rendered by the same
                // Name/Value view the dock uses, in Details mode. Its font comes
                // from the Theme singleton by default, which resolves here too.
                // The id exists for the focus handoff (_focusCurrentTab).
                MetadataView {
                    id: detailsView
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    anchors.topMargin: 10
                    anchors.bottomMargin: 6
                    visible: propertiesWindow.currentTab === 1
                    model: detailsModel
                }

                ReplayGainPropertiesPane {
                    id: rgPane
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    anchors.topMargin: 10
                    anchors.bottomMargin: 6
                    visible: propertiesWindow.currentTab === 2
                    enabled: !propertiesWindow._applying && !scanController.busy
                    controller: audioController
                    model: rgModel

                    onScanRequested: function (items, mode) {
                        propertiesWindow._statusMsg = ""
                        scanController.scan(items, mode)
                    }
                    onScanNoop: function (message) {
                        propertiesWindow._statusMsg = message
                    }
                }
            }

            // ----- footer: status + Apply / OK / Cancel ----------------------
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 52
                color: Theme.headerBand

                FooterButton {
                    id: toolsBtn
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    label: "Tools"
                    enabled: !propertiesWindow._applying && !scanController.busy
                    onClicked: toolsMenu.open()

                    // The window-scoped Tools menu (foobar's bottom-left Tools
                    // button). Parented to the button and opened UPWARD via the
                    // y: -height binding, which settles once the popup sizes
                    // itself. Menu layout: metadata actions first, then the
                    // ReplayGain submenu (all five RG entries), then Reload
                    // info. Scan / clear act over EVERY row through the pane's
                    // all-rows entry points; results stage exactly like the
                    // pane's own right-click menu and commit through the normal
                    // Apply / OK path. Hover hints feed the footer's left text
                    // slot and clear when the menu closes; the auto-generated
                    // submenu TITLE item carries no hint (the parent menu's
                    // delegate creates it, so there is no per-item hint hook),
                    // only its children do.
                    ThemedMenu {
                        id: toolsMenu
                        x: 0
                        y: -height - 6
                        onClosed: propertiesWindow.toolsHint = ""

                        ThemedMenuItem {
                            text: "Add new field"
                            property string hint: "Add a new metadata field"
                            onHoveredChanged: if (hovered) propertiesWindow.toolsHint = hint
                            onTriggered: propertiesWindow._openAddDialog()
                        }
                        ThemedMenuItem {
                            text: "Auto track number"
                            property string hint: "Number the tracks 1..N and set the total"
                            onHoveredChanged: if (hovered) propertiesWindow.toolsHint = hint
                            onTriggered: {
                                metaPane.commitOpenEditor()
                                metaModel.autoNumberTracks()
                            }
                        }
                        ThemedMenuItem {
                            text: "Remove tags"
                            property string hint: "Discards all metadata and removes known tag types from tracks being worked with; written on Apply / OK"
                            onHoveredChanged: if (hovered) propertiesWindow.toolsHint = hint
                            onTriggered: propertiesWindow._removeAllTags()
                        }
                        MenuSeparator {}
                        ThemedMenu {
                            title: "ReplayGain"

                            ThemedMenuItem {
                                text: "Clear ReplayGain information"
                                property string hint: "Stage empty ReplayGain values on every track; written on Apply / OK"
                                onHoveredChanged: if (hovered) propertiesWindow.toolsHint = hint
                                onTriggered: rgModel.clearAllReplayGain()
                            }
                            ThemedMenuItem {
                                text: "Scan track gain"
                                property string hint: "Measure every track's gain and peak; results are staged for review"
                                onHoveredChanged: if (hovered) propertiesWindow.toolsHint = hint
                                onTriggered: rgPane.requestScanAll(0)
                            }
                            ThemedMenuItem {
                                text: "Scan album gain (as one album)"
                                property string hint: "Measure all tracks as one album; results are staged for review"
                                onHoveredChanged: if (hovered) propertiesWindow.toolsHint = hint
                                onTriggered: rgPane.requestScanAll(1)
                            }
                            ThemedMenuItem {
                                text: "Scan album gain (multiple albums, by tags)"
                                property string hint: "Group by album artist / date / album tags and measure each album separately"
                                onHoveredChanged: if (hovered) propertiesWindow.toolsHint = hint
                                onTriggered: rgPane.requestScanAll(2)
                            }
                            MenuSeparator {}
                            ThemedMenuItem {
                                id: skipExistingItem
                                text: "Skip scanning tracks/albums with info present"
                                checkable: true
                                property string hint: "When set, scans skip tracks (or whole albums) that already carry values"
                                onHoveredChanged: if (hovered) propertiesWindow.toolsHint = hint
                                // Clicking a checkable item auto-toggles `checked`,
                                // which would break a plain binding to the setting;
                                // the Binding element re-asserts the settings-backed
                                // truth whenever it changes, and the trigger just
                                // pushes the freshly toggled state into it.
                                onTriggered: audioController.replayGainScanSkipExisting = skipExistingItem.checked
                                Binding {
                                    target: skipExistingItem
                                    property: "checked"
                                    value: audioController.replayGainScanSkipExisting
                                }
                            }
                        }
                        MenuSeparator {}
                        ThemedMenuItem {
                            text: "Reload info"
                            property string hint: "Re-read tags from disk, discarding any staged edits"
                            onHoveredChanged: if (hovered) propertiesWindow.toolsHint = hint
                            onTriggered: propertiesWindow._reloadInfo()
                        }
                    }
                }

                // Footer left text slot: one slot, three tenants in priority
                // order. The Tools hover hint while the menu is open, the busy
                // label while a pass runs, else the status / failure message.
                Text {
                    anchors.left: toolsBtn.right
                    anchors.leftMargin: 12
                    anchors.right: footerButtons.left
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    visible: text.length > 0
                    text: propertiesWindow.toolsHint.length > 0 ? propertiesWindow.toolsHint
                        : propertiesWindow._applying ? propertiesWindow._busyText
                        : propertiesWindow._statusMsg
                    color: (propertiesWindow.toolsHint.length > 0 || propertiesWindow._applying)
                           ? Theme.textInactive
                           : Theme.dangerSoft
                    font.family: propertiesWindow.uiFont
                    font.pixelSize: 12
                }

                RowLayout {
                    id: footerButtons
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10

                    FooterButton {
                        label: "Cancel"
                        enabled: !propertiesWindow._applying
                        onClicked: propertiesWindow._cancel()
                    }
                    FooterButton {
                        label: "Apply"
                        // The strip flag is an enabler in its own right:
                        // Remove tags on already-blank fields stages nothing in
                        // either model, yet the strip still has work to do.
                        enabled: (metaModel.dirty || rgModel.dirty
                                  || propertiesWindow._stripAllStaged)
                                 && !propertiesWindow._applying
                        onClicked: propertiesWindow._apply(false)
                    }
                    FooterButton {
                        label: "OK"
                        accent: true
                        enabled: !propertiesWindow._applying
                        onClicked: propertiesWindow._apply(true)
                    }
                }
            }
        }

        // Scan progress overlay: dims and blocks the window body while a scan
        // runs. Bound to the controller's busy flag, so it shows and hides itself.
        ReplayGainScanProgress {
            anchors.fill: parent
            controller: scanController
        }

        // Edit Value dialog overlay (multi-track editing): dims and blocks the body
        // and centers the dialog. Hosted in-window rather than as a second OS window
        // so its shortcuts and focus stay scoped to THIS instance even when several
        // Properties windows are open.
        Item {
            anchors.fill: parent
            visible: propertiesWindow._editDialogActive
            z: 50

            Rectangle {
                anchors.fill: parent
                color: "#000000"
                opacity: 0.5
                MouseArea { anchors.fill: parent } // swallow clicks to the body behind
            }

            EditValueDialog {
                id: editDialog
                anchors.centerIn: parent
                onCommitted: function (perTrackValues) {
                    metaModel.stageFieldValues(editDialog.fieldKey, perTrackValues)
                    propertiesWindow._editDialogActive = false
                }
                onAddRequested: function (key, perTrackValues) {
                    var err = metaModel.addCustomFieldValues(key, perTrackValues)
                    if (err !== "")
                        editDialog.keyError = err   // stay open, show the inline reason
                    else
                        propertiesWindow._editDialogActive = false
                }
                onCanceled: propertiesWindow._editDialogActive = false
            }
        }
    }

    // Resize edges: this window is frameless on every platform, so the grips
    // are unconditional. Declared after windowBody so they sit above the scan
    // and Edit Value overlays; resizing during a write or scan is harmless
    // (both overlays keep blocking the body), so no _applying gate. topInset
    // keeps the top strip off the title bar's move zone.
    WindowResizeGrips {
        target: propertiesWindow
        topInset: 40
    }

    component FooterButton: Rectangle {
        id: fbtn
        property string label: ""
        property bool accent: false
        signal clicked()

        implicitWidth: Math.max(72, btnText.implicitWidth + 28)
        implicitHeight: 30
        radius: 5
        color: !enabled ? Theme.surfaceControl
             : fbtnHover.hovered ? (accent ? Theme.accentButtonHover : Theme.surfaceControlHover)
             : (accent ? Theme.accentSoft : Theme.buttonFace)
        border.color: accent ? "transparent" : Theme.border
        border.width: accent ? 0 : 1
        opacity: enabled ? 1.0 : 0.5

        Text {
            id: btnText
            anchors.centerIn: parent
            text: fbtn.label
            color: fbtn.accent ? Theme.textOnAccent : Theme.textPrimary
            font.family: propertiesWindow.uiFont
            font.pixelSize: 12
            font.weight: Font.Bold
        }
        HoverHandler { id: fbtnHover }
        TapHandler { onTapped: if (fbtn.enabled) fbtn.clicked() }
    }
}
