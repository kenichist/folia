// Portable fallbacks for non-macOS targets (Linux / Windows).
#include "app/Platform.h"

#ifndef Q_OS_MACOS
#include <QFile>
#include <QSysInfo>
#include <cstring>
#if defined(Q_OS_WIN)
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace folia::platform {

ProcessSample sampleSelf()
{
    ProcessSample s;
#if defined(Q_OS_WIN)
    s.counterName = QStringLiteral("PROCESS_MEMORY_COUNTERS_EX.PrivateUsage");
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof pmc)) {
        s.footprintBytes = qint64(pmc.PrivateUsage);
        s.peakFootprintBytes = qint64(pmc.PeakPagefileUsage);
        s.residentBytes = qint64(pmc.WorkingSetSize);
    }
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        auto toS = [](FILETIME f) { return double((qint64(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7; };
        s.userSeconds = toS(u);
        s.systemSeconds = toS(k);
    }
#else
    s.counterName = QStringLiteral("/proc/self/status VmRSS (RssAnon+RssFile+RssShmem)");
    QFile f(QStringLiteral("/proc/self/status"));
    if (f.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : f.readAll().split('\n')) {
            auto val = [&](const char *k) { return line.mid(int(strlen(k))).trimmed().split(' ').value(0).toLongLong() * 1024; };
            if (line.startsWith("VmRSS:")) { s.footprintBytes = val("VmRSS:"); s.residentBytes = s.footprintBytes; }
            else if (line.startsWith("VmHWM:")) s.peakFootprintBytes = val("VmHWM:");
        }
    }
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        s.userSeconds = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6;
        s.systemSeconds = ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
        s.interruptWakeups = ru.ru_nvcsw + ru.ru_nivcsw; // context switches (proxy, labelled)
    }
#endif
    return s;
}

QString osDescription() { return QSysInfo::prettyProductName(); }
QString thermalState() { return QStringLiteral("unknown"); }
QString powerSource() { return QStringLiteral("unknown"); }
bool reducedMotion() { return false; }
bool reducedTransparency() { return false; }
void onMemoryPressure(std::function<void(bool)>) {}

} // namespace folia::platform
#endif
