// Stage-0 baseline: an empty Qt Widgets window with nothing else loaded.
// Prints the same process counters the app's bench mode reports, so stage
// costs can be compared as deltas on the same OS counter.
#include "app/Platform.h"

#include <QApplication>
#include <QMainWindow>
#include <QTimer>
#include <cstdio>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QMainWindow w;
    w.resize(1180, 780);
    w.show();
    const int settleMs = argc > 1 ? atoi(argv[1]) : 5000;
    QTimer::singleShot(settleMs, [&] {
        const auto s = folia::platform::sampleSelf();
        printf("{\"stage\":\"empty_window\",\"counter\":\"%s\",\"footprint_bytes\":%lld,"
               "\"peak_footprint_bytes\":%lld,\"resident_bytes\":%lld}\n",
               qPrintable(s.counterName), s.footprintBytes, s.peakFootprintBytes, s.residentBytes);
        fflush(stdout);
        app.quit();
    });
    return app.exec();
}
