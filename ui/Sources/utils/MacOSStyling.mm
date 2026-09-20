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

// MacOSStyling.mm
//
// Objective-C++ implementation of native macOS window styling.
//
// This file is compiled as Objective-C++ (.mm) so it can call AppKit APIs
// directly. It bridges from Qt's QQuickWindow to the underlying NSWindow to
// configure the title bar appearance for rawform.
//
// The styling performs five operations:
//   1. Adds `NSWindowStyleMaskFullSizeContentView` so the Qt content area
//      extends behind the native title bar.
//   2. Sets the title bar to transparent and hides the window title text, so
//      only the traffic-light buttons remain visible over rawform's own QML
//      title bar.
//   3. Disables AppKit's default window dragging. rawform's QML TitleBar
//      drag handler moves the window via `startSystemMove()`; leaving AppKit
//      dragging enabled as well would double-drive it.
//   4. Measures the native title bar height (from the traffic-light buttons'
//      superview) and passes it to QML as the `nativeTitleBarHeight` property,
//      so the layout can offset its content by exactly that amount.
//   5. Publishes the system's title-bar double-click preference (System
//      Settings > Desktop & Dock > "Double-click a window's title bar to") to
//      QML as the `macTitleBarDoubleClickAction` property. With the content
//      view covering the title bar band, the double-click reaches Qt, not
//      AppKit's title bar, so the native zoom / minimize never fires on its
//      own; QML performs the action instead, and this property tells it which.
//      The value is re-read live through key-value observation of the
//      defaults key, so a change in System Settings applies without a restart.
//
// The styling is NOT one-shot. Entering and exiting native fullscreen (the
// green traffic light on a resizable window), and miniaturize/deminiaturize,
// make AppKit re-assert Qt's own style mask, which wipes our customization.
// Left unhandled, exiting fullscreen restores an opaque native title bar over
// the QML content, with no drag. To keep the look sticky we install a small
// observer (RawformWindowStyler) that re-applies on those transitions.
//
// One subtlety the observer also handles: Qt runs its own geometry update on
// the fullscreen-exit notification, and re-toggling the style mask inline
// races that pass and loses height (the scenario this prevents: the window
// coming back one title bar shorter on every fullscreen round trip). So the
// observer captures the true windowed frame just before entering fullscreen,
// and on exit defers one runloop pass (past Qt's update) before re-applying
// the styling and stamping that exact frame back.

#include "utils/MacOSStyling.h"

#if defined(__APPLE__)

#import <AppKit/AppKit.h>

#include <QDebug>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QString>

/// Observer that owns the styling and keeps it applied across the window
/// lifecycle events that reset a window's title bar. One instance is created per
/// styled window in applyMacOSStyling() and lives for the app's lifetime.
///
/// Neither Qt pointer is owned: the QQuickWindow and its NSWindow both outlive
/// this helper in a single-window app (closing the window ends the app), so
/// plain non-owning pointers are correct and keep this code independent of
/// whether the build enables ARC.
@interface RawformWindowStyler : NSObject {
    QQuickWindow* _quickWindow;  // not owned
    NSWindow* _nsWindow;         // not owned
    NSRect _windowedFrame;       // frame captured just before entering fullscreen
    BOOL _haveWindowedFrame;     // whether _windowedFrame is currently valid
}
- (instancetype)initWithQuickWindow:(QQuickWindow*)quickWindow
                           nsWindow:(NSWindow*)nsWindow;
- (void)applyTitleBarStyling;
- (void)publishDoubleClickAction;
@end

/// The global-domain defaults key behind "Double-click a window's title bar
/// to". Stored values: absent (the default, zoom), "Maximize" (zoom), "Fill"
/// (newer systems' fill-the-screen variant), "Minimize", or "None".
static NSString* const kDoubleClickActionKey = @"AppleActionOnDoubleClick";

@implementation RawformWindowStyler

- (instancetype)initWithQuickWindow:(QQuickWindow*)quickWindow
                           nsWindow:(NSWindow*)nsWindow {
    if ((self = [super init])) {
        _quickWindow = quickWindow;
        _nsWindow = nsWindow;
        _haveWindowedFrame = NO;

        // NSNotificationCenter does not retain its observers, so this object
        // must be kept alive by the caller (see applyMacOSStyling). We register
        // for the transitions that clobber our styling (or our geometry) and
        // react to each.
        NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
        [center addObserver:self
                   selector:@selector(onWillEnterFullScreen:)
                       name:NSWindowWillEnterFullScreenNotification
                     object:nsWindow];
        [center addObserver:self
                   selector:@selector(onEnterFullScreen:)
                       name:NSWindowDidEnterFullScreenNotification
                     object:nsWindow];
        [center addObserver:self
                   selector:@selector(onExitFullScreen:)
                       name:NSWindowDidExitFullScreenNotification
                     object:nsWindow];
        [center addObserver:self
                   selector:@selector(onDeminiaturize:)
                       name:NSWindowDidDeminiaturizeNotification
                     object:nsWindow];

        // The double-click preference lives in the global defaults domain,
        // which standardUserDefaults searches, and NSUserDefaults is
        // KVO-compliant for defaults keys, including changes made by another
        // process (System Settings). Never removed: this object lives for the
        // app's lifetime (see applyMacOSStyling).
        [[NSUserDefaults standardUserDefaults] addObserver:self
                                                forKeyPath:kDoubleClickActionKey
                                                   options:0
                                                   context:nil];
    }
    return self;
}

- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary*)change
                       context:(void*)context {
    (void)object;
    (void)change;
    (void)context;
    if ([keyPath isEqualToString:kDoubleClickActionKey])
        [self publishDoubleClickAction];
}

- (void)publishDoubleClickAction {
    if (!_quickWindow)
        return;
    // Collapse the stored values to the three actions QML distinguishes.
    // "Fill" is treated as zoom: Qt's showMaximized is the zoom operation, and
    // the difference between zoom and fill is not worth a fourth branch.
    NSString* stored = [[NSUserDefaults standardUserDefaults]
                        stringForKey:kDoubleClickActionKey];
    NSString* action = @"Maximize";
    if ([stored isEqualToString:@"Minimize"])
        action = @"Minimize";
    else if ([stored isEqualToString:@"None"])
        action = @"None";
    _quickWindow->setProperty("macTitleBarDoubleClickAction",
                              QString::fromNSString(action));
}

- (void)onWillEnterFullScreen:(NSNotification*)notification {
    (void)notification;
    // Capture the true windowed frame BEFORE the transition, while the window is
    // still windowed. We stamp this exact frame back on exit so the window
    // cannot drift shorter on each fullscreen round trip.
    _windowedFrame = [_nsWindow frame];
    _haveWindowedFrame = YES;
}

- (void)onEnterFullScreen:(NSNotification*)notification {
    (void)notification;
    // No title bar exists in fullscreen, so drop the QML content offset; leaving
    // it at the windowed height would shift the content up by a title-bar strip
    // and leave a matching gap at the bottom of the screen.
    if (_quickWindow)
        _quickWindow->setProperty("nativeTitleBarHeight", 0);
}

- (void)onExitFullScreen:(NSNotification*)notification {
    (void)notification;
    // Defer one runloop pass: Qt runs its own post-fullscreen geometry update on
    // this same notification, and doing our work inline races it (the race that
    // would shrink the window on each cycle). On the next pass the style mask
    // and frame have settled, so reapplyAfterFullScreenExit can re-style and
    // restore the captured windowed frame cleanly.
    [self performSelectorOnMainThread:@selector(reapplyAfterFullScreenExit)
                           withObject:nil
                        waitUntilDone:NO];
}

- (void)onDeminiaturize:(NSNotification*)notification {
    (void)notification;
    [self applyTitleBarStyling];
}

- (void)reapplyAfterFullScreenExit {
    [self applyTitleBarStyling];
    if (_haveWindowedFrame) {
        [_nsWindow setFrame:_windowedFrame display:YES];
        _haveWindowedFrame = NO;
    }
}

- (void)applyTitleBarStyling {
    if (!_nsWindow)
        return;

    // Only call setStyleMask when our bits are actually missing. Re-setting the
    // mask when it is already correct (for instance after deminiaturize, which
    // never strips it) is needless churn that can itself nudge the frame; the
    // fullscreen-exit path is the one case where Qt drops FullSizeContentView
    // and we must put it back (and there reapplyAfterFullScreenExit also
    // restores the frame). ORing preserves the resizable/closable/
    // miniaturizable bits Qt already set.
    NSWindowStyleMask current = [_nsWindow styleMask];
    NSWindowStyleMask wanted = current | NSWindowStyleMaskTitled
                                       | NSWindowStyleMaskFullSizeContentView;
    if (current != wanted)
        [_nsWindow setStyleMask:wanted];

    // Idempotent and cheap, so these run unconditionally.
    [_nsWindow setTitlebarAppearsTransparent:YES];
    [_nsWindow setTitleVisibility:NSWindowTitleHidden];
    [_nsWindow setMovable:NO];

    // Measure the native title bar height from the traffic-light buttons'
    // superview (the NSTitlebarContainerView) and expose it to QML as the
    // nativeTitleBarHeight property.
    //
    // This is measured via the close button's superview rather than computing
    // (frame.size.height - contentLayoutRect.size.height). The subtraction
    // approach depends on the window being fully laid out and produces wrong
    // values at certain lifecycle points, causing visible UI jumping. The close
    // button's superview is always correctly sized immediately after
    // setStyleMask:.
    NSButton* closeButton = [_nsWindow standardWindowButton:NSWindowCloseButton];
    if (closeButton && closeButton.superview && _quickWindow) {
        NSRect frame = closeButton.superview.frame;
        int titlebarHeight = static_cast<int>(frame.size.height);
        _quickWindow->setProperty("nativeTitleBarHeight", titlebarHeight);
    }
}

@end

void applyMacOSStyling(QQmlApplicationEngine* engine) {
    if (engine->rootObjects().isEmpty())
        return;

    // rawform's root QML object is the ApplicationWindow in MainWindow.qml,
    // which is backed by a QQuickWindow. Anything else means the QML failed to
    // load as expected, so bail rather than guess.
    auto* rootObject = qobject_cast<QQuickWindow*>(engine->rootObjects().first());
    if (!rootObject) {
        qWarning().noquote() << "applyMacOSStyling: root object is not a QQuickWindow";
        return;
    }

    // On macOS, winId() returns a pointer-sized integer whose value is the
    // address of the content NSView backing the QQuickWindow.
    auto nsView = reinterpret_cast<NSView*>(rootObject->winId());
    if (!nsView) {
        qWarning().noquote() << "applyMacOSStyling: could not obtain NSView";
        return;
    }

    // Walk from the NSView to the owning NSWindow.
    NSWindow* nsWindow = [nsView window];
    if (!nsWindow) {
        qWarning().noquote() << "applyMacOSStyling: NSWindow is nil";
        return;
    }

    // Create the observer, apply the styling once now, and keep the observer
    // alive for the app's lifetime so it can re-apply on later transitions. The
    // static strong reference is what keeps it alive: NSNotificationCenter does
    // not retain observers, so without this the object would be reclaimed and
    // the fullscreen-exit re-styling would silently stop working. This is a
    // deliberate app-lifetime singleton, not a leak.
    static RawformWindowStyler* s_styler = nil;
    s_styler = [[RawformWindowStyler alloc] initWithQuickWindow:rootObject
                                                       nsWindow:nsWindow];
    [s_styler applyTitleBarStyling];
    [s_styler publishDoubleClickAction];

    qDebug().noquote() << "applyMacOSStyling: styling applied and observer installed";
}

#else

/// No-op fallback for non-macOS platforms.
///
/// The build normally excludes this translation unit off macOS (see
/// CMakeLists.txt), and main.cpp only calls applyMacOSStyling under Q_OS_MAC, so
/// this branch is a safety net rather than a code path the app depends on.
void applyMacOSStyling(QQmlApplicationEngine* /*engine*/) {}

#endif
