// macOS platform probes: Mach task counters, thermal state, accessibility
// preferences, dispatch memory-pressure notifications, and the native
// window chrome (Liquid Glass panels, transparent title bar).
#include "app/Platform.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#include <QGuiApplication>
#include <QHash>
#include <QMetaObject>
#include <QCoreApplication>
#include <QWidget>
#include <QWindow>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach/mach_time.h>

namespace folia::platform {

ProcessSample sampleSelf()
{
    ProcessSample s;
    s.counterName = QStringLiteral("task_vm_info.phys_footprint");
    task_vm_info_data_t vm{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count) == KERN_SUCCESS) {
        s.footprintBytes = qint64(vm.phys_footprint);
        s.residentBytes = qint64(vm.resident_size);
        if (count >= TASK_VM_INFO_REV4_COUNT)
            s.peakFootprintBytes = qint64(vm.ledger_phys_footprint_peak);
    }
    task_power_info_data_t pw{};
    count = TASK_POWER_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_POWER_INFO, reinterpret_cast<task_info_t>(&pw), &count) == KERN_SUCCESS) {
        // total_user/total_system are Mach absolute-time units, not ns.
        static mach_timebase_info_data_t tb = [] {
            mach_timebase_info_data_t t{};
            mach_timebase_info(&t);
            return t;
        }();
        const double toNs = double(tb.numer) / double(tb.denom);
        s.userSeconds = double(pw.total_user) * toNs / 1e9;
        s.systemSeconds = double(pw.total_system) * toNs / 1e9;
        s.interruptWakeups = qint64(pw.task_interrupt_wakeups);
        s.idleWakeups = qint64(pw.task_platform_idle_wakeups);
    }
    return s;
}

QString osDescription()
{
    NSOperatingSystemVersion v = NSProcessInfo.processInfo.operatingSystemVersion;
    return QStringLiteral("macOS %1.%2.%3").arg(v.majorVersion).arg(v.minorVersion).arg(v.patchVersion);
}

QString thermalState()
{
    switch (NSProcessInfo.processInfo.thermalState) {
    case NSProcessInfoThermalStateNominal: return QStringLiteral("nominal");
    case NSProcessInfoThermalStateFair: return QStringLiteral("fair");
    case NSProcessInfoThermalStateSerious: return QStringLiteral("serious");
    case NSProcessInfoThermalStateCritical: return QStringLiteral("critical");
    }
    return QStringLiteral("unknown");
}

QString powerSource()
{
    return NSProcessInfo.processInfo.lowPowerModeEnabled ? QStringLiteral("low-power-mode")
                                                         : QStringLiteral("normal-power-mode");
}

bool reducedMotion()
{
    return NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion;
}

bool reducedTransparency()
{
    return NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceTransparency;
}

void onMemoryPressure(std::function<void(bool)> cb)
{
    static dispatch_source_t source = nullptr;
    if (source)
        return;
    source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
                                    DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL,
                                    dispatch_get_main_queue());
    auto *heap = new std::function<void(bool)>(std::move(cb));
    dispatch_source_set_event_handler(source, ^{
        const unsigned long level = dispatch_source_get_data(source);
        (*heap)((level & DISPATCH_MEMORYPRESSURE_CRITICAL) != 0);
    });
    dispatch_resume(source);
}

// ---------------------------------------------------------------- chrome --
// Qt's view must stay the window's content view (Qt sizes the window
// through it), so the backdrop and the glass live in the window's frame
// view, ordered *below* the content view: Qt paints the controls on top,
// and the glass shows wherever the translucent Qt window paints nothing.
// (This file is compiled without ARC: views are retained/released by hand.)
namespace {

struct Chrome {
    NSWindow *window = nil;        // not retained; compared by identity
    NSView *backdrop = nil;        // retained
    QVector<NSView *> panels;      // retained, back to front
    QVector<GlassPanel> applied;   // what each panel currently shows
    NSToolbar *toolbar = nil;      // retained
    bool glass = false;
    bool layerWasOpaque = false;
};

QHash<QWidget *, Chrome> &chromes()
{
    static QHash<QWidget *, Chrome> all;
    return all;
}

void dropViews(Chrome &c)
{
    if (c.backdrop) {
        [c.backdrop removeFromSuperview];
        [c.backdrop release];
        c.backdrop = nil;
    }
    for (NSView *v : std::as_const(c.panels)) {
        [v removeFromSuperview];
        [v release];
    }
    c.panels.clear();
    c.applied.clear();
}

NSColor *toNSColor(const QColor &color)
{
    const QColor c = color.toRgb();
    return [NSColor colorWithSRGBRed:c.redF() green:c.greenF() blue:c.blueF() alpha:c.alphaF()];
}

// Qt's NSView for a top-level widget whose native window exists, or nil.
NSView *contentViewOf(QWidget *w)
{
    if (!w || QGuiApplication::platformName() != QLatin1String("cocoa"))
        return nil;
    QWindow *wh = w->windowHandle();
    if (!wh || !wh->handle())
        return nil;
    NSView *view = reinterpret_cast<NSView *>(wh->winId());
    if (!view.window || view.window.contentView != view)
        return nil;
    return view;
}

bool directlyAbove(NSView *frame, NSView *below, NSView *v)
{
    NSArray<NSView *> *subs = frame.subviews;
    const NSUInteger i = [subs indexOfObjectIdenticalTo:below];
    return i != NSNotFound && i + 1 < subs.count && subs[i + 1] == v;
}

} // namespace

bool glassSupported()
{
    if (@available(macOS 26.0, *))
        return true;
    return false;
}

void applyWindowChrome(QWidget *w, const WindowChrome &ch)
{
    NSView *qv = contentViewOf(w);
    if (!qv)
        return;
    @autoreleasepool {
        auto &all = chromes();
        auto it = all.find(w);
        if (it == all.end()) {
            if (!ch.glass && !ch.transparentTitlebar)
                return; // never styled: leave the window as Qt made it
            it = all.insert(w, Chrome{});
            QObject::connect(w, &QObject::destroyed, [w] {
                auto &m = chromes();
                auto found = m.find(w);
                if (found == m.end())
                    return;
                dropViews(*found);
                [found->toolbar release];
                m.erase(found);
            });
        }
        Chrome &c = *it;
        NSWindow *nw = qv.window;
        NSView *frame = qv.superview;
        if (c.window != nw) { // first use, or Qt recreated the native window
            dropViews(c);
            c.window = nw;
            c.glass = false;
            c.layerWasOpaque = qv.layer.opaque;
        }
        [CATransaction begin];
        [CATransaction setDisableActions:YES];

        // Translucency. Qt reports the window's opacity from its surface
        // format (the caller sets it); keep AppKit and the layer in step.
        const bool glass = ch.glass && glassSupported();
        if (glass != c.glass) {
            nw.opaque = !glass;
            nw.backgroundColor = glass ? NSColor.clearColor : NSColor.windowBackgroundColor;
            qv.layer.opaque = glass ? NO : c.layerWasOpaque;
            [nw invalidateShadow];
            c.glass = glass;
        }

        if (glass) {
            if (!c.backdrop) {
                c.backdrop = [[NSView alloc] initWithFrame:frame.bounds];
                c.backdrop.wantsLayer = YES;
                c.backdrop.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            }
            if (c.backdrop.superview != frame) {
                [c.backdrop removeFromSuperview];
                [frame addSubview:c.backdrop positioned:NSWindowBelow relativeTo:qv];
            }
            c.backdrop.frame = frame.bounds;
            c.backdrop.layer.backgroundColor =
                (ch.backdrop.isValid() ? toNSColor(ch.backdrop) : NSColor.windowBackgroundColor).CGColor;
            if (@available(macOS 26.0, *)) {
                while (c.panels.size() > ch.panels.size()) {
                    NSView *v = c.panels.takeLast();
                    c.applied.removeLast();
                    [v removeFromSuperview];
                    [v release];
                }
                NSView *below = c.backdrop;
                for (qsizetype i = 0; i < ch.panels.size(); ++i) {
                    const GlassPanel &p = ch.panels[i];
                    if (i == c.panels.size()) {
                        auto *made = [[NSGlassEffectView alloc] initWithFrame:NSZeroRect];
                        // Qt lays out from the top-left; keep that corner fixed
                        // while a live resize runs ahead of Qt's relayout.
                        made.autoresizingMask = NSViewMinYMargin | NSViewMaxXMargin;
                        c.panels.push_back(made);
                        c.applied.push_back(GlassPanel{QRect(), -1, QColor(), false});
                    }
                    auto *g = static_cast<NSGlassEffectView *>(c.panels[i]);
                    if (g.superview != frame || !directlyAbove(frame, below, g)) {
                        [g removeFromSuperview];
                        [frame addSubview:g positioned:NSWindowAbove relativeTo:below];
                    }
                    // Qt's view is flipped like Qt; the frame view is not.
                    const NSRect r = [qv convertRect:NSMakeRect(p.rect.x(), p.rect.y(), p.rect.width(), p.rect.height())
                                              toView:frame];
                    if (!NSEqualRects(g.frame, r))
                        g.frame = r;
                    GlassPanel &was = c.applied[i];
                    if (was.radius != p.radius)
                        g.cornerRadius = p.radius;
                    if (was.clear != p.clear || was.radius < 0)
                        g.style = p.clear ? NSGlassEffectViewStyleClear : NSGlassEffectViewStyleRegular;
                    if (was.tint != p.tint || was.radius < 0)
                        g.tintColor = p.tint.isValid() ? toNSColor(p.tint) : nil;
                    was = p;
                    below = g;
                }
            }
        } else {
            dropViews(c);
        }

        // A toolbar-height title bar with no title: the window buttons sit
        // inside the layout (Qt::ExpandedClientAreaHint lets Qt draw there;
        // the caller sets it). The empty unified toolbar only sets the height.
        if (ch.transparentTitlebar) {
            if (!c.toolbar) {
                c.toolbar = [[NSToolbar alloc] initWithIdentifier:@"org.folia.titlebar"];
                c.toolbar.allowsUserCustomization = NO;
            }
            if (nw.toolbar != c.toolbar)
                nw.toolbar = c.toolbar;
            nw.toolbarStyle = NSWindowToolbarStyleUnified;
            nw.titleVisibility = NSWindowTitleHidden;
            nw.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
        } else if (c.toolbar && nw.toolbar == c.toolbar) {
            nw.toolbar = nil;
            nw.titleVisibility = NSWindowTitleVisible;
            nw.titlebarSeparatorStyle = NSTitlebarSeparatorStyleAutomatic;
        }
        [CATransaction commit];
    }
}

void titlebarDoubleClicked(QWidget *w)
{
    NSView *qv = contentViewOf(w);
    if (!qv)
        return;
    NSString *action = [NSUserDefaults.standardUserDefaults stringForKey:@"AppleActionOnDoubleClick"];
    if ([action isEqualToString:@"Minimize"])
        [qv.window performMiniaturize:nil];
    else if (![action isEqualToString:@"None"])
        [qv.window performZoom:nil];
}

} // namespace folia::platform
