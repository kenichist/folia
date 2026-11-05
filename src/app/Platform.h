// Platform probes and hooks (macOS implementation in platform/mac; portable
// fallbacks elsewhere). Counter names are reported verbatim in benchmarks.
#pragma once

#include <QColor>
#include <QRect>
#include <QString>
#include <QVector>
#include <functional>

class QWidget;

namespace folia::platform {

struct ProcessSample {
    qint64 footprintBytes = 0;      // macOS: task_vm_info.phys_footprint; Linux: RSS; Windows: PrivateUsage
    qint64 peakFootprintBytes = 0;  // macOS: ledger_phys_footprint_peak
    qint64 residentBytes = 0;       // resident_size (shared pages included)
    double userSeconds = 0;
    double systemSeconds = 0;
    qint64 interruptWakeups = 0;    // macOS: task_power_info.task_interrupt_wakeups
    qint64 idleWakeups = 0;         // macOS: task_power_info.task_platform_idle_wakeups
    QString counterName;            // which OS counter footprintBytes is
};

ProcessSample sampleSelf();
QString osDescription();
QString thermalState();             // "nominal", "fair", "serious", "critical", or "unknown"
QString powerSource();
bool reducedMotion();
bool reducedTransparency();
// Called on the main thread when the OS reports memory pressure.
void onMemoryPressure(std::function<void(bool critical)> cb);

// Native window chrome. On macOS 26+ "glass" draws Liquid Glass panels
// (NSGlassEffectView) *behind* the window's translucent Qt content, so Qt
// still paints every control on top. Elsewhere glassSupported() is false.
struct GlassPanel {
    QRect rect;          // in the window widget's coordinates
    qreal radius = 0;    // corner radius
    QColor tint;         // invalid: untinted
    bool clear = false;  // the more transparent "clear" glass
    bool operator==(const GlassPanel &o) const
    {
        return rect == o.rect && radius == o.radius && tint == o.tint && clear == o.clear;
    }
};
struct WindowChrome {
    bool glass = false;              // translucent window: backdrop + panels drawn behind Qt
    bool transparentTitlebar = false; // content extends under a toolbar-height title bar
    QColor backdrop;                 // window color behind the glass
    QVector<GlassPanel> panels;      // back to front
    bool operator==(const WindowChrome &o) const
    {
        return glass == o.glass && transparentTitlebar == o.transparentTitlebar && backdrop == o.backdrop
            && panels == o.panels;
    }
};
bool glassSupported();
// Applies `chrome` to a top-level widget once its native window exists
// (a no-op before that, for non-native platforms and outside macOS).
void applyWindowChrome(QWidget *window, const WindowChrome &chrome);
// Double-click on a transparent title bar: zoom, minimize or nothing, per
// the user's system setting.
void titlebarDoubleClicked(QWidget *window);

} // namespace folia::platform
