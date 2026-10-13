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

// PresetRow.qml
//
// The named-pattern preset row shared by the Rename Files and Find dialogs:
// an editable combo whose text doubles as the save-as name, and Load / Save /
// Delete buttons over a NamedPatternStore. The dialog keeps the pattern field
// itself: it binds `pattern` to that field's text (what Save stores) and
// applies `loaded` to it (what Load produced); the row never reaches into the
// dialog. Status messages for the dialog's own status line arrive through
// `flash`, with the same wording in both dialogs; a store write that failed
// flashes the store's message instead of the success line.
//
// Programmatic text changes go through setName, which writes BOTH the combo's
// editText and the editor's text: the content item's declarative text binding
// is severed the first time the user types in the field (the standard QML
// binding break on user input), after which editText changes alone stop
// reaching the display, observed as a deleted preset's name sticking in the
// box and selections not rendering.
//
// The instantiation site owns `store`, `pattern`, `busy`, and the two signal
// handlers.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import com.rawform.app

RowLayout {
    id: root

    /// The store behind the row (RenamePatternStore, SearchPresetStore).
    required property NamedPatternStore store
    /// The dialog's current pattern: what Save stores under the typed name.
    property string pattern: ""
    /// True while the dialog runs a pass that must not be disturbed; every
    /// control in the row disables.
    property bool busy: false

    /// The typed or picked preset name, trimmed as Save would store it.
    readonly property string presetName: combo.editText

    /// Load pressed: `pattern` is the stored pattern for the picked name.
    signal loaded(string pattern)
    /// A one-line status for the dialog's status line.
    signal flash(string message)

    // The failure the store reported during the last Save or Delete, if any;
    // the button's own success line yields to it.
    property string _persistFailure: ""

    spacing: 8

    /// Set the combo's text through both channels (see the banner).
    function setName(name) {
        combo.editText = name
        editor.text = name
    }

    /// Preselect `name` when the store still holds it; returns the stored
    /// pattern, or "" when the name is unknown (nothing changes then).
    function selectName(name) {
        var stored = root.store.patternFor(name)
        if (stored.length === 0)
            return ""
        var idx = combo.find(name)
        if (idx >= 0)
            combo.currentIndex = idx
        root.setName(name)
        return stored
    }

    ThemedComboBox {
        id: combo
        Layout.fillWidth: true
        editable: true   // the edit text doubles as the save-as name
        enabled: !root.busy
        textRole: "name"
        model: root.store.catalog()
        // The editable content item replaces the shared combo's read-only
        // label: selectByMouse on declaratively (the Basic style ships its
        // editor with it off), the themed edit context menu, and no
        // duck-typed onCompleted poke at style internals. ComboBox auto-syncs
        // editText with a TextInput-based contentItem, so the one-way text
        // binding here is the whole wiring. background null: the combo draws
        // its own frame; the TextField's would double-border.
        contentItem: TextField {
            id: editor
            background: null
            enabled: combo.editable
            font: combo.font
            text: combo.editable ? combo.editText : combo.displayText
            selectByMouse: true
            verticalAlignment: Text.AlignVCenter
            ContextMenu.menu: ThemedEditMenu { editor: editor }
        }
        Accessible.name: "Preset"
        // Selection must repaint through setName too: with the text binding
        // severed, activating an entry updates editText but not the display,
        // so clicking another preset looked dead.
        onActivated: function (index) {
            root.setName(combo.textAt(index))
        }
        Connections {
            target: root.store
            function onPersistFailed(message) {
                root._persistFailure = message
                root.flash(message)
            }
            function onPatternsChanged() {
                // Reassigning the model recreates the entries and stomps the
                // edit text; preserve a typed name that still EXISTS (the
                // save flow), and clear one that does not (the delete flow;
                // restoring it left a ghost the combo could not select away
                // from).
                var typed = combo.editText
                combo.model = root.store.catalog()
                var i = combo.find(typed)
                if (i >= 0) {
                    combo.currentIndex = i
                    root.setName(typed)
                } else {
                    combo.currentIndex = -1
                    root.setName("")
                }
            }
        }
    }
    ToolDialogButton {
        label: "Load"
        enabled: !root.busy
                 && combo.editText.length > 0
                 && root.store.patternFor(combo.editText).length > 0
        onClicked: {
            root.store.setLastUsed(combo.editText)
            root.loaded(root.store.patternFor(combo.editText))
            root.flash("Loaded preset '" + combo.editText + "'")
        }
    }
    ToolDialogButton {
        label: "Save"
        enabled: !root.busy
                 && combo.editText.trim().length > 0
                 && root.pattern.length > 0
        onClicked: {
            root._persistFailure = ""
            root.store.save(combo.editText, root.pattern)
            root.store.setLastUsed(combo.editText)
            if (root._persistFailure.length === 0)
                root.flash("Saved preset '" + combo.editText.trim() + "'")
        }
    }
    ToolDialogButton {
        label: "Delete"
        // Only a name that actually resolves to a preset can be deleted (the
        // same existence test Load uses).
        enabled: !root.busy
                 && combo.editText.length > 0
                 && root.store.patternFor(combo.editText).length > 0
        onClicked: {
            var name = combo.editText
            root._persistFailure = ""
            root.store.remove(name)
            // patternsChanged already cleared the box (the name no longer
            // resolves); this is the belt to that brace.
            combo.currentIndex = -1
            root.setName("")
            if (root._persistFailure.length === 0)
                root.flash("Deleted preset '" + name + "'")
        }
    }
}
