#include "app/BenchDriver.h"

#include "app/AppController.h"
#include "app/Platform.h"
#include "app/SettingsPage.h"
#include "core/Instrument.h"
#include "editor/EditorContext.h"
#include "editor/EditorPane.h"
#include "editor/NoteEditor.h"
#include "editor/PageSession.h"
#include "editor/PreviewService.h"
#include "editor/VirtualLayout.h"
#include "shell/DataContext.h"
#include "shell/ListSources.h"
#include "shell/ShellHost.h"
#include "storage/FaultVfs.h"
#include "storage/Store.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QInputMethodEvent>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QMainWindow>
#include <QMimeData>
#include <QPixmap>
#include <QScrollBar>
#include <QSysInfo>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>
#include <QWindow>

#include <algorithm>

#ifdef Q_OS_MACOS
#include <sys/sysctl.h>
#include <sys/time.h>
#endif
#ifdef Q_OS_WIN
#include <io.h>
#define fsync _commit
#else
#include <unistd.h>
#endif

namespace folia {

namespace {

QElapsedTimer g_mainClock;
double g_execToMainMs = -1;

double processAgeMs()
{
#ifdef Q_OS_MACOS
    kinfo_proc info{};
    size_t len = sizeof info;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    if (sysctl(mib, 4, &info, &len, nullptr, 0) == 0) {
        timeval now{};
        gettimeofday(&now, nullptr);
        const timeval st = info.kp_proc.p_starttime;
        return (now.tv_sec - st.tv_sec) * 1000.0 + (now.tv_usec - st.tv_usec) / 1000.0;
    }
#endif
    return -1;
}

const char *kTypingText = "The quick brown fox jumps over the lazy dog — naïve café, Ελληνικά, кириллица, 日本語 한국어 العربية 🙂👍🏽 ";

const QStringList &formulaCorpus()
{
    static const QStringList f = {
        QStringLiteral("\\frac{a+b}{c-d} = \\sqrt{x^2 + y^2}"),
        QStringLiteral("\\sum_{i=1}^{n} i^2 = \\frac{n(n+1)(2n+1)}{6}"),
        QStringLiteral("\\int_0^1 x^{k}\\,dx = \\frac{1}{k+1}"),
        QStringLiteral("e^{i\\theta} = \\cos\\theta + i\\sin\\theta"),
        QStringLiteral("\\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}^{-1} = \\frac{1}{ad-bc}\\begin{pmatrix} d & -b \\\\ -c & a \\end{pmatrix}"),
        QStringLiteral("\\lim_{x \\to 0} \\frac{\\sin x}{x} = 1"),
        QStringLiteral("\\begin{aligned} \\nabla \\cdot E &= \\frac{\\rho}{\\varepsilon_0} \\\\ \\nabla \\times B &= \\mu_0 J \\end{aligned}"),
        QStringLiteral("P(A \\mid B) = \\frac{P(B \\mid A)\\,P(A)}{P(B)}"),
    };
    return f;
}

} // namespace

void BenchDriver::markMainEntered()
{
    g_mainClock.start();
    g_execToMainMs = processAgeMs();
}

BenchDriver::BenchDriver(AppController *app, const QString &scenario, const QString &out, const QVariantMap &args)
    : QObject(app), m_app(app), m_scenario(scenario), m_out(out), m_args(args)
{
    const QString manifest = args.value(QStringLiteral("manifest")).toString();
    if (!manifest.isEmpty()) {
        QFile f(manifest);
        if (f.open(QIODevice::ReadOnly))
            m_manifest = QJsonDocument::fromJson(f.readAll()).object();
    }
    connect(app->editorPane()->editor(), &NoteEditor::painted, this, [this] { ++m_paints; });
    // Launch probe: the first editable paint of whatever page startup opens.
    connect(app, &AppController::pagePainted, this, [this](PageId id, qint64) {
        if (m_firstPaintProcMs >= 0)
            return;
        m_firstPaintProcMs = processAgeMs();
        m_firstPaintMainMs = g_mainClock.nsecsElapsed() / 1e6;
        m_firstPaintPage = id;
    });
}

// --------------------------------------------------------------- utils ----
bool BenchDriver::waitFor(const std::function<bool()> &pred, int timeoutMs)
{
    // Event-driven: re-check whenever the editor paints or the app signals
    // progress, with a coarse 10 ms fallback, so the harness itself does not
    // spin (its CPU would otherwise pollute per-action CPU measurements).
    QElapsedTimer t;
    t.start();
    while (!pred()) {
        const qint64 left = timeoutMs - t.elapsed();
        if (left <= 0)
            return false;
        QEventLoop loop;
        QTimer fallback;
        fallback.setSingleShot(true);
        connect(&fallback, &QTimer::timeout, &loop, &QEventLoop::quit);
        fallback.start(int(qMin<qint64>(left, 10)));
        auto c1 = connect(m_app->editorPane()->editor(), &NoteEditor::painted, &loop, &QEventLoop::quit, Qt::QueuedConnection);
        auto c2 = connect(m_app, &AppController::pagePainted, &loop, &QEventLoop::quit, Qt::QueuedConnection);
        auto c3 = connect(m_app->store(), &Store::committed, &loop, &QEventLoop::quit, Qt::QueuedConnection);
        loop.exec();
        disconnect(c1);
        disconnect(c2);
        disconnect(c3);
    }
    return true;
}

void BenchDriver::settle(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

void BenchDriver::add(const QString &metric, double v, const QString &unit, const QString &proxy)
{
    Metric &m = m_metrics[metric];
    m.unit = unit;
    if (!proxy.isEmpty())
        m.proxy = proxy;
    m.samples.push_back(v);
}

void BenchDriver::addInstrSamples()
{
    const auto samples = instr::takeSamples();
    for (auto it = samples.cbegin(); it != samples.cend(); ++it) {
        Metric &m = m_metrics[it.key()];
        m.unit = QStringLiteral("us");
        for (qint64 v : it.value())
            m.samples.push_back(double(v));
    }
}

PageId BenchDriver::manifestPage(const QString &key) const
{
    if (m_args.contains(key))
        return m_args.value(key).toLongLong();
    return m_manifest.value(key).toVariant().toLongLong();
}

QVector<PageId> BenchDriver::manifestList(const QString &key) const
{
    QVector<PageId> out;
    for (const QJsonValue &v : m_manifest.value(key).toArray())
        out << v.toVariant().toLongLong();
    return out;
}

bool BenchDriver::openAndWait(PageId id, double *openUs, bool *cached)
{
    bool painted = false, wasCached = false;
    qint64 us = -1;
    auto c1 = connect(m_app, &AppController::pagePainted, this, [&](PageId p, qint64 t) {
        if (p == id) {
            painted = true;
            us = t;
        }
    });
    auto c2 = connect(m_app, &AppController::pageActivated, this, [&](PageId p, qint64, bool c) {
        if (p == id)
            wasCached = c;
    });
    const bool alreadyActive = m_app->currentPage() == id;
    m_app->openPage(id, true);
    const bool ok = alreadyActive || waitFor([&] { return painted; }, 5000);
    disconnect(c1);
    disconnect(c2);
    if (openUs)
        *openUs = double(us);
    if (cached)
        *cached = wasCached;
    // Editable means: the editor shows this page's document and accepts input.
    NoteEditor *ed = m_app->editorPane()->editor();
    return ok && !ed->isReadOnly() && ed->session() && ed->session()->pageId() == id;
}

void BenchDriver::typeText(const QString &text, int count, bool ime)
{
    NoteEditor *ed = m_app->editorPane()->editor();
    ed->setFocus();
    int typed = 0;
    int i = 0;
    while (typed < count) {
        const int before = m_paints;
        if (ime && typed % 40 == 39) {
            // Compose "にほん" -> commit "日本" (3 preedit updates + commit).
            for (const QString &pre : {QStringLiteral("に"), QStringLiteral("にほ"), QStringLiteral("にほん")}) {
                QInputMethodEvent e(pre, {});
                QApplication::sendEvent(ed, &e);
                waitFor([&] { return m_paints > before; }, 40);
            }
            QInputMethodEvent commit;
            commit.setCommitString(QStringLiteral("日本"));
            QApplication::sendEvent(ed, &commit);
        } else if (typed % 61 == 60) {
            QKeyEvent p(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
            QApplication::sendEvent(ed, &p);
            QKeyEvent r(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
            QApplication::sendEvent(ed, &r);
        } else if (typed % 37 == 36) {
            QKeyEvent p(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
            QApplication::sendEvent(ed, &p);
            QKeyEvent r(QEvent::KeyRelease, Qt::Key_Backspace, Qt::NoModifier);
            QApplication::sendEvent(ed, &r);
        } else {
            const QChar ch = text[i % text.size()];
            ++i;
            QString s(ch);
            if (ch.isHighSurrogate() && i < text.size()) {
                s += text[i % text.size()];
                ++i;
            }
            const int key = ch.isLetter() && ch.unicode() < 128 ? ch.toUpper().unicode()
                            : ch == QLatin1Char(' ') ? Qt::Key_Space : Qt::Key_unknown;
            QKeyEvent p(QEvent::KeyPress, key, Qt::NoModifier, s);
            QApplication::sendEvent(ed, &p);
            QKeyEvent r(QEvent::KeyRelease, key, Qt::NoModifier, s);
            QApplication::sendEvent(ed, &r);
        }
        ++typed;
        // One edit per frame: wait for the repaint it caused (bounded).
        waitFor([&] { return m_paints > before; }, 50);
    }
}

QJsonObject BenchDriver::envJson() const
{
    QJsonObject e;
    e.insert(QStringLiteral("os"), platform::osDescription());
    e.insert(QStringLiteral("kernel"), QSysInfo::kernelVersion());
    e.insert(QStringLiteral("arch"), QSysInfo::currentCpuArchitecture());
    e.insert(QStringLiteral("qt"), QString::fromLatin1(qVersion()));
    e.insert(QStringLiteral("folia"), QStringLiteral(FOLIA_VERSION));
    e.insert(QStringLiteral("thermal_state"), platform::thermalState());
    e.insert(QStringLiteral("power_mode"), platform::powerSource());
    e.insert(QStringLiteral("time"), QDateTime::currentDateTime().toString(Qt::ISODate));
    e.insert(QStringLiteral("window_size"), QStringLiteral("%1x%2").arg(m_app->window()->width()).arg(m_app->window()->height()));
    e.insert(QStringLiteral("device_pixel_ratio"), m_app->window()->devicePixelRatioF());
    e.insert(QStringLiteral("preset"), m_app->shell()->activeName());
    e.insert(QStringLiteral("math_renderer"), m_app->previews() != nullptr);
    e.insert(QStringLiteral("fixture_seed"), m_manifest.value(QStringLiteral("seed")));
    e.insert(QStringLiteral("fixture_hash"), m_manifest.value(QStringLiteral("content_hash")));
    e.insert(QStringLiteral("args"), QJsonObject::fromVariantMap(m_args));
    e.insert(QStringLiteral("os_file_cache"), QStringLiteral("not purged (warm after first touch); purging needs root"));
    e.insert(QStringLiteral("input_path"), QStringLiteral("synthetic Qt events via QApplication::sendEvent (bypasses the OS HID/IME path)"));
    return e;
}

QJsonObject BenchDriver::processJson() const
{
    const platform::ProcessSample s = platform::sampleSelf();
    QJsonObject p;
    p.insert(QStringLiteral("footprint_counter"), s.counterName);
    p.insert(QStringLiteral("footprint_bytes"), double(s.footprintBytes));
    p.insert(QStringLiteral("peak_footprint_bytes"), double(s.peakFootprintBytes));
    p.insert(QStringLiteral("resident_bytes"), double(s.residentBytes));
    p.insert(QStringLiteral("user_s"), s.userSeconds);
    p.insert(QStringLiteral("system_s"), s.systemSeconds);
    p.insert(QStringLiteral("interrupt_wakeups"), double(s.interruptWakeups));
    p.insert(QStringLiteral("idle_wakeups"), double(s.idleWakeups));
    p.insert(QStringLiteral("lua_bytes"), double(m_app->shell()->luaMemory()));
    p.insert(QStringLiteral("doc_cache_bytes_est"), double(m_app->docCacheBytes()));
    p.insert(QStringLiteral("doc_cache_entries"), m_app->docCacheEntries());
    p.insert(QStringLiteral("preview_cache_bytes"), double(m_app->previews() ? m_app->previews()->cacheBytes() : 0));
    p.insert(QStringLiteral("threads"), s.threads);
    if (PageSession *a = m_app->activeSession())
        p.insert(QStringLiteral("laid_out_blocks"), a->layout()->laidOutCount());
    return p;
}

// ------------------------------------------------------------ dispatch ----
void BenchDriver::run()
{
    m_extra.insert(QStringLiteral("exec_to_main_ms"), g_execToMainMs);
    // Occluded windows get throttled paints on macOS: bring ours forward and
    // sample whether it stays exposed (reported as window_exposed_ratio).
    m_app->window()->raise();
    m_app->window()->activateWindow();
    auto *exposure = new QTimer(this);
    connect(exposure, &QTimer::timeout, this, [this] {
        ++m_exposureSamples;
        if (m_app->window()->windowHandle() && m_app->window()->windowHandle()->isExposed()
            && m_app->window()->isActiveWindow())
            ++m_exposedSamples;
    });
    exposure->start(250);
    // Let startup work (initial page open, tree load) finish first.
    waitFor([this] { return m_app->activeSession() != nullptr || m_paints > 0; }, 3000);
    settle(m_args.value(QStringLiteral("startup_settle_ms"), 300).toInt());
    const QString s = m_scenario;
    if (s == QLatin1String("snapshot")) scenarioSnapshot();
    else if (s == QLatin1String("launch")) scenarioLaunch();
    else if (s == QLatin1String("open")) scenarioOpen();
    else if (s == QLatin1String("create")) scenarioCreate();
    else if (s == QLatin1String("expand")) scenarioExpand();
    else if (s == QLatin1String("typing")) scenarioTyping();
    else if (s == QLatin1String("scroll")) scenarioScroll();
    else if (s == QLatin1String("stress")) scenarioStress();
    else if (s == QLatin1String("presentation")) scenarioPresentation();
    else if (s == QLatin1String("idle")) scenarioIdle();
    else if (s == QLatin1String("navigate")) scenarioNavigate();
    else if (s == QLatin1String("integration")) scenarioIntegration();
    else if (s == QLatin1String("settings")) scenarioSettings();
    else if (s == QLatin1String("images")) scenarioImages();
    else if (s == QLatin1String("recovery")) { scenarioRecovery(); return; }
    else m_notes.append(QStringLiteral("unknown scenario"));
    finish();
}

void BenchDriver::finish()
{
    QJsonObject r;
    r.insert(QStringLiteral("scenario"), m_scenario);
    r.insert(QStringLiteral("env"), envJson());
    QJsonObject metrics;
    for (auto it = m_metrics.cbegin(); it != m_metrics.cend(); ++it) {
        QJsonObject m;
        m.insert(QStringLiteral("unit"), it->unit);
        if (!it->proxy.isEmpty())
            m.insert(QStringLiteral("proxy"), it->proxy);
        QJsonArray a;
        for (double v : it->samples)
            a.append(v);
        m.insert(QStringLiteral("samples"), a);
        metrics.insert(it.key(), m);
    }
    r.insert(QStringLiteral("metrics"), metrics);
    QJsonObject counters = m_counters;
    for (int c = 0; c < instr::CounterCount; ++c)
        counters.insert(QString::fromLatin1(instr::counterName(instr::Counter(c))), double(instr::get(instr::Counter(c))));
    r.insert(QStringLiteral("counters"), counters);
    r.insert(QStringLiteral("process"), processJson());
    r.insert(QStringLiteral("notes"), m_notes);
    r.insert(QStringLiteral("window_exposed_and_active_ratio"),
             m_exposureSamples ? double(m_exposedSamples) / m_exposureSamples : -1.0);
    for (auto it = m_extra.constBegin(); it != m_extra.constEnd(); ++it)
        r.insert(it.key(), it.value());
    const QByteArray json = QJsonDocument(r).toJson();
    if (!m_out.isEmpty()) {
        QFile f(m_out);
        if (f.open(QIODevice::WriteOnly))
            f.write(json);
    } else {
        fprintf(stdout, "%s\n", json.constData());
        fflush(stdout);
    }
    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
}

// ----------------------------------------------------------- scenarios ----
void BenchDriver::scenarioSnapshot()
{
    const PageId page = manifestPage(QStringLiteral("page"));
    if (page)
        openAndWait(page, nullptr, nullptr);
    // mode=light|dark|system: capture with that Settings appearance override.
    const QString mode = m_args.value(QStringLiteral("mode")).toString();
    if (!mode.isEmpty()) {
        QVariantMap o = m_app->themeOverrides();
        o.insert(QStringLiteral("mode"), mode);
        m_app->setThemeOverrides(o);
    }
    settle(m_args.value(QStringLiteral("wait"), 600).toInt());
    const QString png = m_args.value(QStringLiteral("png"), QStringLiteral("snapshot.png")).toString();
    m_extra.insert(QStringLiteral("png"), png);
    m_extra.insert(QStringLiteral("saved"), m_app->window()->grab().save(png));
    // settings_png=<prefix>: also <prefix>-<n>.png of the window showing each Settings section.
    const QString settingsPng = m_args.value(QStringLiteral("settings_png")).toString();
    if (!settingsPng.isEmpty()) {
        m_app->showSettings();
        SettingsPage *w = m_app->settingsPage();
        settle(300);
        for (int i = 0; i < w->sectionCount(); ++i) {
            w->setSection(i);
            settle(150);
            m_app->window()->grab().save(QStringLiteral("%1-%2.png").arg(settingsPng).arg(i));
        }
        m_app->closeSettings();
    }
    m_extra.insert(QStringLiteral("shell_error"), m_app->shell()->lastError());
    m_extra.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(m_app->shell()->warnings()));
}

void BenchDriver::scenarioLaunch()
{
    // Process start -> first editable paint of the page startup opens (pass
    // --bench-arg start_page=<id> so it is the ordinary fixture page).
    waitFor([this] { return m_firstPaintProcMs >= 0; }, 10000);
    NoteEditor *ed = m_app->editorPane()->editor();
    const bool editable = !ed->isReadOnly() && ed->session() && ed->session()->pageId() == m_firstPaintPage;
    add(QStringLiteral("launch_process_to_editable_ms"), m_firstPaintProcMs, QStringLiteral("ms"),
        QStringLiteral("kernel process start (sysctl KERN_PROC p_starttime) to end of the first editor paint of the start page"));
    add(QStringLiteral("launch_main_to_editable_ms"), m_firstPaintMainMs, QStringLiteral("ms"),
        QStringLiteral("main() entry to end of the first editor paint of the start page"));
    m_extra.insert(QStringLiteral("editable"), editable);
    m_extra.insert(QStringLiteral("start_page"), double(m_firstPaintPage));
    settle(2000);
    const platform::ProcessSample s = platform::sampleSelf();
    add(QStringLiteral("footprint_after_launch_bytes"), double(s.footprintBytes), QStringLiteral("bytes"), s.counterName);
    addInstrSamples();
}

void BenchDriver::scenarioOpen()
{
    QVector<PageId> pages = manifestList(QStringLiteral("sample_pages"));
    const int n = qMin(m_args.value(QStringLiteral("n"), 120).toInt(), int(pages.size()));
    if (n < 2) {
        m_notes.append(QStringLiteral("manifest has no sample_pages"));
        return;
    }
    instr::takeSamples();
    PageId prev = 0;
    int failures = 0;
    for (int i = 0; i < n; ++i) {
        double us = 0;
        bool cached = false;
        if (!openAndWait(pages[i], &us, &cached))
            ++failures;
        add(cached ? QStringLiteral("open_cached_to_editable_ms") : QStringLiteral("open_uncached_to_editable_ms"),
            us / 1000.0, QStringLiteral("ms"),
            QStringLiteral("page.open dispatch to end of the first editor paint showing the page (app-internal; excludes compositor)"));
        if (prev) {
            if (!openAndWait(prev, &us, &cached))
                ++failures;
            add(cached ? QStringLiteral("open_cached_to_editable_ms") : QStringLiteral("open_uncached_to_editable_ms"),
                us / 1000.0, QStringLiteral("ms"));
        }
        prev = pages[i];
        settle(20);
    }
    m_counters.insert(QStringLiteral("open_failures"), failures);
    addInstrSamples();
}

void BenchDriver::scenarioCreate()
{
    const int n = m_args.value(QStringLiteral("n"), 120).toInt();
    Store *store = m_app->store();
    QHash<PageId, QElapsedTimer> pending;
    auto conn = connect(store, &Store::committed, this, [&](PageId p, Revision, qint64) {
        auto it = pending.find(p);
        if (it != pending.end()) {
            add(QStringLiteral("create_to_durable_commit_ms"), it->nsecsElapsed() / 1e6, QStringLiteral("ms"),
                QStringLiteral("page.new dispatch to Store::committed for the new page's first revision"));
            pending.erase(it);
        }
    });
    instr::takeSamples();
    for (int i = 0; i < n; ++i) {
        QElapsedTimer t;
        t.start();
        const int before = m_paints;
        const PageId id = m_app->newPage(0, -1, QString(), true);
        const double syncUs = t.nsecsElapsed() / 1000.0;
        const bool titleFocused = m_app->editorPane()->titleField()->hasFocus()
            || QApplication::focusWidget() == m_app->editorPane()->titleField();
        pending.insert(id, t);
        waitFor([&] { return m_paints > before; }, 200);
        add(QStringLiteral("create_to_editable_title_ms"), syncUs / 1000.0, QStringLiteral("ms"),
            QStringLiteral("page.new dispatch until the new page is active and its title field has focus (synchronous)"));
        add(QStringLiteral("create_to_first_paint_ms"), t.nsecsElapsed() / 1e6, QStringLiteral("ms"));
        if (!titleFocused)
            m_notes.append(QStringLiteral("title not focused after create #%1").arg(i));
        settle(30);
    }
    waitFor([&] { return pending.isEmpty(); }, 5000);
    disconnect(conn);
    addInstrSamples();
}

void BenchDriver::scenarioExpand()
{
    auto *list = qobject_cast<QListView *>(m_app->shell()->widgetById(QStringLiteral("outline")));
    TreeSource *tree = list ? dynamic_cast<TreeSource *>(list->model()) : nullptr;
    if (!tree) {
        m_notes.append(QStringLiteral("layout has no tree list with id 'outline'"));
        return;
    }
    waitFor([&] { return tree->rowCount() > 0; }, 3000);
    const QVector<PageId> targets = {manifestPage(QStringLiteral("broad_parent")), manifestPage(QStringLiteral("deep_root"))};
    const int n = m_args.value(QStringLiteral("n"), 100).toInt();
    qint64 lastMain = 0, lastTotal = 0;
    bool supplied = false;
    connect(tree, &TreeSource::firstRowsSupplied, this, [&](qint64 m, qint64 t) {
        lastMain = m;
        lastTotal = t;
        supplied = true;
    });
    // Presentation: first list paint after rows arrive.
    int listPaints = 0;
    QElapsedTimer listClock;
    listClock.start();
    qint64 lastListPaintNs = 0;
    struct PaintCounter : QObject {
        int *n;
        qint64 *lastNs;
        QElapsedTimer *clock;
        bool eventFilter(QObject *, QEvent *e) override
        {
            if (e->type() == QEvent::Paint) {
                ++*n;
                *lastNs = clock->nsecsElapsed();
            }
            return false;
        }
    } pc;
    pc.n = &listPaints;
    pc.lastNs = &lastListPaintNs;
    pc.clock = &listClock;
    list->viewport()->installEventFilter(&pc);
    for (PageId target : targets) {
        if (!target)
            continue;
        tree->reveal(target);
        waitFor([&] { return tree->rowOf(target) >= 0; }, 5000);
        for (int i = 0; i < n; ++i) {
            const int row = tree->rowOf(target);
            if (row < 0)
                break;
            tree->collapse(row);
            // Put the branch at the top so its new children land on screen
            // (rows inserted below the viewport legitimately cause no paint).
            list->scrollTo(tree->index(row), QAbstractItemView::PositionAtTop);
            settle(5);
            supplied = false;
            // Snapshot paints before expanding: the repaint showing the new
            // rows usually happens inside the same event-loop pass that
            // delivers them, before control returns here.
            const int before = listPaints;
            const qint64 t0 = listClock.nsecsElapsed();
            QElapsedTimer t;
            t.start();
            tree->expand(row);
            const double requestUs = t.nsecsElapsed() / 1000.0;
            waitFor([&] { return supplied; }, 3000);
            waitFor([&] { return listPaints > before; }, 1000);
            add(QStringLiteral("expand_main_thread_ms"), (requestUs + lastMain) / 1000.0, QStringLiteral("ms"),
                QStringLiteral("main-thread time: expand request + inserting the first page of rows (reader thread excluded)"));
            add(QStringLiteral("expand_rows_available_ms"), lastTotal / 1000.0, QStringLiteral("ms"));
            add(QStringLiteral("expand_to_list_paint_ms"),
                listPaints > before ? (lastListPaintNs - t0) / 1e6 : -1.0, QStringLiteral("ms"),
                QStringLiteral("expand request to the first sidebar list paint after it (-1 = no paint within 1 s)"));
        }
    }
    list->viewport()->removeEventFilter(&pc);
    m_extra.insert(QStringLiteral("list_viewport_paints_total"), listPaints);
    m_extra.insert(QStringLiteral("list_visible"), list->isVisible());
    m_extra.insert(QStringLiteral("list_viewport_size"), QStringLiteral("%1x%2").arg(list->viewport()->width()).arg(list->viewport()->height()));
    m_counters.insert(QStringLiteral("tree_rows_loaded"), tree->rowCount());
    addInstrSamples();
}

void BenchDriver::scenarioTyping()
{
    const QString which = m_args.value(QStringLiteral("doc"), QStringLiteral("ordinary")).toString();
    const PageId page = manifestPage(which + QStringLiteral("_page"));
    if (!page || !openAndWait(page, nullptr, nullptr)) {
        m_notes.append(QStringLiteral("cannot open %1 page").arg(which));
        return;
    }
    if (m_args.value(QStringLiteral("recover")).toBool()) {
        QString err;
        const bool loaded = m_app->shell()->loadSource("return { version = 1, view = function( end }", QStringLiteral("broken.lua"), &err);
        m_extra.insert(QStringLiteral("invalid_shell_rejected"), !loaded);
        m_extra.insert(QStringLiteral("shell_after_invalid"), m_app->shell()->activeName());
    }
    QTimer bg;
    int bgSerial = 0;
    if (m_args.value(QStringLiteral("bgrender")).toBool() && m_app->previews()) {
        connect(&bg, &QTimer::timeout, this, [&] {
            PreviewRequest r;
            r.block = (qint64(1) << 50) + bgSerial % 4;
            r.blockRev = quint64(++bgSerial);
            r.kind = PreviewKind::DisplayMath;
            r.source = formulaCorpus()[bgSerial % formulaCorpus().size()] + QStringLiteral(" + x_{%1}").arg(bgSerial);
            r.pointSize = 16;
            m_app->previews()->renderPreview(r, this, [](PreviewResult) {});
        });
        bg.start(8);
    }
    // Caret: middle of a long text line ~1/3 into the document (or code line).
    PageSession *s = m_app->activeSession();
    QTextDocument *doc = s->document();
    QTextBlock target = doc->findBlockByNumber(qMax(1, doc->blockCount() / 3));
    for (QTextBlock b = target; b.isValid(); b = b.next()) {
        const LineRole r = PageSession::roleOf(b);
        if ((which == QLatin1String("code") ? r == LineRole::CodeLine : r == LineRole::Para) && b.length() > 30) {
            target = b;
            break;
        }
    }
    NoteEditor *ed = m_app->editorPane()->editor();
    QTextCursor c(target);
    c.setPosition(target.position() + target.length() / 2);
    ed->setTextCursor(c);
    ed->ensureCursorVisible();
    settle(200);
    instr::takeSamples();
    const platform::ProcessSample before = platform::sampleSelf();
    const Revision revBefore = s->revision();
    const int n = m_args.value(QStringLiteral("n"), 1000).toInt();
    QElapsedTimer wall;
    wall.start();
    typeText(QString::fromUtf8(kTypingText), n, m_args.value(QStringLiteral("ime"), true).toBool());
    const double wallMs = wall.nsecsElapsed() / 1e6;
    const platform::ProcessSample after = platform::sampleSelf();
    bg.stop();
    // Correctness: every edit bumped the revision and gets durably committed.
    waitFor([&] { return s->committedRevision() >= s->revision() && !s->hasUnsnapshotted(); }, 4000);
    addInstrSamples();
    const double cpuMs = ((after.userSeconds + after.systemSeconds) - (before.userSeconds + before.systemSeconds)) * 1000;
    add(QStringLiteral("cpu_ms_per_edit_all_threads"), cpuMs / n, QStringLiteral("ms"),
        QStringLiteral("process user+system CPU over the typing run / edits (includes paint, save, previews)"));
    m_extra.insert(QStringLiteral("typing_wall_ms"), wallMs);
    m_extra.insert(QStringLiteral("edits"), n);
    m_extra.insert(QStringLiteral("revisions"), double(s->revision() - revBefore));
    m_extra.insert(QStringLiteral("committed_matches_revision"), s->committedRevision() == s->revision());
    m_extra.insert(QStringLiteral("idle_wakeups_delta"), double(after.idleWakeups - before.idleWakeups));
    m_metrics[QStringLiteral("input_to_paint_us")].proxy =
        QStringLiteral("event delivery to end of editor viewport paint (synthetic event; excludes compositor)");
    m_metrics[QStringLiteral("input_to_flush_us")].proxy =
        QStringLiteral("event delivery to the event-loop turn after the paint (backing store flushed); excludes WindowServer composition");
}

void BenchDriver::scenarioScroll()
{
    // Continuous scroll paced by the display (one step per presented frame).
    // Per frame we record main-thread work (scroll handling + editor paint)
    // and the interval between presented paints; a hitch is an interval
    // longer than 1.5 frames (> 25 ms at 60 Hz).
    const PageId page = manifestPage(QStringLiteral("stress_page"));
    if (!page || !openAndWait(page, nullptr, nullptr)) {
        m_notes.append(QStringLiteral("cannot open stress page"));
        return;
    }
    NoteEditor *ed = m_app->editorPane()->editor();
    QScrollBar *vb = ed->verticalScrollBar();
    vb->setValue(0);
    settle(300);
    instr::takeSamples();
    const int frames = m_args.value(QStringLiteral("frames"), 1200).toInt();
    const int step = m_args.value(QStringLiteral("step"), 40).toInt();
    QElapsedTimer clock;
    clock.start();
    qint64 lastPaintNs = -1;
    QVector<double> intervals;
    qint64 paintStartNs = 0;
    double lastPaintMs = 0;
    struct PaintTimer : QObject {
        qint64 *start;
        QElapsedTimer *clock;
        bool eventFilter(QObject *, QEvent *e) override
        {
            if (e->type() == QEvent::Paint)
                *start = clock->nsecsElapsed();
            return false;
        }
    } pt;
    pt.start = &paintStartNs;
    pt.clock = &clock;
    ed->viewport()->installEventFilter(&pt);
    auto conn = connect(ed, &NoteEditor::painted, this, [&] {
        const qint64 now = clock.nsecsElapsed();
        lastPaintMs = (now - paintStartNs) / 1e6;
        if (lastPaintNs >= 0)
            intervals << (now - lastPaintNs) / 1e6;
        lastPaintNs = now;
    });
    int workOver = 0, hitches = 0;
    double longestWork = 0;
    for (int i = 0; i < frames; ++i) {
        const int before = m_paints;
        const qint64 t0 = clock.nsecsElapsed();
        vb->setValue(vb->value() + step);
        if (vb->value() >= vb->maximum())
            vb->setValue(0);
        const double scrollMs = (clock.nsecsElapsed() - t0) / 1e6;
        waitFor([&] { return m_paints > before; }, 100);
        const double work = scrollMs + lastPaintMs;
        add(QStringLiteral("scroll_frame_work_ms"), work, QStringLiteral("ms"),
            QStringLiteral("main-thread scroll handling + editor paint for one frame (backing-store flush excluded)"));
        if (work > 1000.0 / 60.0)
            ++workOver;
        longestWork = qMax(longestWork, work);
    }
    disconnect(conn);
    ed->viewport()->removeEventFilter(&pt);
    double longestInterval = 0;
    for (double v : intervals) {
        add(QStringLiteral("scroll_paint_interval_ms"), v, QStringLiteral("ms"),
            QStringLiteral("interval between consecutive presented editor paints during continuous scrolling"));
        if (v > 25.0)
            ++hitches;
        longestInterval = qMax(longestInterval, v);
    }
    m_extra.insert(QStringLiteral("frames"), frames);
    m_extra.insert(QStringLiteral("frames_work_within_16_7ms_ratio"), 1.0 - double(workOver) / frames);
    m_extra.insert(QStringLiteral("longest_frame_work_ms"), longestWork);
    m_extra.insert(QStringLiteral("hitches_over_25ms"), hitches);
    m_extra.insert(QStringLiteral("longest_paint_interval_ms"), longestInterval);
    addInstrSamples();
}

void BenchDriver::scenarioStress()
{
    const PageId page = manifestPage(m_args.value(QStringLiteral("doc"), QStringLiteral("stress")).toString() + QStringLiteral("_page"));
    double openUs = 0;
    bool cached = false;
    const bool ok = page && openAndWait(page, &openUs, &cached);
    if (!ok) {
        m_notes.append(QStringLiteral("cannot open stress page"));
        return;
    }
    add(QStringLiteral("stress_first_editable_ms"), openUs / 1000.0, QStringLiteral("ms"));
    NoteEditor *ed = m_app->editorPane()->editor();
    settle(300);
    const int reps = m_args.value(QStringLiteral("n"), 20).toInt();
    for (int i = 0; i < reps; ++i) {
        // Jump to end / start (Mod+End / Mod+Home as the user would).
        for (bool end : {true, false}) {
            const int before = m_paints;
            QElapsedTimer t;
            t.start();
            QKeyEvent p(QEvent::KeyPress, end ? Qt::Key_End : Qt::Key_Home, Qt::ControlModifier);
            QApplication::sendEvent(ed, &p);
            waitFor([&] { return m_paints > before; }, 500);
            add(end ? QStringLiteral("jump_to_end_ms") : QStringLiteral("jump_to_start_ms"), t.nsecsElapsed() / 1e6, QStringLiteral("ms"));
        }
        // Select across off-screen blocks (whole document) and copy.
        QElapsedTimer t;
        t.start();
        const int before = m_paints;
        QTextCursor c(ed->document());
        c.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        ed->setTextCursor(c);
        waitFor([&] { return m_paints > before; }, 500);
        add(QStringLiteral("select_all_to_paint_ms"), t.nsecsElapsed() / 1e6, QStringLiteral("ms"));
        QElapsedTimer cp;
        cp.start();
        ed->copy();
        add(QStringLiteral("copy_selection_ms"), cp.nsecsElapsed() / 1e6, QStringLiteral("ms"));
        if (i == 0)
            m_extra.insert(QStringLiteral("copied_chars"), double(QApplication::clipboard()->text().size()));
        QTextCursor home(ed->document());
        ed->setTextCursor(home);
        settle(20);
    }
    const platform::ProcessSample s = platform::sampleSelf();
    m_extra.insert(QStringLiteral("doc_chars"), ed->document()->characterCount());
    m_extra.insert(QStringLiteral("doc_lines"), ed->document()->blockCount());
    m_extra.insert(QStringLiteral("peak_footprint_bytes"), double(s.peakFootprintBytes));
    addInstrSamples();
}

void BenchDriver::scenarioPresentation()
{
    const PageId page = manifestPage(QStringLiteral("ordinary_page"));
    if (!page || !openAndWait(page, nullptr, nullptr)) {
        m_notes.append(QStringLiteral("cannot open ordinary page"));
        return;
    }
    NoteEditor *ed = m_app->editorPane()->editor();
    PageSession *s = m_app->activeSession();
    const int n = m_args.value(QStringLiteral("n"), 100).toInt();
    auto insertAtEnd = [&](const QString &text) {
        QTextCursor c(s->document());
        c.movePosition(QTextCursor::End);
        c.insertText(QStringLiteral("\n") + text);
        ed->setTextCursor(c);
        ed->ensureCursorVisible();
    };
    // Code: insert a short visible block, time until it is painted highlighted.
    static const char *codeTpl =
        "```python\ndef f_%1(x):\n    \"\"\"doc %1\"\"\"\n    total = 0\n    for i in range(x):\n"
        "        total += i * %1  # sum\n    return total\n```";
    for (int i = 0; i < n; ++i) {
        const int before = m_paints;
        QElapsedTimer t;
        t.start();
        insertAtEnd(QString::fromLatin1(codeTpl).arg(i));
        waitFor([&] { return m_paints > before; }, 500);
        add(QStringLiteral("code_block_visible_ms"), t.nsecsElapsed() / 1e6, QStringLiteral("ms"),
            QStringLiteral("insert of a 7-line code block to end of the paint that shows it highlighted"));
        settle(10);
    }
    // Formulas: first render in process, then uncached distinct, then cache hits.
    auto formulaRound = [&](const QString &src, const QString &metric) {
        QElapsedTimer t;
        t.start();
        insertAtEnd(QStringLiteral("$$\n") + src + QStringLiteral("\n$$"));
        QTextBlock close = s->document()->lastBlock();
        bool shown = waitFor([&] {
            BlockData *d = PageSession::dataOf(close);
            return d && !d->previewPending && (d->preview || !d->previewError.isEmpty());
        }, 3000);
        const int before = m_paints;
        waitFor([&] { return m_paints > before; }, 200);
        add(metric, t.nsecsElapsed() / 1e6, QStringLiteral("ms"),
            QStringLiteral("insert of a display formula to the paint that shows its rendered preview"));
        if (!shown)
            m_notes.append(QStringLiteral("formula not shown: ") + src);
        BlockData *d = PageSession::dataOf(close);
        if (d && !d->previewError.isEmpty())
            m_notes.append(QStringLiteral("formula error: ") + d->previewError);
        settle(10);
    };
    if (m_app->previews()) {
        formulaRound(formulaCorpus()[0] + QStringLiteral(" + 0"), QStringLiteral("formula_first_in_process_ms"));
        for (int i = 1; i < n; ++i)
            formulaRound(formulaCorpus()[i % formulaCorpus().size()] + QStringLiteral(" + %1").arg(i),
                         QStringLiteral("formula_uncached_ms"));
        for (int i = 1; i < n; ++i)
            formulaRound(formulaCorpus()[i % formulaCorpus().size()] + QStringLiteral(" + %1").arg(i),
                         QStringLiteral("formula_cache_hit_ms"));
    } else {
        m_notes.append(QStringLiteral("math renderer disabled (--no-math)"));
    }
    addInstrSamples();
}

void BenchDriver::scenarioIdle()
{
    const PageId page = manifestPage(QStringLiteral("page"));
    if (page)
        openAndWait(page, nullptr, nullptr);
    settle(m_args.value(QStringLiteral("settle_ms"), 5000).toInt());
    const int seconds = m_args.value(QStringLiteral("seconds"), 60).toInt();
    const platform::ProcessSample first = platform::sampleSelf();
    QElapsedTimer t;
    t.start();
    platform::ProcessSample prev = first;
    for (int i = 0; i < seconds; ++i) {
        settle(1000);
        const platform::ProcessSample s = platform::sampleSelf();
        const double dt = 1.0;
        add(QStringLiteral("idle_cpu_percent_of_core"),
            ((s.userSeconds + s.systemSeconds) - (prev.userSeconds + prev.systemSeconds)) / dt * 100, QStringLiteral("ratio"));
        add(QStringLiteral("idle_footprint_bytes"), double(s.footprintBytes), QStringLiteral("bytes"), s.counterName);
        add(QStringLiteral("idle_wakeups_per_s"), double((s.idleWakeups + s.interruptWakeups) - (prev.idleWakeups + prev.interruptWakeups)), QStringLiteral("ratio"),
            QStringLiteral("task_power_info idle + interrupt wakeups per second (includes the 1 s sampling timer)"));
        prev = s;
    }
    const double wall = t.nsecsElapsed() / 1e9;
    m_extra.insert(QStringLiteral("idle_mean_cpu_percent"),
                   ((prev.userSeconds + prev.systemSeconds) - (first.userSeconds + first.systemSeconds)) / wall * 100);
    m_extra.insert(QStringLiteral("idle_seconds"), wall);
}

void BenchDriver::scenarioNavigate()
{
    const QVector<PageId> pages = manifestList(QStringLiteral("sample_pages"));
    if (pages.size() < 20) {
        m_notes.append(QStringLiteral("not enough sample_pages"));
        return;
    }
    const int batches = m_args.value(QStringLiteral("batches"), 5).toInt();
    const int per = m_args.value(QStringLiteral("per_batch"), 100).toInt();
    int k = 0;
    for (int b = 0; b < batches; ++b) {
        for (int i = 0; i < per; ++i) {
            double us = 0;
            bool cached = false;
            openAndWait(pages[k++ % pages.size()], &us, &cached);
            add(QStringLiteral("navigate_switch_ms"), us / 1000.0, QStringLiteral("ms"));
        }
        settle(2000);
        const platform::ProcessSample s = platform::sampleSelf();
        add(QStringLiteral("navigate_settled_footprint_bytes"), double(s.footprintBytes), QStringLiteral("bytes"), s.counterName);
        add(QStringLiteral("navigate_doc_cache_bytes_est"), double(m_app->docCacheBytes()), QStringLiteral("bytes"));
        add(QStringLiteral("navigate_doc_cache_entries"), m_app->docCacheEntries(), QStringLiteral("ratio"));
        add(QStringLiteral("navigate_preview_cache_bytes"),
            double(m_app->previews() ? m_app->previews()->cacheBytes() : 0), QStringLiteral("bytes"));
    }
    addInstrSamples();
}

void BenchDriver::scenarioSettings()
{
    // What the Settings page costs: opening it (showSettings -> its next
    // paint; the first open also creates it), applying a change (main-thread
    // work, and until the editor repaints), the footprint before, while open
    // and after closing (the page is deleted), and the lazy font lists.
    // window=0 / apply=0 isolate the two costs; modes=0 leaves out light/dark.
    const PageId page = manifestPage(QStringLiteral("page"));
    if (page)
        openAndWait(page, nullptr, nullptr);
    const int reps = qMax(2, m_args.value(QStringLiteral("reps"), 10).toInt());
    const bool useWindow = m_args.value(QStringLiteral("window"), 1).toInt() != 0;
    const bool doApply = m_args.value(QStringLiteral("apply"), 1).toInt() != 0;
    m_extra.insert(QStringLiteral("settings_window"), useWindow);
    m_extra.insert(QStringLiteral("settings_apply"), doApply);
    const QVariantMap original = m_app->themeOverrides();
    const qreal baseSize = m_app->editorContext()->theme().body.pointSizeF();
    auto footprint = [] { return double(platform::sampleSelf().footprintBytes); };
    const QString counter = platform::sampleSelf().counterName;
    settle(m_args.value(QStringLiteral("settle_ms"), 3000).toInt());
    add(QStringLiteral("settings_footprint_before_bytes"), footprint(), QStringLiteral("bytes"), counter);

    for (int i = 0; i < reps; ++i) {
        SettingsPage *w = nullptr;
        if (useWindow) {
            const int paintsBefore = 0; // a new page each open
            QElapsedTimer t;
            t.start();
            m_app->showSettings();
            w = m_app->settingsPage();
            const bool painted = waitFor([w, paintsBefore] { return w && w->paintCount() > paintsBefore; }, 5000);
            if (!painted)
                m_notes.append(QStringLiteral("settings window did not paint (rep %1)").arg(i));
            add(i == 0 ? QStringLiteral("settings_open_first_ms") : QStringLiteral("settings_open_ms"), t.nsecsElapsed() / 1e6,
                QStringLiteral("ms"), QStringLiteral("showSettings() to the end of the Settings page's first paint"));
            settle(500);
            add(QStringLiteral("settings_open_footprint_bytes"), footprint(), QStringLiteral("bytes"), counter);
        }
        // Three text-size changes, then a switch to dark and back to light
        // (after a light/dark switch macOS drops Qt's font engines, so the
        // first layout after it reloads them: measured separately).
        const int changes = !doApply ? 0 : m_args.value(QStringLiteral("modes"), 1).toInt() ? 5 : 3; // modes=0: sizes only
        for (int k = 0; k < changes; ++k) {
            const bool mode = k >= 3;
            QVariantMap o = original;
            if (mode) {
                o.insert(QStringLiteral("mode"), k == 3 ? QStringLiteral("dark") : QStringLiteral("light"));
            } else if (k < 2) {
                QVariantMap f = o.value(QStringLiteral("font")).toMap();
                f.insert(QStringLiteral("size"), baseSize + 1 + k);
                o.insert(QStringLiteral("font"), f);
            }
            // While the page covers the editor, the change shows as a page repaint.
            const int before = w ? w->paintCount() : m_paints;
            QElapsedTimer a;
            a.start();
            m_app->setThemeOverrides(o);
            const double mainUs = a.nsecsElapsed() / 1000.0;
            const bool repainted = waitFor([this, w, before] { return (w ? w->paintCount() : m_paints) > before; }, 2000);
            const QString kind = mode ? QStringLiteral("mode") : QStringLiteral("size");
            add(QStringLiteral("settings_apply_%1_main_us").arg(kind), mainUs, QStringLiteral("us"),
                QStringLiteral("synchronous work in setThemeOverrides (theme, palette, layout invalidation)"));
            if (repainted)
                add(QStringLiteral("settings_apply_%1_to_paint_ms").arg(kind), a.nsecsElapsed() / 1e6, QStringLiteral("ms"),
                    QStringLiteral("setThemeOverrides() to the next paint of the editor, or of the Settings page while it is open"));
            settle(150);
        }
        m_app->setThemeOverrides(original);
        settle(200);
        if (w) {
            m_app->closeSettings(); // back to the notes; the page is deleted
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); // nested bench loops defer deleteLater
        }
        settle(m_args.value(QStringLiteral("close_settle_ms"), 1000).toInt());
        add(QStringLiteral("settings_closed_footprint_bytes"), footprint(), QStringLiteral("bytes"), counter);
    }
    if (!useWindow) {
        addInstrSamples();
        return;
    }

    // The font lists are filled only when first opened; measure that once.
    m_app->showSettings();
    SettingsPage *w = m_app->settingsPage();
    settle(500);
    const double beforeFonts = footprint();
    QElapsedTimer f;
    f.start();
    w->populateFontLists();
    add(QStringLiteral("settings_font_lists_ms"), f.nsecsElapsed() / 1e6, QStringLiteral("ms"),
        QStringLiteral("enumerate text + monospaced families (first popup)"));
    settle(500);
    add(QStringLiteral("settings_font_lists_footprint_delta_bytes"), footprint() - beforeFonts, QStringLiteral("bytes"), counter);
    m_app->closeSettings();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    settle(500);
    addInstrSamples();
}

void BenchDriver::scenarioIntegration()
{
    // (a) edit during an outstanding save, (b) switch pages while a formula
    // renders, (c) hot-reload the layout during IME composition. Each check
    // reconciles what the user did with durable revisions, displayed preview,
    // selection and layout state. (d) close/reopen is driven externally.
    QJsonObject checks;
    const PageId p1 = manifestPage(QStringLiteral("ordinary_page"));
    const PageId p2 = manifestPage(QStringLiteral("math_page"));
    NoteEditor *ed = m_app->editorPane()->editor();
    if (p1 && openAndWait(p1, nullptr, nullptr)) {
        PageSession *s = m_app->activeSession();
        QTextCursor c(s->document());
        c.movePosition(QTextCursor::End);
        ed->setTextCursor(c);
        typeText(QStringLiteral("before save "), 20, false);
        m_app->flushSaves(); // save now in flight
        typeText(QStringLiteral("during save "), 20, false); // edits while it is outstanding
        const QString expected = s->document()->toPlainText();
        m_app->flushSaves();
        const bool durable = waitFor([&] { return s->committedRevision() == s->revision(); }, 4000);
        PageContent stored;
        bool got = false;
        QObject ctx;
        m_app->store()->openPage(p1, &ctx, [&](PageContent pc) { stored = pc; got = true; });
        waitFor([&] { return got; }, 3000);
        PageSession probe(p1, m_app->editorContext(), [this] { return m_app->store()->allocateBlockId(); });
        probe.load(stored);
        checks.insert(QStringLiteral("a_edit_during_save_durable"), durable);
        checks.insert(QStringLiteral("a_stored_text_matches_editor"), probe.document()->toPlainText() == expected);
        checks.insert(QStringLiteral("a_stored_rev_matches"), double(stored.rev) == double(s->revision()));
    }
    if (p1 && p2 && m_app->previews()) {
        // Insert a heavy formula, switch away before it can render, come back.
        openAndWait(p2, nullptr, nullptr);
        PageSession *ms = m_app->activeSession();
        const qint64 staleBefore = instr::get(instr::PreviewStaleDrops);
        QTextCursor c(ms->document());
        c.movePosition(QTextCursor::End);
        const QString heavy = QStringLiteral("\\begin{bmatrix}") + QString(QStringLiteral("a & b & c & d \\\\ ")).repeated(40)
            + QStringLiteral("\\end{bmatrix}");
        c.insertText(QStringLiteral("\n$$\n") + heavy + QStringLiteral("\n$$"));
        ed->setTextCursor(c);
        ed->ensureCursorVisible();
        openAndWait(p1, nullptr, nullptr); // switch while rendering
        // Edit the formula source behind the scenes: the old render must not win.
        QTextBlock last = ms->document()->lastBlock();
        QTextCursor e(last.previous());
        e.movePosition(QTextCursor::EndOfBlock);
        e.insertText(QStringLiteral(" + z"));
        openAndWait(p2, nullptr, nullptr);
        QTextBlock close = ms->document()->lastBlock();
        ed->setTextCursor(QTextCursor(close));
        ed->ensureCursorVisible();
        const bool shown = waitFor([&] {
            BlockData *d = PageSession::dataOf(close);
            return d && !d->previewPending && d->preview;
        }, 5000);
        BlockData *d = PageSession::dataOf(close);
        VirtualLayout *vl = ms->layout();
        const quint64 want = VirtualLayout::previewHash(vl->mathSource(close), vl->previewPointSize());
        checks.insert(QStringLiteral("b_preview_shown_after_switch"), shown);
        checks.insert(QStringLiteral("b_preview_is_current_source"), d && d->preview && d->previewHash == want);
        // (d) Rapid edits inside a visible formula: every keystroke supersedes
        // the previous render; only the newest source may end up displayed.
        const qint64 cancelledBefore = instr::get(instr::PreviewCancelled);
        QTextCursor fc(close.previous());
        fc.movePosition(QTextCursor::EndOfBlock);
        ed->setTextCursor(fc);
        for (int i = 0; i < 30; ++i) {
            QKeyEvent kp(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, QString(QChar(u'a' + i % 26)));
            QApplication::sendEvent(ed, &kp);
            ed->viewport()->repaint(); // force layout -> render request per keystroke
        }
        const bool settled = waitFor([&] {
            BlockData *x = PageSession::dataOf(close);
            return x && !x->previewPending && x->preview;
        }, 5000);
        settle(300);
        BlockData *x = PageSession::dataOf(close);
        const quint64 want2 = VirtualLayout::previewHash(vl->mathSource(close), vl->previewPointSize());
        checks.insert(QStringLiteral("d_rapid_edit_final_preview_is_current"), settled && x && x->previewHash == want2 && x->preview);
        checks.insert(QStringLiteral("d_superseded_renders_cancelled_or_dropped"),
                      double((instr::get(instr::PreviewCancelled) - cancelledBefore) + (instr::get(instr::PreviewStaleDrops) - staleBefore)));
    }
    {
        // Hot reload during composition: the editor (document, caret, preedit)
        // must survive; the reload must wait for the commit.
        PageSession *s = m_app->activeSession();
        const QString path = m_app->shell()->activePath();
        QFile f(path);
        QByteArray original;
        if (s && f.open(QIODevice::ReadOnly)) {
            original = f.readAll();
            f.close();
            const int caret = ed->textCursor().position();
            QInputMethodEvent pre(QStringLiteral("かんじ"), {});
            QApplication::sendEvent(ed, &pre);
            const quint64 renders = quint64(instr::get(instr::LuaRenders));
            QByteArray modified = original;
            modified.replace("name = \"", "name = \"Reloaded ");
            if (QFile w(path); w.open(QIODevice::WriteOnly | QIODevice::Truncate))
                w.write(modified);
            settle(600);
            const QString nameDuring = m_app->shell()->activeName();
            const bool preeditKept = ed->hasPreedit();
            QInputMethodEvent commit;
            commit.setCommitString(QStringLiteral("漢字"));
            QApplication::sendEvent(ed, &commit);
            settle(600);
            const QString nameAfter = m_app->shell()->activeName();
            checks.insert(QStringLiteral("c_reload_deferred_during_composition"), !nameDuring.startsWith(QStringLiteral("Reloaded")));
            checks.insert(QStringLiteral("c_preedit_kept"), preeditKept);
            checks.insert(QStringLiteral("c_reload_applied_after_commit"), nameAfter.startsWith(QStringLiteral("Reloaded")));
            checks.insert(QStringLiteral("c_same_document_after_reload"), ed->document() == s->document());
            checks.insert(QStringLiteral("c_caret_after_commit"), ed->textCursor().position() == caret + 2);
            checks.insert(QStringLiteral("c_lua_renders_during"), double(quint64(instr::get(instr::LuaRenders)) - renders));
            if (QFile w(path); w.open(QIODevice::WriteOnly | QIODevice::Truncate))
                w.write(original);
            settle(600);
        }
    }
    m_extra.insert(QStringLiteral("checks"), checks);
    addInstrSamples();
}

void BenchDriver::scenarioRecovery()
{
    // Modes:
    //  log      : type, snapshot every `every_ms`, log (rev, text) per snapshot
    //             to `log` (fsync'd) and keep going until killed/crashed.
    //  quit     : type `n` edits then close the window immediately (normal
    //             close must flush everything).
    //  diskfull : like log, but arm the fault VFS after `fail_after` snapshots,
    //             keep typing, verify the failure is surfaced, then disarm.
    const QString mode = m_args.value(QStringLiteral("mode"), QStringLiteral("log")).toString();
    const PageId page = manifestPage(QStringLiteral("page"));
    if (!page || !openAndWait(page, nullptr, nullptr)) {
        fprintf(stderr, "recovery: cannot open page\n");
        QCoreApplication::exit(3);
        return;
    }
    PageSession *s = m_app->activeSession();
    NoteEditor *ed = m_app->editorPane()->editor();
    QTextCursor c(s->document());
    c.movePosition(QTextCursor::End);
    ed->setTextCursor(c);
    const QString logPath = m_args.value(QStringLiteral("log")).toString();
    QFile log(logPath);
    if (!logPath.isEmpty())
        log.open(QIODevice::WriteOnly | QIODevice::Append);
    auto logSnapshot = [&](const char *what) {
        if (!log.isOpen())
            return;
        QJsonObject o;
        o.insert(QStringLiteral("event"), QString::fromLatin1(what));
        o.insert(QStringLiteral("rev"), double(s->revision()));
        o.insert(QStringLiteral("committed"), double(s->committedRevision()));
        o.insert(QStringLiteral("text"), s->document()->toPlainText());
        o.insert(QStringLiteral("status"), m_app->data()->status());
        log.write(QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n");
        log.flush();
        fsync(log.handle());
    };
    if (mode == QLatin1String("quit")) {
        typeText(QString::fromUtf8(kTypingText), m_args.value(QStringLiteral("n"), 200).toInt(), true);
        logSnapshot("before_close");
        m_app->window()->close(); // normal close path: flush + store close
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        return;
    }
    m_app->setAutosaveEnabled(false);
    const int every = m_args.value(QStringLiteral("every_ms"), 300).toInt();
    const int rounds = m_args.value(QStringLiteral("rounds"), 40).toInt();
    const int failAfter = m_args.value(QStringLiteral("fail_after"), -1).toInt();
    bool surfaced = false, recovered = false;
    QString failStatus;
    for (int r = 0; r < rounds; ++r) {
        typeText(QString::fromUtf8(kTypingText), 12, r % 3 == 0);
        if (mode == QLatin1String("diskfull") && r == failAfter)
            faultvfs::arm(faultvfs::Mode::DiskFull);
        logSnapshot("snapshot");
        m_app->flushSaves();
        settle(every);
        if (mode == QLatin1String("diskfull") && r > failAfter && failAfter >= 0 && !surfaced) {
            surfaced = m_app->store()->hasFailingJob() && !m_app->data()->status().startsWith(QStringLiteral("Saved"));
            failStatus = m_app->data()->status();
        }
        if (mode == QLatin1String("diskfull") && failAfter >= 0 && r == failAfter + 5)
            faultvfs::disarm();
    }
    if (mode == QLatin1String("diskfull")) {
        recovered = waitFor([&] { return s->committedRevision() == s->revision() && !m_app->store()->hasFailingJob(); }, 8000);
        logSnapshot("final");
        m_extra.insert(QStringLiteral("disk_full_surfaced"), surfaced);
        m_extra.insert(QStringLiteral("disk_full_status_text"), failStatus);
        m_extra.insert(QStringLiteral("recovered_after_space_freed"), recovered);
        m_extra.insert(QStringLiteral("final_rev"), double(s->revision()));
        m_extra.insert(QStringLiteral("injected_failures"), double(faultvfs::injectedFailures()));
        m_extra.insert(QStringLiteral("editor_text_kept"), s->document()->toPlainText().size() > 0);
    } else {
        waitFor([&] { return s->committedRevision() == s->revision(); }, 4000);
        logSnapshot("final");
    }
    finish();
}

} // namespace folia
