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

// SanitizerSuppressions.cpp
//
// Baked-in default suppressions for the sanitizer profiles. The runtimes call
// these weak hooks at init and treat the returned string like a suppressions
// file's contents; anything passed via TSAN_OPTIONS / LSAN_OPTIONS
// suppressions=... is ADDED on top, so ui/Sources/tools/tsan.supp and
// lsan.supp stay usable as the no-rebuild staging area for triage. The proven
// floor is baked into the binary because delivering *SAN_OPTIONS through IDE
// run configurations is fragile (CLion re-quotes values containing '=' / ':'
// on round-trip, and a quoted value is silently ignored by the runtime's flag
// parser); the binary route has no delivery chain to break, on either
// platform or any launcher.
//
// ADMISSION CRITERIA, same spirit for both lists, spelled out in full in
// ui/Sources/tools/tsan.supp: an entry is admitted only when the implicating
// stacks are entirely third-party (Qt, Mesa, system libraries), so it cannot
// hide a rawform defect. For races that means BOTH racing stacks (provenance
// sections do not count); for leaks it means the allocation stack. A leak
// allocated through rawform code is never suppressed; it gets fixed.
// Third-party is judged by authorship, not by which binary holds the frame:
// code Qt's build tooling generates into the rawform executable (qmlcachegen's
// module loader) is Qt's, provided the pattern is anchored so it cannot reach
// a hand-written rawform frame.
//
// Each hook exists ONLY under its sanitizer (Clang advertises via
// __has_feature, GCC via __SANITIZE_*__); a normal build compiles this TU to
// nothing.

#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define RAWFORM_TSAN_BUILD 1
#  endif
#  if __has_feature(address_sanitizer)
#    define RAWFORM_ASAN_BUILD 1
#  endif
#elif defined(__SANITIZE_THREAD__)
#  define RAWFORM_TSAN_BUILD 1
#elif defined(__SANITIZE_ADDRESS__)
#  define RAWFORM_ASAN_BUILD 1
#endif

#if defined(RAWFORM_TSAN_BUILD)

extern "C" const char* __tsan_default_suppressions() {
    return
        // QtConcurrent ThreadEngine/IterateKernel self-deletion racing late
        // pool-thread probes (shouldStartThread vs ~IterateKernel in
        // asynchronousFinish). Qt-internal on both racing stacks; see
        // tools/tsan.supp for the full report anatomy and context.
        "race:QtConcurrent::\n"

        // glibc's timezone cache (tzset_internal): the cached TZ string is
        // freed/strdup'd under glibc's INTERNAL lock, which TSan cannot see
        // in an uninstrumented libc, so concurrent local-time lookups (GUI
        // thread vs a worker statting files -> QDateTime local conversion)
        // report as an unsynchronized malloc/free pair. Both racing stacks
        // are pure glibc; rawform frames appear only as provenance. A
        // canonical TSan fixture. main() also warms the cache with tzset()
        // before any thread exists, which removes the pair in normal runs;
        // this entry covers the residual (a genuine TZ change at runtime).
        "race:tzset_internal\n"

        // --- Linux (GCC libtsan, uninstrumented prebuilt Qt) --------------
        // Structural cause for this whole block: Qt6's QMutex/atomics are
        // futex-based on Linux, INVISIBLE to TSan when Qt itself is not
        // instrumented, so handoffs synchronized purely inside Qt report as
        // races. called_from_lib ignores interceptor calls whose caller sits
        // inside the named library; our own instrumented frames still report
        // normally, which keeps the rename/tag-write drill surface (our TUs
        // end to end) fully sighted. Observed set, Fedora/GCC 16:
        //  - libdbus internal rmutex init races + its known lock-order
        //    inversion (QDBusConnection thread);
        //  - glib g_wakeup eventfd choreography (pixmap reader thread);
        //  - allocator handoffs inside libQt6Qml/Quick/Core (unsymbolized
        //    frames; no race: pattern is possible, the library scope is).
        // CAVEAT for future triage: the same blindness means a Linux-only
        // report with BOTH racing stacks in rawform frames, where the claimed
        // missing sync is a Qt handoff (QFuture result delivery, queued
        // signal), can still be a false positive. Reason about the actual
        // handoff before treating it as ours; macOS agreement is the
        // tiebreaker.
        // CONSTRAINT: each called_from_lib entry must match EXACTLY ONE
        // loaded library; on the second match the runtime Die()s at
        // library-load time, i.e. app startup (the scenario this prevents:
        // exit 66 before any window). An unanchored "libQt6Qml" also matches
        // libQt6QmlModels/WorkerScript/Meta, "libQt6Quick" the
        // Controls2/Templates2/Layouts/Shapes family. The ".so" in the
        // pattern lands on the base module's name boundary, which is what
        // makes each entry unique. If a sibling module (e.g. QuickControls2)
        // ever produces its own reports, it gets its own exact entry; never
        // widen these.
        "called_from_lib:libdbus-1.so\n"
        "called_from_lib:libglib-2.0.so\n"
        "called_from_lib:libQt6Core.so\n"
        "called_from_lib:libQt6Qml.so\n"
        "called_from_lib:libQt6Quick.so\n"
        // The layer beneath the QtConcurrent reports: the dbus noise
        // resurfacing one layer up in Qt's wrapper, QtGui raster/paint/QImage
        // teardown churn (the bulk of it), the Wayland client plugin, and the
        // Mesa driver stack. All verdicts per the criteria; each pattern
        // matches exactly one loaded library (the gallium name is versioned,
        // "libgallium" still matches only it).
        "called_from_lib:libQt6DBus.so\n"
        "called_from_lib:libQt6Gui.so\n"
        "called_from_lib:libQt6WaylandClient.so\n"
        "called_from_lib:libgallium\n"
        "called_from_lib:libvulkan_nouveau.so\n"
        "called_from_lib:libEGL_mesa.so\n"
        // Below Qt: libwayland-client's proxy/ring-buffer/dispatch internals
        // (a notorious TSan fixture; its sync is its own), libffi's closure
        // dispatch underneath it, PipeWire's rt module doing the rtkit
        // property dance, and Qt's wayland-egl integration plugin. All
        // third-party racing stacks; ".so" anchoring keeps each to one
        // loaded library (module-rt.so cannot match an rtp-* module name).
        "called_from_lib:libwayland-client.so\n"
        "called_from_lib:libffi.so\n"
        "called_from_lib:libpipewire-module-rt.so\n"
        "called_from_lib:libqt-plugin-wayland-egl.so\n"
#if defined(__SANITIZE_THREAD__)
        // GCC/Linux ONLY, deliberately absent from the Clang/macOS build:
        // the engine->GUI bridge's queued-connection handoffs
        // (QMetaObject::invokeMethod lambdas in EngineListenerBridge, and
        // applyTrack consuming the marshaled facts). Their synchronization
        // is Qt's event-queue mutex plus implicit-sharing refcounts, futex/
        // inlined atomics on Linux, INVISIBLE to TSan with uninstrumented
        // Qt, so every handoff reports as our-frame races (~20 per
        // session, observed). This is the documented exception to the
        // admission criteria, licensed by the tiebreaker: the identical
        // paths run TSan-CLEAN on macOS, whose runtime sees Qt's locking.
        // COST, stated plainly: a genuine Linux-only race involving these
        // two symbols would be masked here; macOS and the engine's own
        // instrumented tests remain the sighted coverage for them. Making
        // the handoff visible to the runtime (__tsan_release/__tsan_acquire
        // annotations on the bridge) is outside this list's scope; these two
        // entries exist until that annotation does.
        "race:rawform::EngineListenerBridge\n"
        "race:rawform::AudioController::applyTrack\n"

        // qmlcachegen's generated module loader: the Q_GLOBAL_STATIC Registry
        // QHash of compiled QML units, freed by the atexit handler on the
        // main thread, against a read of the same hash by the QML type
        // loader thread (QQmlMetaType::findCachedCompilationUnit, a QThread
        // the QML engine starts and joins in ~QQmlEngine). Three reports per
        // exit, one per heap field the destructor frees. The join is
        // QThread::wait, futex-based on Linux, so the runtime never sees the
        // happens-before that is really there. called_from_lib:libQt6Qml.so
        // cannot cover it: both accesses are issued from the generated TU
        // compiled (instrumented) into this binary. Anchored on the generated
        // file name, which the runtime matches like a function name and which
        // no hand-written rawform frame carries. Absent on macOS, whose
        // runtime sees the join: the tiebreaker that licenses the entry.
        "race:rawform_qmlcache_loader.cpp\n"
#endif
        ;
}

#endif // RAWFORM_TSAN_BUILD

#if defined(RAWFORM_ASAN_BUILD)

// LeakSanitizer runs piggybacked on the ASan profile; leak: entries match a
// frame anywhere in the ALLOCATION stack.
extern "C" const char* __lsan_default_suppressions() {
    return
        // Mesa EGL/Wayland screen-factory init caches (zink/dri2 under
        // eglInitialize, reached through QtWaylandClient's EGL integration):
        // one-shot process-lifetime allocations Mesa never frees, a
        // well-known LSan fixture on Wayland. Anchored at eglInitialize,
        // which is on the allocation stack of each and is unreachable from
        // rawform allocations.
        "leak:eglInitialize\n"

        // libxkbcommon keymap/compose state, allocated once by the Qt
        // Wayland platform plugin (QWaylandDisplay::setupConnection, and the
        // text-input compose table the first time any text field in the
        // session takes a key; which field is incidental). Process-lifetime
        // singletons the Wayland client teardown never frees; every frame of
        // every allocation stack is libxkbcommon's own, so the module
        // pattern cannot mask a rawform leak.
        "leak:libxkbcommon\n";
}

#endif // RAWFORM_ASAN_BUILD
