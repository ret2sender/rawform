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

// QmlSingletonInstance.h
//
// The one instance pointer behind each C++-owned QML singleton.
//
// The app-global services (PlaylistTabs, AudioController, SettingsStore and
// the rest) are constructed on main()'s stack, wired together there, and
// reached from QML as singletons of the com.rawform.app module, so every
// access is a qualified, lint-checked, ahead-of-time compiled
// `AudioController.play()` rather than a dynamically scoped name. The QML
// engine instantiates a singleton through a static
// `create(QQmlEngine*, QJSEngine*)`; for these types that factory must hand
// back the instance main() already built, not a fresh one. This template
// holds that pointer per type: main() calls `set` before the root component
// is created, and each service's factory is the one-liner
// `return QmlSingletonInstance<T>::take(engine);`.
//
// The factory lives in a QML_FOREIGN wrapper struct next to each class
// (`PlaylistTabsSingleton` and so on), not in the class as QML_SINGLETON.
// The engine's construction-mode rule prefers a default constructor over a
// create() on the type itself, so a service with `T(QObject* parent =
// nullptr)` would be built a second time, empty and unwired, and QML would
// silently bind to that one; a wrapper's create() is consulted first,
// whatever the class's constructors look like.
//
// Ownership stays in C++. `take` marks the object CppOwnership before the
// engine sees it, so the engine never deletes a stack object; main()'s
// declaration order (every service before the engine) keeps each one alive
// past the last QML access.

#pragma once

#include <QJSEngine>
#include <QQmlEngine>
#include <QThread>
#include <QtGlobal>  // Q_ASSERT, Q_UNUSED

namespace rawform {

template <typename T>
class QmlSingletonInstance {
public:
    /// Record the instance the QML singleton of type T resolves to. Called
    /// once from main(), before the root component is created; nullptr
    /// unregisters (the tests never register anything).
    static void set(T* instance) { s_instance = instance; }

    /// The singleton factory body: the registered instance, marked
    /// CppOwnership. Asserts that main() registered one and that the engine
    /// runs on its thread (a singleton is per engine, and every rawform
    /// engine lives on the GUI thread with the services).
    [[nodiscard]] static T* take(QQmlEngine* engine) {
        Q_ASSERT(s_instance != nullptr);
        Q_ASSERT(engine->thread() == s_instance->thread());
        Q_UNUSED(engine);  // the assert is the only reader, and it compiles out
        QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
        return s_instance;
    }

private:
    static inline T* s_instance = nullptr;
};

}  // namespace rawform
