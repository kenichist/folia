#include "app/AppController.h"
#include "app/BenchDriver.h"
#include "core/Instrument.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QMainWindow>
#include <QMessageBox>
#include <QTimer>

#ifdef Q_OS_MACOS
#include <mach-o/dyld.h>

#include <climits>
#include <cstdlib>
#include <unistd.h>
#endif

namespace {

#ifdef Q_OS_MACOS
// Run without malloc's large-block cache (docs/BENCHMARKS.md §6b). macOS keeps
// freed blocks above ~32 KB dirty in that cache (up to ~520 MB on an 8 GB
// Mac), counted in the footprint, and a purgeable block that CoreGraphics'
// image cache gets from it can never become volatile. Imports and 600 dpi
// scans leave 30-50 MB there. libmalloc reads MallocLargeCache once, at
// process start, so when it is unset: set it and exec this binary again (same
// pid). That covers every launch (Finder, Dock, a terminal, the benches);
// Info.plist's LSEnvironment would cover only LaunchServices launches.
// MallocLargeCache=1 opts out; setting it (0 or 1) also skips the exec, which
// lldb otherwise stops at.
void execWithoutLargeMallocCache(char **argv)
{
    if (getenv("MallocLargeCache"))
        return;
    char path[PATH_MAX];
    uint32_t size = sizeof path;
    if (_NSGetExecutablePath(path, &size) != 0 || setenv("MallocLargeCache", "0", 1) != 0)
        return;
    execv(path, argv);
    unsetenv("MallocLargeCache"); // exec failed: carry on with the default allocator
}
#endif

} // namespace

int main(int argc, char **argv)
{
#ifdef Q_OS_MACOS
    execWithoutLargeMallocCache(argv);
#endif
    folia::BenchDriver::markMainEntered();
    QApplication::setApplicationName(QStringLiteral("Folia"));
    QApplication::setOrganizationName(QStringLiteral("Folia"));
    QApplication::setApplicationVersion(QStringLiteral(FOLIA_VERSION));
    QApplication app(argc, argv);

    QCommandLineParser p;
    p.setApplicationDescription(QStringLiteral("Folia — a programmable, lightweight local notes app"));
    p.addHelpOption();
    p.addVersionOption();
    QCommandLineOption home(QStringLiteral("home"), QStringLiteral("Data folder (workspace + shell)."), QStringLiteral("dir"));
    QCommandLineOption ws(QStringLiteral("workspace"), QStringLiteral("Workspace database file."), QStringLiteral("file"));
    QCommandLineOption shell(QStringLiteral("shell"), QStringLiteral("Load this shell layout file."), QStringLiteral("file"));
    QCommandLineOption preset(QStringLiteral("preset"), QStringLiteral("Use a preset from the shell folder (default, keyboard, right-nav)."), QStringLiteral("name"));
    QCommandLineOption safe(QStringLiteral("safe-mode"), QStringLiteral("Start with the built-in default layout (also: hold Shift while launching)."));
    QCommandLineOption noMath(QStringLiteral("no-math"), QStringLiteral("Do not load the formula renderer."));
    QCommandLineOption fault(QStringLiteral("fault-vfs"), QStringLiteral("Route storage through the fault-injection VFS (testing)."));
    QCommandLineOption noWatch(QStringLiteral("no-watch"), QStringLiteral("Do not live-reload the shell file."));
    QCommandLineOption bench(QStringLiteral("bench"), QStringLiteral("Run a benchmark scenario and exit."), QStringLiteral("scenario"));
    QCommandLineOption benchOut(QStringLiteral("bench-out"), QStringLiteral("Benchmark JSON output file."), QStringLiteral("file"));
    QCommandLineOption benchArgs(QStringLiteral("bench-arg"), QStringLiteral("key=value for the benchmark (repeatable)."), QStringLiteral("kv"));
    p.addOptions({home, ws, shell, preset, safe, noMath, fault, noWatch, bench, benchOut, benchArgs});
    p.process(app);

    folia::AppController::Options o;
    o.homeDir = p.value(home);
    o.workspacePath = p.value(ws);
    o.shellPath = p.value(shell);
    o.preset = p.value(preset);
    o.safeMode = p.isSet(safe) || (QGuiApplication::queryKeyboardModifiers() & Qt::ShiftModifier);
    o.noMath = p.isSet(noMath);
    o.faultVfs = p.isSet(fault);
    o.watchShell = !p.isSet(noWatch);
    const bool benchMode = p.isSet(bench);
    QVariantMap benchArgMap;
    for (const QString &kv : p.values(benchArgs)) {
        const int eq = kv.indexOf(QLatin1Char('='));
        if (eq > 0)
            benchArgMap.insert(kv.left(eq), kv.mid(eq + 1));
    }
    if (benchMode) {
        o.startPage = benchArgMap.value(QStringLiteral("start_page")).toLongLong();
        // media_budget_mib=N, media_cache_mib=N: run with another media
        // budget (cache + in-flight work) or cache cap (cached pixels).
        o.mediaBudgetBytes = benchArgMap.value(QStringLiteral("media_budget_mib")).toLongLong() << 20;
        o.mediaCacheBytes = benchArgMap.value(QStringLiteral("media_cache_mib")).toLongLong() << 20;
        o.seedIfEmpty = p.value(bench) == QLatin1String("snapshot");
        o.restoreLastPage = false;
        folia::instr::setSamplingEnabled(true);
    }

    folia::AppController ctl(o);
    QString err;
    if (!ctl.start(&err)) {
        if (!benchMode)
            QMessageBox::critical(nullptr, QStringLiteral("Folia"), err);
        fprintf(stderr, "folia: %s\n", qPrintable(err));
        return 2;
    }
    if (benchMode) {
        // Occluded windows get no paints on macOS; measurements need the
        // window exposed, so bench runs float above other windows.
        ctl.window()->setWindowFlag(Qt::WindowStaysOnTopHint, true);
        // Click-through, so someone working on the machine cannot perturb
        // a run by hovering or clicking the floating window. The driver's
        // synthetic events are delivered directly and are unaffected.
        ctl.window()->setWindowFlag(Qt::WindowTransparentForInput, true);
    }
    ctl.window()->show();

    std::unique_ptr<folia::BenchDriver> driver;
    if (benchMode) {
        driver = std::make_unique<folia::BenchDriver>(&ctl, p.value(bench), p.value(benchOut), benchArgMap);
        QTimer::singleShot(0, driver.get(), &folia::BenchDriver::run);
    }
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &ctl, [&ctl] { ctl.shutdown(false); });
    return app.exec();
}
