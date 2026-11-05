// Platform probes and hooks (macOS implementation in platform/mac; portable
// fallbacks elsewhere). Counter names are reported verbatim in benchmarks.
#pragma once

#include <QString>
#include <functional>

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

} // namespace folia::platform
