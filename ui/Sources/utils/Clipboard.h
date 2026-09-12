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

#pragma once

// =============================================================================
// Clipboard.h
//
// A minimal QML-exposed wrapper over the system clipboard, since QML has no
// direct read access to it. Consumers: EditValueDialog's per-track cell
// copy/paste (so values round-trip with a spreadsheet column),
// MetadataPropertiesPane's copy/paste of the selected fields, and
// AboutWindow's click-to-copy version line. Header-only: the two calls are
// trivial and stateless, so there is no .cpp.
// =============================================================================

#include <QClipboard>
#include <QGuiApplication>
#include <QObject>
#include <QQmlEngine> // QML_ELEMENT
#include <QString>

namespace rawform {

class Clipboard : public QObject {
    Q_OBJECT
    QML_ELEMENT

public:
    explicit Clipboard(QObject* parent = nullptr) : QObject(parent) {}

    /// The clipboard's current plain text (empty if unavailable). Newlines are
    /// preserved, so a spreadsheet column pastes as one value per line.
    Q_INVOKABLE [[nodiscard]] QString text() const {
        const QClipboard* cb = QGuiApplication::clipboard();
        return cb ? cb->text() : QString{};
    }

    /// Replace the clipboard's plain text.
    Q_INVOKABLE void setText(const QString& value) const {
        if (QClipboard* cb = QGuiApplication::clipboard())
            cb->setText(value);
    }
};

} // namespace rawform
