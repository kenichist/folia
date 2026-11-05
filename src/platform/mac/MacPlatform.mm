// macOS platform probes: Mach task counters, thermal state, accessibility
// preferences, and dispatch memory-pressure notifications.
#include "app/Platform.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#include <QMetaObject>
#include <QCoreApplication>
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

} // namespace folia::platform
