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

// PlaylistHeaderMenu.qml
//
// The playlist header's right-click column menu: for a custom column, an
// "Edit custom column..." entry that jumps to the manager; a "Columns" submenu
// that is the checkable catalog of every native field (checked = shown); a
// "Custom columns" submenu with the defined custom columns, toggled the same
// way; and "Manage custom columns...". The host (PlaylistView) creates it on
// the first right-click and opens it through popupForColumn, which refreshes
// the catalog from the model and the target column's identity per open.
//
// The ThemedMenuBar shape: state comes DOWN through `model`, requests go UP
// through the two signals, so this file reaches no view internals. The host
// owns the toggling (it has the header widths and the selection to re-assert)
// and the manager window.

pragma ComponentBehavior: Bound

import QtQml.Models
import QtQuick
import QtQuick.Controls.Basic
import com.rawform.app

ThemedMenu {
    id: headerMenu

    // The ACTIVE tab's filter proxy, bound by the host; the check marks query
    // it live (isColumnShown) and popupForColumn reads its catalogs.
    property PlaylistFilterProxy model: null

    /// Show or hide the column with this field id; `width` is the catalog's
    /// seed width for a column being shown.
    signal toggleColumn(string fieldId, int width, bool shown)
    /// Open the custom-columns manager, scrolled to `fieldId` ("" for none).
    signal manageCustomColumns(string fieldId)

    // The native catalog, split on the composite flag (see
    // availableColumns: pre-sorted plain-alpha then composite-alpha; the
    // split decides which side of the submenu separator an entry lands on).
    property var colsPlain: []
    property var colsComposite: []
    property var customCols: []
    // Bumped on every open; the check-mark bindings read it so they
    // re-query isColumnShown() fresh per popup (they are invokable calls,
    // not tracked properties).
    property int catalogRevision: 0
    property string targetFieldId: ""
    property string targetTitle: ""
    // The right-clicked column is a custom one ("custom:<id>"). Native
    // columns have no per-column context action (their header label is
    // code-only; build a custom column to show a field under another name).
    property bool targetIsCustom: false

    // Refresh the catalogs and the target from the model, then pop up at the
    // cursor. Clear-then-assign forces a full Instantiator teardown/rebuild
    // cycle for the submenu items, so no delegate instance survives a reopen,
    // including one whose `checked` binding a previous interactive toggle
    // broke (a click WRITES checked, severing the declarative binding on that
    // instance for good).
    function popupForColumn(column, title) {
        headerMenu.colsPlain = []
        headerMenu.colsComposite = []
        headerMenu.customCols = []
        var catalog = headerMenu.model ? headerMenu.model.availableColumns() : []
        headerMenu.colsPlain = catalog.filter(function (e) { return !e.composite })
        headerMenu.colsComposite = catalog.filter(function (e) { return e.composite })
        headerMenu.customCols = headerMenu.model
            ? headerMenu.model.customColumnCatalog() : []
        // Kick the live-query check-mark bindings (see catalogRevision).
        headerMenu.catalogRevision++
        if (headerMenu.model) {
            headerMenu.targetFieldId = headerMenu.model.columnFieldId(column)
            headerMenu.targetTitle = title
            headerMenu.targetIsCustom =
                headerMenu.targetFieldId.indexOf("custom:") === 0
        } else {
            headerMenu.targetFieldId = ""
            headerMenu.targetTitle = ""
            headerMenu.targetIsCustom = false
        }
        headerMenu.popup()
    }

    // Custom columns only: jump to the manager, scrolled to this record.
    // ThemedMenuItem rather than a bare MenuItem: direct children bypass
    // the delegate, so the bare form rendered unthemed AND had no
    // fitWidth, meaning the (arbitrarily long) title never drove the
    // popup width.
    ThemedMenuItem {
        text: "Edit custom column “" + headerMenu.targetTitle + "”…"
        collapsed: !headerMenu.targetIsCustom

        onTriggered: headerMenu.manageCustomColumns(headerMenu.targetFieldId)
    }
    MenuSeparator {
        visible: headerMenu.targetIsCustom
        height: visible ? implicitHeight : 0
    }
    ThemedMenu {
        id: columnsSubmenu
        title: "Columns"

        // Two Instantiators, one per catalog group, around a static
        // separator. availableColumns() delivers the sort (plain fields
        // alphabetically, then the composite presentation fields
        // alphabetically); the split here only decides which side of the
        // separator an entry lands on. Insert mechanics: the separator is
        // a declared child (item 0 at creation); the plain group inserts
        // at 0..n-1, pushing the separator to position n; the composite
        // group APPENDS, landing after it. The per-popup clear-then-assign
        // rebuild replays that same sequence, so the layout is stable
        // across reopens.
        Instantiator {
            model: headerMenu.colsPlain
            delegate: ThemedMenuItem {
                // Injected by the Instantiator's delegate model; under Bound
                // it must be declared or creation fails with a loud
                // "required property not set" (never a silent undefined).
                required property var modelData
                checkable: true
                // Live query against the CURRENT model, kicked per popup;
                // never the catalog snapshot's `visible`, so the mark can't
                // show another tab's or an earlier open's state.
                checked: {
                    headerMenu.catalogRevision
                    headerMenu.model ? headerMenu.model.isColumnShown(modelData.fieldId)
                                     : false
                }
                text: modelData.title

                // `checked` is the NEW state after the click toggled it, so it
                // is exactly the desired visibility to apply.
                onTriggered: headerMenu.toggleColumn(modelData.fieldId, modelData.width, checked)
            }
            onObjectAdded: function (index, object) { columnsSubmenu.insertItem(index, object) }
            onObjectRemoved: function (index, object) { columnsSubmenu.removeItem(object) }
        }
        MenuSeparator {
            visible: headerMenu.colsPlain.length > 0
                     && headerMenu.colsComposite.length > 0
        }
        Instantiator {
            model: headerMenu.colsComposite
            delegate: ThemedMenuItem {
                required property var modelData
                checkable: true
                // Same live query as the plain group above.
                checked: {
                    headerMenu.catalogRevision
                    headerMenu.model ? headerMenu.model.isColumnShown(modelData.fieldId)
                                     : false
                }
                text: modelData.title

                onTriggered: headerMenu.toggleColumn(modelData.fieldId, modelData.width, checked)
            }
            onObjectAdded: function (index, object) { columnsSubmenu.addItem(object) }
            onObjectRemoved: function (index, object) { columnsSubmenu.removeItem(object) }
        }
    }

    // Defined custom columns, toggled exactly like the built-in catalog
    // (same toggleColumn path; the field ids are "custom:<id>", which the
    // host resolves against the registry). Definitions are created/edited
    // in the "Manage custom columns..." window.
    ThemedMenu {
        id: customColumnsSubmenu
        title: "Custom columns"

        // A bare disabled hint when none are defined yet, so the submenu is
        // never confusingly empty.
        ThemedMenuItem {
            collapsed: headerMenu.customCols.length !== 0
            enabled: false
            text: "Nothing to see here!"
            textItalic: true
        }
        Instantiator {
            delegate: ThemedMenuItem {
                required property var modelData
                checkable: true
                // Same live query as the native catalog above.
                checked: {
                    headerMenu.catalogRevision
                    headerMenu.model ? headerMenu.model.isColumnShown(modelData.fieldId)
                                     : false
                }
                text: modelData.title.length > 0 ? modelData.title : "(unnamed)"

                onTriggered: headerMenu.toggleColumn(modelData.fieldId, modelData.width, checked)
            }
            model: headerMenu.customCols

            onObjectAdded: function (index, object) { customColumnsSubmenu.insertItem(1 + index, object) }
            onObjectRemoved: function (index, object) { customColumnsSubmenu.removeItem(object) }
        }
    }
    MenuSeparator {}
    ThemedMenuItem {
        text: "Manage custom columns…"

        onTriggered: headerMenu.manageCustomColumns("")
    }
}
