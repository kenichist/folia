// Independent robustness suite for the programmable Lua shell
// (performance-and-QA owner). Builds a real host — temp Store, EditorContext,
// EditorPane, CommandRegistry, DataContext, window + container, ShellHost —
// and attacks it with broken, hostile and resource-hungry layouts.
//
// Probes that can hang or crash the process (budget escapes, metamethods
// running outside the protected call, finalizers on lua_close) run in a child
// process: this binary re-executed with FOLIA_SHELL_CHILD=<scenario>, killed
// after a timeout. Bugs this suite found (budget escapes, unprotected host
// access, finalizer bypass, focus loop, reserved ids, reload dedupe) were fixed
// in src/shell; their probes are now plain regression assertions.

#include "core/Instrument.h"
#include "editor/EditorContext.h"
#include "editor/EditorPane.h"
#include "editor/NoteEditor.h"
#include "editor/PageSession.h"
#include "shell/CommandRegistry.h"
#include "shell/DataContext.h"
#include "shell/ListSources.h"
#include "shell/ShellHost.h"
#include "storage/Store.h"

#include <QApplication>
#include <QBoxLayout>
#include <QElapsedTimer>
#include <QFile>
#include <QFileSystemWatcher>
#include <QFrame>
#include <QInputMethodEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace folia;

namespace {

// Running the binary directly (outside ctest) should also be headless.
const bool s_offscreen = [] {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    return true;
}();

#ifdef Q_OS_MACOS
const char *kSameAsMod = "Cmd+K";  // Qt maps both Mod and Cmd to Ctrl (= Command) on macOS
const char *kNotMod = "Ctrl+K";    // physical Control key -> Meta
#else
const char *kSameAsMod = "Ctrl+K";
const char *kNotMod = "Cmd+K";
#endif

QByteArray spec(const QByteArray &viewBody, const QByteArray &name = "T", const QByteArray &fields = QByteArray())
{
    return "return {\n  version = 1,\n  name = \"" + name + "\",\n" + fields + "\n  view = function(state, ctx)\n"
        + viewBody + "\n  end,\n}\n";
}

const QByteArray kBaseline = R"LUA(
return {
  version = 1,
  name = "Baseline",
  view = function(state, ctx)
    return ui.row{
      id = "root",
      ui.column{ id = "side", ui.text{ id = "label", text = "baseline" }, ui.button{ id = "btn", text = "go" } },
      ui.editor{},
    }
  end,
}
)LUA";

const QByteArray kMinimal = R"LUA(
return {
  version = 1,
  name = "Minimal",
  view = function(state, ctx)
    return ui.column{ id = "root", ui.text{ id = "hello", text = "Hello" }, ui.editor{} }
  end,
}
)LUA";

double nearestRank(QVector<double> v, double p)
{
    if (v.isEmpty())
        return 0;
    std::sort(v.begin(), v.end());
    const int rank = qBound(1, int(std::ceil(p / 100.0 * v.size())), int(v.size()));
    return v[rank - 1];
}

struct Harness {
    QTemporaryDir dir;
    Store store;
    EditorContext ectx;
    CommandRegistry commands;
    std::unique_ptr<DataContext> data;
    std::unique_ptr<QWidget> window;
    QWidget *container = nullptr;
    EditorPane *pane = nullptr;
    std::unique_ptr<ShellHost> host;
    QStringList dispatched;
    QVariantMap lastArgs;
    int pings = 0;

    Harness()
    {
        Store::Options so;
        so.path = dir.filePath(QStringLiteral("workspace.db"));
        QString err;
        if (!store.open(so, &err))
            qFatal("Store::open failed: %s", qPrintable(err));
        data = std::make_unique<DataContext>(&store);
        window = std::make_unique<QWidget>();
        window->resize(1000, 700);
        auto *lay = new QVBoxLayout(window.get());
        lay->setContentsMargins(0, 0, 0, 0);
        container = new QWidget(window.get());
        lay->addWidget(container);
        pane = new EditorPane(&ectx, container);
        auto noop = [this](const QString &name, QVector<ArgSpec> args = {}) {
            CommandSpec c;
            c.name = name;
            c.title = name;
            c.args = std::move(args);
            c.run = [this, name](const QVariantMap &a) {
                dispatched << name;
                lastArgs = a;
                return CommandResult{true, {}};
            };
            commands.add(c);
        };
        // Dummy commands, plus every command the built-in presets reference.
        CommandSpec ping;
        ping.name = QStringLiteral("test.ping");
        ping.run = [this](const QVariantMap &) {
            ++pings;
            dispatched << QStringLiteral("test.ping");
            return CommandResult{true, {}};
        };
        commands.add(ping);
        CommandSpec echo;
        echo.name = QStringLiteral("test.echo");
        echo.args = {ArgSpec{QStringLiteral("text"), QMetaType::QString, true}};
        echo.run = [this](const QVariantMap &a) {
            dispatched << QStringLiteral("test.echo:") + a.value(QStringLiteral("text")).toString();
            return CommandResult{true, {}};
        };
        commands.add(echo);
        for (const char *n : {"page.new_child", "nav.back", "nav.forward", "nav.parent", "title.focus",
                              "code.copy_block", "page.move_up", "page.move_down", "page.indent", "page.outdent",
                              "palette.open", "outline.toggle", "page.trash", "shell.reload", "link.edit",
                              "app.settings"})
            noop(QString::fromLatin1(n));
        noop(QStringLiteral("page.open"), {ArgSpec{QStringLiteral("id"), QMetaType::LongLong, true}});
        noop(QStringLiteral("page.new"), {ArgSpec{QStringLiteral("parent"), QMetaType::LongLong},
                                          ArgSpec{QStringLiteral("title"), QMetaType::QString}});
        noop(QStringLiteral("page.delete"), {ArgSpec{QStringLiteral("id"), QMetaType::LongLong},
                                             ArgSpec{QStringLiteral("confirm"), QMetaType::Bool}});
        ShellHost::Deps d;
        d.editor = pane;
        d.editorCtx = &ectx;
        d.commands = &commands;
        d.data = data.get();
        d.window = window.get();
        d.container = container;
        host = std::make_unique<ShellHost>(d);
        window->show();
        QTest::qWait(1);
    }
    ~Harness()
    {
        if (pane)
            pane->setSession(nullptr);
        host.reset();
        window.reset();
        data.reset();
        store.close();
    }

    QFrame *banner() const { return container->findChild<QFrame *>(QStringLiteral("folia-shell-banner")); }
    bool bannerShown() const { return banner() && !banner()->isHidden(); }
    QString bannerText() const
    {
        const auto labels = banner() ? banner()->findChildren<QLabel *>() : QList<QLabel *>();
        return labels.isEmpty() ? QString() : labels.first()->text();
    }
    QString labelText(const char *id) const
    {
        auto *l = qobject_cast<QLabel *>(host->widgetById(QString::fromLatin1(id)));
        return l ? l->text() : QStringLiteral("<no label '%1'>").arg(QLatin1String(id));
    }
    QList<QShortcut *> shortcutsFor(const QKeySequence &seq) const
    {
        QList<QShortcut *> out;
        for (QShortcut *s : window->findChildren<QShortcut *>())
            if (s->key() == seq)
                out << s;
        return out;
    }
    bool load(const QByteArray &code, QString *err = nullptr, const char *name = "test.lua")
    {
        QString e;
        const bool ok = host->loadSource(code, QString::fromLatin1(name), &e);
        if (err)
            *err = e;
        return ok;
    }
    QString writeFile(const QString &name, const QByteArray &content) const
    {
        const QString p = dir.filePath(name);
        QFile f(p);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            qFatal("cannot write %s", qPrintable(p));
        f.write(content);
        f.close();
        return p;
    }
};

// A small page with text, a code block and a math block.
PageContent samplePage()
{
    PageContent pc;
    pc.id = 4242;
    pc.found = true;
    pc.title = QStringLiteral("Survivor");
    pc.rev = 3;
    auto add = [&](BlockId id, BlockKind k, const QString &text, const QString &lang = {}) {
        BlockRecord r;
        r.id = id;
        r.kind = k;
        r.text = text;
        r.lang = lang;
        pc.blocks.push_back(r);
    };
    add(1, BlockKind::Text, QStringLiteral("# Heading"));
    add(2, BlockKind::Text, QStringLiteral("Body text with café and 東京"));
    add(3, BlockKind::Code, QStringLiteral("int x = 1;\nint y = 2;"), QStringLiteral("cpp"));
    add(4, BlockKind::Math, QStringLiteral("\\frac{a}{b}"), QStringLiteral("single"));
    add(5, BlockKind::Text, QStringLiteral("last line"));
    return pc;
}

// Detaches the pane before a test-local session (and its document) dies.
struct Attach {
    EditorPane *pane;
    Attach(EditorPane *p, PageSession *s) : pane(p) { pane->setSession(s); }
    ~Attach() { pane->setSession(nullptr); }
};

struct ChildResult {
    bool finished = false;
    bool normalExit = false;
    int exitCode = -1;
    bool haveResult = false;
    bool ok = false;
    double ms = -1;
    QString err;
    QString stderrTail;
    double wallMs = 0;
};

ChildResult runChild(const QByteArray &scenario, int timeoutMs)
{
    ChildResult r;
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("FOLIA_SHELL_CHILD"), QString::fromLatin1(scenario));
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    p.setProcessEnvironment(env);
    QElapsedTimer t;
    t.start();
    // initTestCase() of the child diverts into runChildScenario(); the named
    // function is only a harmless fallback should that ever not happen.
    p.start(QCoreApplication::applicationFilePath(), {QStringLiteral("validMinimalLayout")});
    if (!p.waitForStarted(5000))
        return r;
    r.finished = p.waitForFinished(timeoutMs);
    r.wallMs = t.nsecsElapsed() / 1e6;
    if (!r.finished) {
        p.kill();
        p.waitForFinished(5000);
    } else {
        r.normalExit = p.exitStatus() == QProcess::NormalExit;
        r.exitCode = p.exitCode();
    }
    const QString out = QString::fromUtf8(p.readAllStandardOutput());
    r.stderrTail = QString::fromUtf8(p.readAllStandardError()).right(600);
    for (const QString &line : out.split(QLatin1Char('\n'))) {
        if (!line.startsWith(QLatin1String("CHILD_RESULT ")))
            continue;
        static const QRegularExpression re(QStringLiteral("ok=(\\d) ms=([0-9.]+) err=(.*)$"));
        const auto m = re.match(line);
        if (m.hasMatch()) {
            r.haveResult = true;
            r.ok = m.captured(1) == QLatin1String("1");
            r.ms = m.captured(2).toDouble();
            r.err = m.captured(3);
        }
    }
    return r;
}

QByteArray describe(const ChildResult &r)
{
    return QStringLiteral("finished=%1 normalExit=%2 exitCode=%3 result=%4 ok=%5 ms=%6 wall=%7ms err='%8' stderr='%9'")
        .arg(int(r.finished)).arg(int(r.normalExit)).arg(r.exitCode).arg(int(r.haveResult)).arg(int(r.ok)).arg(r.ms)
        .arg(r.wallMs, 0, 'f', 0)
        .arg(r.err.left(200), r.stderrTail.simplified().left(300))
        .toUtf8();
}

// Child side: run one hostile scenario against a host that already has a
// valid layout, report the outcome on stdout and exit without unwinding.
[[noreturn]] void runChildScenario(const QByteArray &name)
{
    QString err;
    bool ok = false;
    bool renderAfter = false;
    double ms = -1;
    {
        auto *h = new Harness; // leaked on purpose: _Exit below
        if (!h->load(kBaseline, &err)) {
            std::printf("CHILD_RESULT ok=0 ms=-1 err=baseline failed: %s\n", qPrintable(err));
            std::fflush(stdout);
            std::_Exit(3);
        }
        QByteArray code;
        if (name == "pcall_loop") {
            code = spec("while true do pcall(function() while true do end end) end");
        } else if (name == "pattern_bomb") {
            code = spec("local s = string.rep('a', 25)\n"
                        "string.find(s, string.rep('a?', 25) .. s)\n"
                        "return ui.column{ ui.editor{} }");
        } else if (name == "meta_index_error") {
            code = "return setmetatable({}, { __index = function(t, k) error('meta boom') end })";
        } else if (name == "meta_index_loop") {
            code = spec("return setmetatable({ ui.editor{} }, { __index = function(t, k) while true do end end })");
        } else if (name == "mem_retained") {
            code = R"LUA(
local head = false
local shell = { version = 1, name = "Hog", view = function(state, ctx) local _ = head; return ui.column{ ui.editor{} } end }
pcall(function() while true do head = { next = head } end end)
return shell
)LUA";
        } else if (name == "gc_loop" || name == "gc_loop_bypass") {
            // A: a layout holding an object whose __gc loops forever. The
            // bypass variant shadows the global `type` while calling
            // setmetatable, in case the sandbox's guard consults it.
            const QByteArray a = name == "gc_loop" ? QByteArray(R"LUA(
local guard = setmetatable({}, { __gc = function() while true do end end })
return { version = 1, name = "A", view = function(state, ctx) local _ = guard; return ui.column{ ui.editor{} } end }
)LUA") : QByteArray(R"LUA(
local real_type = type
type = function() return "not a table" end
local made, guard = pcall(setmetatable, {}, { __gc = function() while true do end end })
type = real_type
return { version = 1, name = made and "A-armed" or "A-refused",
         view = function(state, ctx) local _ = guard; return ui.column{ ui.editor{} } end }
)LUA");
            QElapsedTimer ta;
            ta.start();
            if (!h->load(a, &err)) {
                std::printf("CHILD_RESULT ok=0 ms=%.1f err=A_REJECTED: %s\n", ta.nsecsElapsed() / 1e6,
                            qPrintable(err.simplified().left(300)));
                std::fflush(stdout);
                std::_Exit(0);
            }
            std::printf("CHILD_INFO layout A loaded as %s\n", qPrintable(h->host->activeName()));
            std::fflush(stdout);
            code = kMinimal; // replacing A closes A's Lua state (runs its finalizer)
        } else if (name == "ctx_newindex_error" || name == "ctx_newindex_loop") {
            // view() arms a __newindex trap on the host-owned ctx table; the
            // next render's pushCtx() writes ctx.page into it.
            code = spec(QByteArray("ctx.page = nil\nsetmetatable(ctx, { __newindex = function(t, k, v) ")
                        + (name == "ctx_newindex_error" ? "error('ctx boom')" : "while true do end")
                        + " end })\nreturn ui.column{ ui.editor{} }");
            renderAfter = true;
        } else {
            std::printf("CHILD_RESULT ok=0 ms=-1 err=unknown scenario\n");
            std::fflush(stdout);
            std::_Exit(5);
        }
        QElapsedTimer t;
        t.start();
        ok = h->load(code, &err);
        if (ok && renderAfter) {
            std::printf("CHILD_INFO layout loaded; rendering again\n");
            std::fflush(stdout);
            t.restart();
            ok = h->host->renderNow(&err);
        }
        ms = t.nsecsElapsed() / 1e6;
        // Prove the process is alive and the host still answers.
        const bool layoutOk = h->host->hasLayout() && h->host->widgetById(QStringLiteral("editor"));
        std::printf("CHILD_RESULT ok=%d ms=%.1f err=%s%s\n", ok ? 1 : 0, ms, qPrintable(err.simplified().left(300)),
                    layoutOk ? "" : " [no layout]");
        std::fflush(stdout);
    }
    std::_Exit(0);
}

} // namespace

class ShellTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        const QByteArray child = qgetenv("FOLIA_SHELL_CHILD");
        if (!child.isEmpty())
            runChildScenario(child);
    }

    // ------------------------------------------------------------ basics --
    void validMinimalLayout()
    {
        Harness h;
        QString err;
        QVERIFY2(h.load(kMinimal, &err), qPrintable(err));
        QVERIFY(err.isEmpty());
        QVERIFY(h.host->hasLayout());
        QCOMPARE(h.host->activeName(), QStringLiteral("Minimal"));
        QVERIFY(h.host->lastError().isEmpty());
        QVERIFY(h.host->warnings().isEmpty());
        QVERIFY(!h.bannerShown());
        QCOMPARE(h.labelText("hello"), QStringLiteral("Hello"));
        QVERIFY(h.host->widgetById(QStringLiteral("root")));
        QCOMPARE(h.host->widgetById(QStringLiteral("editor")), static_cast<QWidget *>(h.pane));
        QVERIFY(h.container->isAncestorOf(h.pane));
        QVERIFY(h.pane->isVisible());
        QVERIFY(h.host->widgetById(QStringLiteral("root"))->isAncestorOf(h.pane));
    }

    void sandboxHasNoEscapeHatches()
    {
        Harness h;
        const QByteArray code = spec(R"LUA(
    local names = { "io", "os", "package", "debug", "require", "load", "loadfile", "dofile", "collectgarbage" }
    local seen = {}
    for _, n in ipairs(names) do seen[#seen + 1] = n .. "=" .. type(_G[n]) end
    seen[#seen + 1] = "string.dump=" .. type(string.dump)
    seen[#seen + 1] = "strmeta.dump=" .. type(getmetatable("").__index.dump)
    return ui.column{ ui.text{ id = "probe", text = table.concat(seen, ",") }, ui.editor{} })LUA");
        QString err;
        QVERIFY2(h.load(code, &err), qPrintable(err));
        QCOMPARE(h.labelText("probe"),
                 QStringLiteral("io=nil,os=nil,package=nil,debug=nil,require=nil,load=nil,loadfile=nil,dofile=nil,"
                                "collectgarbage=nil,string.dump=nil,strmeta.dump=nil"));
        // Binary chunks are refused (text-only load mode).
        QVERIFY(!h.load(QByteArray("\x1bLua\x55\x00", 6) + QByteArray(64, '\0'), &err));
        QVERIFY2(err.contains(QLatin1String("binary")), qPrintable(err));
    }

    // ------------------------------------------- invalid layouts rejected --
    void syntaxErrorKeepsPreviousLayout()
    {
        Harness h;
        QString err;
        QVERIFY2(h.load(kBaseline, &err), qPrintable(err));
        QWidget *root = h.host->widgetById(QStringLiteral("root"));
        QWidget *label = h.host->widgetById(QStringLiteral("label"));
        QWidget *btn = h.host->widgetById(QStringLiteral("btn"));
        const int live = h.host->liveWidgetCount();
        QVERIFY(root && label && btn);
        QVERIFY(!h.load("return { version = 1, view = function( end }", &err, "broken.lua"));
        QVERIFY2(err.contains(QLatin1String("broken.lua")), qPrintable(err));
        QCOMPARE(h.host->widgetById(QStringLiteral("root")), root);
        QCOMPARE(h.host->widgetById(QStringLiteral("label")), label);
        QCOMPARE(h.host->widgetById(QStringLiteral("btn")), btn);
        QCOMPARE(h.host->liveWidgetCount(), live);
        QCOMPARE(h.host->activeName(), QStringLiteral("Baseline"));
        QCOMPARE(h.labelText("label"), QStringLiteral("baseline"));
        QVERIFY(h.bannerShown());
        QVERIFY2(h.bannerText().contains(QLatin1String("last valid layout is still active")), qPrintable(h.bannerText()));
        QVERIFY(!h.host->lastError().isEmpty());
        // The surviving layout is fully functional.
        QVERIFY(h.host->renderNow(&err));
        QCOMPARE(h.host->widgetById(QStringLiteral("label")), label);
        QVERIFY(h.pane->isVisible());
    }

    void runtimeErrors_data()
    {
        QTest::addColumn<QByteArray>("code");
        QTest::addColumn<QString>("needle");
        QTest::newRow("error in view") << spec("error('boom from view')") << "boom from view";
        QTest::newRow("nil index in view") << spec("local t = nil; return t.x") << "attempt to index";
        QTest::newRow("view returns string") << spec("return 'not a node'") << "expected a ui node table";
        QTest::newRow("child is a number") << spec("return ui.column{ 42, ui.editor{} }") << "expected a ui node table";
        QTest::newRow("error in chunk") << QByteArray("error('top level boom')") << "top level boom";
        QTest::newRow("returns nothing") << QByteArray("local x = 1") << "must return a table";
        QTest::newRow("wrong version") << QByteArray("return { version = 2, view = function() return ui.editor{} end }")
                                       << "unsupported shell version";
        QTest::newRow("no view") << QByteArray("return { version = 1 }") << "needs a view";
        QTest::newRow("function prop") << spec("return ui.column{ ui.text{ text = function() end }, ui.editor{} }")
                                       << "only on_* fields may be handlers";
        QTest::newRow("children on leaf") << spec("return ui.column{ ui.text{ ui.spacer{} }, ui.editor{} }")
                                          << "nodes cannot have children";
        QTest::newRow("component redefined") << QByteArray("ui.component('text', function() end)") << "already defined";
        QTest::newRow("missing editor") << spec("return ui.column{ ui.text{ text = 'x' } }") << "no ui.editor{} slot";
        QTest::newRow("two editors") << spec("return ui.row{ ui.editor{}, ui.editor{} }") << "2 ui.editor{} slots";
        QTest::newRow("unknown node type") << spec("return ui.column{ { type = 'frobnicate' }, ui.editor{} }")
                                           << "unknown node type 'frobnicate'";
        QTest::newRow("node without type") << spec("return ui.column{ {}, ui.editor{} }") << "unknown node type ''";
        QTest::newRow("duplicate ids") << spec("return ui.column{ ui.text{ id = 'x' }, ui.button{ id = 'x' }, ui.editor{} }")
                                       << "duplicate id 'x'";
        QTest::newRow("list without source") << spec("return ui.column{ ui.list{ id = 'l' }, ui.editor{} }")
                                             << "needs source";
        QTest::newRow("list with bogus source") << spec("return ui.column{ ui.list{ id = 'l', source = { kind = 'disk' } }, ui.editor{} }")
                                                << "needs source";
        QTest::newRow("set in view") << spec("ctx.set{ x = 1 }; return ui.column{ ui.editor{} }")
                                     << "ctx.set is not allowed inside view()";
        QTest::newRow("toggle in view") << spec("ctx.toggle('x'); return ui.column{ ui.editor{} }")
                                        << "ctx.toggle is not allowed inside view()";
        QTest::newRow("run in view") << spec("ctx.run('test.ping'); return ui.column{ ui.editor{} }")
                                     << "ctx.run is not allowed inside view()";
    }

    void runtimeErrors()
    {
        QFETCH(QByteArray, code);
        QFETCH(QString, needle);
        Harness h;
        QString err;
        QVERIFY2(h.load(kBaseline, &err), qPrintable(err));
        QWidget *label = h.host->widgetById(QStringLiteral("label"));
        const int pingsBefore = h.pings;
        QVERIFY(!h.load(code, &err, "bad.lua"));
        QVERIFY2(err.contains(needle), qPrintable(QStringLiteral("error '%1' lacks '%2'").arg(err, needle)));
        QCOMPARE(h.host->activeName(), QStringLiteral("Baseline"));
        QCOMPARE(h.host->widgetById(QStringLiteral("label")), label);
        QCOMPARE(h.host->widgetById(QStringLiteral("editor")), static_cast<QWidget *>(h.pane));
        QVERIFY(h.pane->isVisible());
        QVERIFY(h.bannerShown());
        QCOMPARE(h.pings, pingsBefore); // a rejected ctx.run never dispatched
    }

    // ------------------------------------------------------------ budgets --
    void infiniteLoops_data()
    {
        QTest::addColumn<QByteArray>("code");
        QTest::newRow("while true in view") << spec("while true do end");
        QTest::newRow("repeat in view") << spec("local i = 0; repeat i = i + 1 until false");
        QTest::newRow("tail recursion") << spec("local function f(n) return f(n + 1) end; return f(0)");
        QTest::newRow("loop at top level") << QByteArray("while true do end");
        QTest::newRow("string work loop") << spec("local s = ''; while true do s = string.upper(string.rep('a', 64)) end");
    }

    void infiniteLoops()
    {
        QFETCH(QByteArray, code);
        Harness h;
        QString err;
        QVERIFY2(h.load(kBaseline, &err), qPrintable(err));
        QWidget *root = h.host->widgetById(QStringLiteral("root"));
        QElapsedTimer t;
        t.start();
        QVERIFY(!h.load(code, &err, "loop.lua"));
        const qint64 ms = t.elapsed();
        qInfo("infinite loop rejected after %lld ms: %s", ms, qPrintable(err.section(QLatin1Char('\n'), 0, 0)));
        QVERIFY2(ms < 1000, qPrintable(QStringLiteral("took %1 ms").arg(ms)));
        QVERIFY2(err.contains(QLatin1String("budget exceeded")), qPrintable(err));
        QCOMPARE(h.host->widgetById(QStringLiteral("root")), root);
        QCOMPARE(h.host->activeName(), QStringLiteral("Baseline"));
    }

    void memoryBombs_data()
    {
        QTest::addColumn<QByteArray>("code");
        QTest::newRow("growing table in view")
            << spec("local t = {}; while true do t[#t + 1] = string.rep('x', 1024) .. #t end");
        QTest::newRow("doubling string in view") << spec("local s = 'x'; while true do s = s .. s end");
        QTest::newRow("one huge string.rep") << spec("local s = string.rep('x', 1 << 30); return ui.editor{}");
        QTest::newRow("huge table.concat")
            << spec("local t = {}; for i = 1, 4096 do t[i] = string.rep('y', 4096) end; local s = table.concat(t, t[1]); return ui.editor{}");
        QTest::newRow("growing table at top level")
            << QByteArray("local t = {}; while true do t[#t + 1] = {} end");
    }

    void memoryBombs()
    {
        QFETCH(QByteArray, code);
        Harness h;
        QString err;
        QVERIFY2(h.load(kBaseline, &err), qPrintable(err));
        QWidget *root = h.host->widgetById(QStringLiteral("root"));
        const size_t before = h.host->luaMemory();
        QElapsedTimer t;
        t.start();
        QVERIFY(!h.load(code, &err, "bomb.lua"));
        qInfo("memory bomb rejected after %lld ms: %s", t.elapsed(), qPrintable(err.section(QLatin1Char('\n'), 0, 0)));
        QVERIFY2(err.contains(QLatin1String("memory budget exceeded")) || err.contains(QLatin1String("not enough memory")),
                 qPrintable(err));
        QCOMPARE(h.host->widgetById(QStringLiteral("root")), root);
        QCOMPARE(h.host->luaMemory(), before); // the active state was not touched
        QVERIFY(h.host->renderNow(&err));
    }

    void handlerBudgets()
    {
        Harness h;
        const QByteArray code = spec(R"LUA(
    return ui.column{
      ui.button{ id = "spin", text = "spin", on_click = function(ctx) while true do end end },
      ui.button{ id = "hog", text = "hog", on_click = function(ctx) local t = {}; while true do t[#t + 1] = string.rep('z', 512) end end },
      ui.button{ id = "calls", text = "calls", on_click = function(ctx) for i = 1, 30000 do ctx.commands() end end },
      ui.text{ id = "label", text = "kept" },
      ui.editor{},
    })LUA");
        QString err;
        QVERIFY2(h.load(code, &err), qPrintable(err));
        QWidget *label = h.host->widgetById(QStringLiteral("label"));
        struct Case { const char *id; const char *needle; };
        for (const Case &c : {Case{"spin", "budget exceeded"}, Case{"hog", "memory budget exceeded"},
                              Case{"calls", "host call budget exceeded"}}) {
            auto *b = qobject_cast<QPushButton *>(h.host->widgetById(QString::fromLatin1(c.id)));
            QVERIFY(b);
            QElapsedTimer t;
            t.start();
            b->click();
            const qint64 ms = t.elapsed();
            QVERIFY2(ms < 1000, qPrintable(QStringLiteral("%1 took %2 ms").arg(QLatin1String(c.id)).arg(ms)));
            QVERIFY(h.bannerShown());
            QVERIFY2(h.bannerText().contains(QLatin1String(c.needle)), qPrintable(h.bannerText()));
            QCOMPARE(h.host->widgetById(QStringLiteral("label")), label);
            QVERIFY(h.host->hasLayout());
        }
        QVERIFY(h.host->renderNow(&err));
    }

    void nestingLimit()
    {
        Harness h;
        auto nest = [](int depth) {
            return spec("local function nest(d) if d == 0 then return ui.editor{} end return ui.column{ nest(d - 1) } end\n"
                        "return nest(" + QByteArray::number(depth) + ")");
        };
        QString err;
        QVERIFY2(h.load(nest(40), &err), qPrintable(err)); // editor at depth 40: allowed
        QVERIFY(!h.load(nest(41), &err));
        QVERIFY2(err.contains(QLatin1String("nested deeper than 40 levels")), qPrintable(err));
        QVERIFY(!h.load(nest(5000), &err));
        QVERIFY2(err.contains(QLatin1String("nested deeper than 40 levels")), qPrintable(err));
        // A self-referencing node is caught by the same limit.
        QVERIFY(!h.load(spec("local n = ui.column{ ui.editor{} }; n[2] = n; return n"), &err));
        QVERIFY2(err.contains(QLatin1String("nested deeper")), qPrintable(err));
        QVERIFY(h.host->hasLayout());
    }

    void nodeLimit()
    {
        Harness h;
        auto many = [](int texts) {
            return spec("local kids = {}\nfor i = 1, " + QByteArray::number(texts)
                        + " do kids[i] = ui.spacer{} end\nkids[#kids + 1] = ui.editor{}\nreturn ui.column(kids)");
        };
        QString err;
        QVERIFY2(h.load(many(1998), &err), qPrintable(err)); // 1998 + editor + root = 2000 nodes
        QVERIFY(!h.load(many(1999), &err));
        QVERIFY2(err.contains(QLatin1String("more than 2000 nodes")), qPrintable(err));
        // Shared subtrees are counted per occurrence (no exponential blow-up).
        QVERIFY(!h.load(spec("local leaf = ui.spacer{}\nlocal n = ui.column{ leaf, leaf }\n"
                             "for i = 1, 30 do n = ui.column{ n, n } end\nreturn ui.row{ n, ui.editor{} }"), &err));
        QVERIFY2(err.contains(QLatin1String("more than 2000 nodes")) || err.contains(QLatin1String("nested deeper")),
                 qPrintable(err));
    }

    // ---------------------------------------------------------- shortcuts --
    void shortcutConflictsAndReserved()
    {
        Harness h;
        const QByteArray fields = QByteArray("  keys = {\n    [\"Mod+K\"] = \"test.ping\",\n    [\"") + kSameAsMod
            + "\"] = \"test.ping\",\n    [\"mod+k\"] = \"test.ping\",\n    [\"" + kNotMod
            + "\"] = \"test.ping\",\n    [\"Mod+Z\"] = \"test.ping\",\n    [\"Mod+Shift+Z\"] = \"test.ping\",\n"
              "    [\"Mod+C\"] = \"test.ping\",\n    [\"Mod+Alt+Shift+R\"] = \"test.ping\",\n"
              "    [\"Mod+J\"] = \"no.such.command\",\n    [\"Hyper+J\"] = \"test.ping\",\n  },";
        QString err;
        QVERIFY2(h.load(spec("return ui.column{ ui.editor{} }", "Keys", fields), &err), qPrintable(err));
        const QStringList w = h.host->warnings();
        const QString all = w.join(QLatin1Char('\n'));
        QCOMPARE(w.filter(QStringLiteral("conflicts with")).size(), 2); // Cmd/Ctrl+K and mod+k vs Mod+K
        QCOMPARE(w.filter(QStringLiteral("reserved")).size(), 4);       // Mod+Z, Mod+Shift+Z, Mod+C, recovery key
        QVERIFY2(all.contains(QLatin1String("keys['Mod+Z'] is reserved")), qPrintable(all));
        QVERIFY2(all.contains(QLatin1String("unknown command 'no.such.command'")), qPrintable(all));
        QVERIFY2(all.contains(QLatin1String("unknown modifier 'Hyper'")), qPrintable(all));
        // Exactly one binding for the conflicting sequence, none for reserved ones.
        const QKeySequence modK = QKeySequence(QStringLiteral("Ctrl+K"));
        QCOMPARE(h.shortcutsFor(modK).size(), 1);
        QCOMPARE(h.shortcutsFor(QKeySequence(QStringLiteral("Meta+K"))).size(), 1); // the non-conflicting one
        for (const char *r : {"Ctrl+Z", "Ctrl+Shift+Z", "Ctrl+C", "Ctrl+Alt+Shift+R", "Ctrl+J"})
            QVERIFY2(h.shortcutsFor(QKeySequence(QString::fromLatin1(r))).isEmpty(), r);
        QVERIFY(h.bannerShown()); // warnings banner
        QVERIFY2(h.bannerText().contains(QLatin1String("warnings")), qPrintable(h.bannerText()));
        // The surviving binding works.
        const int before = h.pings;
        emit h.shortcutsFor(modK).first()->activated();
        QCOMPARE(h.pings, before + 1);
    }

    // ---------------------------------------------------- handlers, ctx ----
    void handlerErrorShowsBannerKeepsLayout()
    {
        Harness h;
        const QByteArray code = spec(R"LUA(
    return ui.column{
      id = "col",
      ui.button{ id = "bad", text = "bad", on_click = function(ctx) error("handler boom") end },
      ui.button{ id = "good", text = "good", on_click = function(ctx) ctx.set{ n = (state.n or 0) + 1 } end },
      ui.text{ id = "count", text = tostring(state.n or 0) },
      ui.editor{},
    })LUA");
        QString err;
        QVERIFY2(h.load(code, &err), qPrintable(err));
        QWidget *col = h.host->widgetById(QStringLiteral("col"));
        auto *bad = qobject_cast<QPushButton *>(h.host->widgetById(QStringLiteral("bad")));
        QVERIFY(bad && !h.bannerShown());
        bad->click();
        QVERIFY(h.bannerShown());
        QVERIFY2(h.bannerText().contains(QLatin1String("Shell handler error")), qPrintable(h.bannerText()));
        QVERIFY2(h.bannerText().contains(QLatin1String("handler boom")), qPrintable(h.bannerText()));
        QCOMPARE(h.host->widgetById(QStringLiteral("col")), col);
        QCOMPARE(h.host->widgetById(QStringLiteral("bad")), static_cast<QWidget *>(bad));
        QVERIFY(h.host->hasLayout());
        // Other handlers keep working after the error.
        qobject_cast<QPushButton *>(h.host->widgetById(QStringLiteral("good")))->click();
        QTRY_COMPARE(h.labelText("count"), QStringLiteral("1"));
    }

    void ctxRunResults()
    {
        Harness h;
        const QByteArray code = spec(R"LUA(
    local function probe(ctx, name, args)
      local ok, msg = ctx.run(name, args)
      ctx.set{ out = tostring(ok) .. "|" .. tostring(msg) }
    end
    return ui.column{
      ui.button{ id = "unknown", on_click = function(ctx) probe(ctx, "no.such.command") end },
      ui.button{ id = "valid", on_click = function(ctx) probe(ctx, "test.echo", { text = "hi" }) end },
      ui.button{ id = "wrongtype", on_click = function(ctx) probe(ctx, "test.echo", { text = 5 }) end },
      ui.button{ id = "missing", on_click = function(ctx) probe(ctx, "test.echo", {}) end },
      ui.button{ id = "extra", on_click = function(ctx) probe(ctx, "test.ping", { bogus = true }) end },
      ui.text{ id = "out", text = state.out or "" },
      ui.editor{},
    })LUA");
        QString err;
        QVERIFY2(h.load(code, &err), qPrintable(err));
        auto click = [&](const char *id) {
            qobject_cast<QPushButton *>(h.host->widgetById(QString::fromLatin1(id)))->click();
        };
        click("unknown");
        QTRY_COMPARE(h.labelText("out"), QStringLiteral("false|unknown command 'no.such.command'"));
        QVERIFY(!h.bannerShown()); // a failed ctx.run is a return value, not an error
        click("valid");
        QTRY_COMPARE(h.labelText("out"), QStringLiteral("true|nil"));
        QCOMPARE(h.dispatched.last(), QStringLiteral("test.echo:hi"));
        click("wrongtype");
        QTRY_COMPARE(h.labelText("out"), QStringLiteral("false|test.echo: argument 'text' has the wrong type"));
        click("missing");
        QTRY_COMPARE(h.labelText("out"), QStringLiteral("false|test.echo: missing argument 'text'"));
        click("extra");
        QTRY_COMPARE(h.labelText("out"), QStringLiteral("false|test.ping: unknown argument 'bogus'"));
        QCOMPARE(h.dispatched.count(QStringLiteral("test.echo:hi")), 1);
    }

    void rejectedSetInViewHasNoSideEffect()
    {
        // ctx.set inside view() must be rejected *without* mutating state.
        Harness h;
        const QByteArray code = spec(R"LUA(
    local ok = pcall(ctx.set, { leak = "yes" })
    return ui.column{ ui.text{ id = "probe", text = tostring(ok) .. "|" .. tostring(state.leak) }, ui.editor{} })LUA");
        QString err;
        QVERIFY2(h.load(code, &err), qPrintable(err));
        QCOMPARE(h.labelText("probe"), QStringLiteral("false|nil"));
    }

    void focusInsideViewDoesNotLoop()
    {
        // view() must be a pure function of state: ctx.focus inside it should
        // be rejected or ignored, never re-trigger rendering forever.
        Harness h;
        QString err;
        // Fixed by rejecting it (like ctx.set / ctx.run inside view()); the
        // layout is refused and nothing re-renders afterwards.
        QVERIFY(!h.load(spec("ctx.focus('editor')\nreturn ui.column{ ui.editor{} }", "Focus"), &err));
        QVERIFY2(err.contains(QLatin1String("ctx.focus is not allowed inside view()")), qPrintable(err));
        QSignalSpy spy(h.host.get(), &ShellHost::rendered);
        QTest::qWait(500);
        qInfo("renders in 500 ms of idle after a view() that calls ctx.focus: %lld", qint64(spy.count()));
        QVERIFY2(spy.count() <= 1, qPrintable(QStringLiteral("%1 renders while idle").arg(spy.count())));
    }

    // ------------------------------------------------------- identity ------
    void rerenderKeepsWidgetIdentity()
    {
        Harness h;
        const QByteArray code = spec(R"LUA(
    return ui.row{
      id = "root",
      ui.column{
        id = "side",
        ui.text{ id = "count", text = "n=" .. tostring(state.n or 0) },
        ui.button{ id = "inc", text = "+", on_click = function(ctx) ctx.set{ n = (state.n or 0) + 1 } end },
        ui.spacer{},                       -- no id: keyed by position
        ui.input{ id = "query", placeholder = "q" },
        ui.list{ id = "outline", source = data.tree{} },
      },
      ui.editor{},
    })LUA", "Ident");
        QString err;
        QVERIFY2(h.load(code, &err), qPrintable(err));
        const QStringList ids = {QStringLiteral("root"), QStringLiteral("side"), QStringLiteral("count"),
                                 QStringLiteral("inc"), QStringLiteral("query"), QStringLiteral("outline")};
        QHash<QString, QWidget *> before;
        for (const QString &id : ids) {
            before.insert(id, h.host->widgetById(id));
            QVERIFY2(before.value(id), qPrintable(id));
        }
        auto *side = h.host->widgetById(QStringLiteral("side"));
        QWidget *spacer = nullptr;
        for (QWidget *c : side->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
            if (!ids.contains(c->objectName()) && c->sizePolicy().horizontalPolicy() == QSizePolicy::Expanding
                && !qobject_cast<QLabel *>(c) && !qobject_cast<QPushButton *>(c) && !qobject_cast<QLineEdit *>(c)
                && !c->inherits("QAbstractItemView"))
                spacer = c;
        QVERIFY(spacer);
        auto *query = qobject_cast<QLineEdit *>(before.value(QStringLiteral("query")));
        query->setText(QStringLiteral("typed by user"));
        const int live = h.host->liveWidgetCount();
        const qint64 created = instr::get(instr::WidgetsCreated);
        for (int i = 0; i < 5; ++i)
            qobject_cast<QPushButton *>(h.host->widgetById(QStringLiteral("inc")))->click();
        QTRY_COMPARE(h.labelText("count"), QStringLiteral("n=5"));
        for (int i = 0; i < 20; ++i)
            QVERIFY(h.host->renderNow(&err));
        QCOMPARE(instr::get(instr::WidgetsCreated), created);
        QCOMPARE(h.host->liveWidgetCount(), live);
        for (const QString &id : ids)
            QCOMPARE(h.host->widgetById(id), before.value(id));
        QVERIFY(side->isAncestorOf(spacer));
        QCOMPARE(query->text(), QStringLiteral("typed by user")); // input state is not reset by renders
        // A reload of a new version with the same ids reuses the widgets too.
        QVERIFY2(h.load(code, &err), qPrintable(err));
        for (const QString &id : ids)
            QCOMPARE(h.host->widgetById(id), before.value(id));
        QCOMPARE(query->text(), QStringLiteral("typed by user"));
    }

    void reservedEditorId()
    {
        // A non-editor node with id "editor" collides with the editor slot's
        // reconciliation key; it must be rejected or work, never silently drop.
        Harness h;
        QString err;
        const bool ok = h.load(spec("return ui.column{ ui.text{ id = 'editor', text = 'imposter' }, ui.editor{} }"), &err);
        QTest::qWait(10); // let deleteLater run
        if (ok) {
            bool labelAlive = false;
            for (QLabel *l : h.container->findChildren<QLabel *>())
                if (l->text() == QLatin1String("imposter") && !l->isHidden())
                    labelAlive = true;
            QVERIFY2(labelAlive, "layout accepted but the ui.text{id='editor'} widget is gone");
            QVERIFY(h.pane->isVisible());
        } else {
            QVERIFY2(err.contains(QLatin1String("editor")), qPrintable(err));
        }
    }

    // -------------------------------------------------- editor survival ----
    void editorSurvivesReload()
    {
        Harness h;
        PageSession session(4242, &h.ectx, [&h] { return h.store.allocateBlockId(); });
        session.load(samplePage());
        Attach attach(h.pane, &session);
        QString err;
        QVERIFY2(h.load(kBaseline, &err), qPrintable(err));
        NoteEditor *ed = h.pane->editor();
        QTextCursor c = ed->textCursor();
        c.setPosition(17);
        c.setPosition(21, QTextCursor::KeepAnchor);
        ed->setTextCursor(c);
        ed->setFocus();
        QTest::keyClicks(ed, QStringLiteral("XYZ")); // a real edit: undo stack non-empty
        const QString text = ed->toPlainText();
        const int pos = ed->textCursor().position();
        QTextDocument *doc = ed->document();
        QCOMPARE(doc, session.document());
        QVERIFY(doc->isUndoAvailable());
        const Revision rev = session.revision();
        // Reload into a structurally different layout (editor in another parent).
        const QByteArray other = spec(R"LUA(
    return ui.overlay{
      id = "ov",
      ui.column{ id = "main", ui.row{ id = "bar", ui.text{ id = "t", text = "bar" } }, ui.editor{} },
      ui.column{ id = "pop", ui.text{ text = "popup" } },
    })LUA", "Other");
        for (const QByteArray &code : {other, kMinimal, kBaseline}) {
            QVERIFY2(h.load(code, &err), qPrintable(err));
            QCOMPARE(h.host->widgetById(QStringLiteral("editor")), static_cast<QWidget *>(h.pane));
            QCOMPARE(h.pane->editor(), ed);
            QCOMPARE(ed->document(), doc);
            QCOMPARE(ed->toPlainText(), text);
            QCOMPARE(ed->textCursor().position(), pos);
            QVERIFY(doc->isUndoAvailable());
            QCOMPARE(session.revision(), rev); // reload is not an edit
            QVERIFY(h.pane->isVisible());
            QCOMPARE(h.pane->session(), &session);
        }
        // ... and a failed reload changes nothing either.
        QVERIFY(!h.load("syntax error here", &err));
        QCOMPARE(ed->document(), doc);
        QCOMPARE(ed->textCursor().position(), pos);
        QVERIFY(h.pane->isVisible());
    }

    void liveReloadDeferredDuringPreedit()
    {
        Harness h;
        PageSession session(4242, &h.ectx, [&h] { return h.store.allocateBlockId(); });
        session.load(samplePage());
        Attach attach(h.pane, &session);
        const QByteArray tag = QByteArray::number(QDateTime::currentMSecsSinceEpoch());
        auto version = [&](const char *name) {
            return spec("return ui.column{ id = 'root', ui.text{ id = 'v', text = '" + QByteArray(name) + "' }, ui.editor{} }",
                        QByteArray(name), "  -- " + tag + "\n");
        };
        const QString path = h.writeFile(QStringLiteral("shell.lua"), version("V1"));
        QString err;
        QVERIFY2(h.host->loadFile(path, &err), qPrintable(err));
        h.host->setWatchFile(true);
        QCOMPARE(h.host->activeName(), QStringLiteral("V1"));
        // Sanity: the watcher works when nothing is composing.
        h.writeFile(QStringLiteral("shell.lua"), version("V2"));
        QTRY_COMPARE_WITH_TIMEOUT(h.host->activeName(), QStringLiteral("V2"), 5000);
        // Start an IME composition in the editor.
        NoteEditor *ed = h.pane->editor();
        ed->setFocus();
        QTextCursor c = ed->textCursor();
        c.setPosition(5);
        ed->setTextCursor(c);
        const QString textBefore = ed->toPlainText();
        QTextDocument *doc = ed->document();
        QInputMethodEvent pre(QStringLiteral("にほ"), {});
        QCoreApplication::sendEvent(ed, &pre);
        QVERIFY(ed->hasPreedit());
        // Rewrite the file and make sure the change was observed.
        QFileSystemWatcher observer({path});
        QSignalSpy seen(&observer, &QFileSystemWatcher::fileChanged);
        h.writeFile(QStringLiteral("shell.lua"), version("V3"));
        QTRY_VERIFY_WITH_TIMEOUT(seen.count() > 0, 5000);
        QTest::qWait(400); // > the host's 120 ms reload debounce
        QCOMPARE(h.host->activeName(), QStringLiteral("V2"));
        QCOMPARE(h.labelText("v"), QStringLiteral("V2"));
        QVERIFY(ed->hasPreedit());
        QCOMPARE(ed->document(), doc);
        // Commit the composition: the pending reload is applied now.
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral("日本"));
        QCoreApplication::sendEvent(ed, &commit);
        QVERIFY(!ed->hasPreedit());
        QTRY_COMPARE_WITH_TIMEOUT(h.host->activeName(), QStringLiteral("V3"), 2000);
        QCOMPARE(h.labelText("v"), QStringLiteral("V3"));
        QCOMPARE(ed->document(), doc);
        QCOMPARE(h.pane->editor(), ed);
        QString expected = textBefore;
        expected.insert(5, QStringLiteral("日本"));
        QCOMPARE(ed->toPlainText(), expected);
        QCOMPARE(ed->textCursor().position(), 7);
    }

    void liveReloadAfterLayoutSwitch()
    {
        // Copying a layout over the active file must reload it, whatever was
        // loaded before (e.g. after Use "Keyboard" Layout, cp default over it).
        Harness h;
        const QByteArray tag = QByteArray::number(QDateTime::currentMSecsSinceEpoch()) + "-switch";
        auto version = [&](const char *name) {
            return spec("return ui.column{ ui.text{ id = 'v', text = '" + QByteArray(name) + "' }, ui.editor{} }",
                        QByteArray(name), "  -- " + tag + "\n");
        };
        h.host->setWatchFile(true);
        const QString a = h.writeFile(QStringLiteral("default.lua"), version("A"));
        QString err;
        QVERIFY2(h.host->loadFile(a, &err), qPrintable(err));
        h.writeFile(QStringLiteral("default.lua"), version("B"));
        QTRY_COMPARE_WITH_TIMEOUT(h.host->activeName(), QStringLiteral("B"), 5000);
        const QString k = h.writeFile(QStringLiteral("keyboard.lua"), version("K"));
        QVERIFY2(h.host->loadFile(k, &err), qPrintable(err));
        QCOMPARE(h.host->activeName(), QStringLiteral("K"));
        QFileSystemWatcher observer({k});
        QSignalSpy seen(&observer, &QFileSystemWatcher::fileChanged);
        h.writeFile(QStringLiteral("keyboard.lua"), version("B")); // same bytes as the last live reload
        QTRY_VERIFY_WITH_TIMEOUT(seen.count() > 0, 5000);
        QTest::qWait(600);
        QCOMPARE(h.host->activeName(), QStringLiteral("B"));
    }

    void fileLoadErrors()
    {
        Harness h;
        QString err;
        QVERIFY2(h.load(kBaseline, &err), qPrintable(err));
        QWidget *root = h.host->widgetById(QStringLiteral("root"));
        QVERIFY(!h.host->loadFile(h.dir.filePath(QStringLiteral("missing.lua")), &err));
        QVERIFY2(err.contains(QLatin1String("Cannot read")), qPrintable(err));
        const QString big = h.writeFile(QStringLiteral("big.lua"), "-- " + QByteArray(2 << 20, 'x') + "\n" + kMinimal);
        QVERIFY(!h.host->loadFile(big, &err));
        QVERIFY2(err.contains(QLatin1String("1 MiB")), qPrintable(err));
        QCOMPARE(h.host->widgetById(QStringLiteral("root")), root);
        QCOMPARE(h.host->activeName(), QStringLiteral("Baseline"));
    }

    // ------------------------------------- hostile code, child processes ---
    void budgetEscapeViaPcall()
    {
        const ChildResult r = runChild("pcall_loop", 4000);
        qInfo("pcall_loop: %s", describe(r).constData());
        QVERIFY2(r.finished, describe(r));
        QVERIFY2(r.normalExit && r.haveResult && !r.ok && r.ms < 1000, describe(r));
    }

    void patternMatchingCpuBomb()
    {
        const ChildResult r = runChild("pattern_bomb", 15000);
        qInfo("pattern_bomb: %s", describe(r).constData());
        QVERIFY2(r.finished && r.normalExit && r.haveResult, describe(r));
        QVERIFY2(r.ms < 1000, describe(r));
    }

    void metamethodErrorOnSpecTable()
    {
        // An erroring __index on the returned spec table must not reach the
        // host unprotected (it used to abort the process).
        const ChildResult r = runChild("meta_index_error", 5000);
        qInfo("meta_index_error: %s", describe(r).constData());
        QVERIFY2(r.finished && r.normalExit && r.haveResult, describe(r));
        QVERIFY2(!r.ok && !r.err.isEmpty() && r.ms < 1000, describe(r));
        QVERIFY2(!r.err.contains(QLatin1String("[no layout]")), describe(r));
    }

    void metamethodLoopOnNode()
    {
        // A looping __index on a node table must not run unbudgeted.
        const ChildResult r = runChild("meta_index_loop", 4000);
        qInfo("meta_index_loop: %s", describe(r).constData());
        QVERIFY2(r.finished && r.normalExit && r.haveResult && !r.ok && r.ms < 1000, describe(r));
        QVERIFY2(!r.err.contains(QLatin1String("[no layout]")), describe(r));
    }

    void memoryHeldAtBudgetByLayout()
    {
        // A layout that keeps the Lua heap at the 24 MB budget must not make
        // host-side allocations fail unprotected (it used to abort).
        const ChildResult r = runChild("mem_retained", 8000);
        qInfo("mem_retained: %s", describe(r).constData());
        QVERIFY2(r.finished && r.normalExit && r.haveResult, describe(r));
        // Either outcome is acceptable as long as the process survives and the
        // host still has a layout (checked in the child).
        QVERIFY2(!r.err.contains(QLatin1String("[no layout]")), describe(r));
    }

    void finalizerLoopOnLayoutReplace()
    {
        // Finalizers must never run unbudgeted: either __gc is refused when
        // the layout is loaded, or replacing the layout stays fast.
        const ChildResult r = runChild("gc_loop", 4000);
        qInfo("gc_loop: %s", describe(r).constData());
        QVERIFY2(r.finished && r.normalExit && r.haveResult, describe(r));
        if (r.err.startsWith(QLatin1String("A_REJECTED")))
            QVERIFY2(r.err.contains(QLatin1String("__gc")), describe(r));
        else
            QVERIFY2(r.ok && r.ms < 1000, describe(r));
    }

    void finalizerGuardBypass()
    {
        // Same probe, but the layout shadows the global `type` while calling
        // setmetatable. The guard must not depend on globals the shell can
        // replace.
        const ChildResult r = runChild("gc_loop_bypass", 4000);
        qInfo("gc_loop_bypass: %s", describe(r).constData());
        QVERIFY2(r.finished && r.normalExit && r.haveResult, describe(r));
        QVERIFY2(r.err.startsWith(QLatin1String("A_REJECTED")) || (r.ok && r.ms < 1000), describe(r));
    }

    void ctxMetamethodOnRender()
    {
        // view() can put a metatable on the host-owned ctx table; the host
        // must not then run shell code unprotected while filling ctx.
        const ChildResult r = runChild("ctx_newindex_error", 5000);
        qInfo("ctx_newindex_error: %s", describe(r).constData());
        QVERIFY2(r.finished && r.normalExit && r.haveResult, describe(r));
        QVERIFY2(r.ms < 1000 && !r.err.contains(QLatin1String("[no layout]")), describe(r));
    }

    void ctxMetamethodLoopOnRender()
    {
        const ChildResult r = runChild("ctx_newindex_loop", 4000);
        qInfo("ctx_newindex_loop: %s", describe(r).constData());
        QVERIFY2(r.finished && r.normalExit && r.haveResult && r.ms < 1000, describe(r));
    }

    // ------------------------------------------------------------- timing --
    void nilHolesInChildrenAreSkipped()
    {
        Harness h;
        QString err;
        QVERIFY2(h.load("return { version = 1, state = { off = true }, view = function(s) return ui.row{ id = 'root', "
                        "ui.text{ id = 'a', text = 'A' }, (not s.off) and ui.text{ id = 'b', text = 'B' } or nil, "
                        "nil, ui.column{ id = 'main', ui.editor{} } } end }", &err),
                 qPrintable(err));
        QVERIFY(h.host->widgetById(QStringLiteral("a")));
        QVERIFY(h.host->widgetById(QStringLiteral("main")));
        QVERIFY(!h.host->widgetById(QStringLiteral("b")));
    }

    void buttonIconsAndDefaultPresetControls()
    {
        Harness h;
        QString err;
        QVERIFY2(!h.load("return { version = 1, view = function() return ui.column{ "
                         "ui.button{ id = 'b', icon = 'no-such-icon' }, ui.editor{} } end }", &err),
                 "an unknown icon was accepted");
        QVERIFY2(err.contains(QStringLiteral("unknown icon")), qPrintable(err));
        const QString path = QStringLiteral(FOLIA_PRESET_SOURCE_DIR) + QStringLiteral("/default/shell.lua");
        QVERIFY2(h.host->loadFile(path, &err), qPrintable(err));
        auto *toggle = qobject_cast<QPushButton *>(h.host->widgetById(QStringLiteral("toggle_sidebar")));
        QVERIFY(toggle);
        QVERIFY(!toggle->icon().isNull());
        QVERIFY(toggle->text().isEmpty());
        QCOMPARE(toggle->accessibleName(), QStringLiteral("Hide sidebar"));
        QVERIFY(h.host->widgetById(QStringLiteral("sidebar"))->isVisible());
        toggle->click();
        QTRY_VERIFY(!h.host->widgetById(QStringLiteral("sidebar"))->isVisible());
        QVERIFY2(!h.bannerShown(), qPrintable(h.bannerText()));
        QCOMPARE(toggle->accessibleName(), QStringLiteral("Show sidebar"));
        toggle->click();
        QTRY_VERIFY(h.host->widgetById(QStringLiteral("sidebar"))->isVisible());
    }

    void sidebarRowHoverActions()
    {
        Harness h;
        h.store.createPage(0, -1, QStringLiteral("Alpha"));
        h.store.createPage(0, -1, QStringLiteral("Beta"));
        QString err;
        const QString path = QStringLiteral(FOLIA_PRESET_SOURCE_DIR) + QStringLiteral("/default/shell.lua");
        QVERIFY2(h.host->loadFile(path, &err), qPrintable(err));
        auto *list = qobject_cast<QListView *>(h.host->widgetById(QStringLiteral("outline")));
        QVERIFY(list);
        QTRY_VERIFY(list->model() && list->model()->rowCount() >= 2);
        auto *rows = qobject_cast<PageRowDelegate *>(list->itemDelegate());
        QVERIFY(rows);
        QCOMPARE(rows->tpl.actions.size(), 2);
        const QModelIndex first = list->model()->index(0, 0);
        const qint64 firstId = first.data(PageListModel::IdRole).toLongLong();
        const QRect r0 = list->visualRect(first);
        const QPoint trash = rows->actionRect(r0, 0).center();
        const QPoint plus = rows->actionRect(r0, 1).center();

        // Hovering a row reveals its actions.
        QTest::mouseMove(list->viewport(), QPoint(r0.left() + 60, r0.center().y()));
        QTRY_COMPARE(rows->hoverRow, 0);
        QCOMPARE(rows->hoverAction, -1);
        QTest::mouseMove(list->viewport(), plus);
        QTRY_COMPARE(rows->hoverAction, 1);

        // "+" adds a page inside that row's page; it does not open the row.
        h.dispatched.clear();
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, plus);
        QCOMPARE(h.dispatched, QStringList{QStringLiteral("page.new")});
        QCOMPARE(h.lastArgs.value(QStringLiteral("parent")).toLongLong(), firstId);
        // The trash asks before deleting that row's page.
        h.dispatched.clear();
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, trash);
        QCOMPARE(h.dispatched, QStringList{QStringLiteral("page.delete")});
        QCOMPARE(h.lastArgs.value(QStringLiteral("id")).toLongLong(), firstId);
        QVERIFY(h.lastArgs.value(QStringLiteral("confirm")).toBool());
        // The title still opens the page.
        h.dispatched.clear();
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, QPoint(r0.left() + 60, r0.center().y()));
        QVERIFY2(h.dispatched.contains(QStringLiteral("page.open")), qPrintable(h.dispatched.join(QLatin1Char(','))));

        // Bad actions are layout errors.
        QVERIFY(!h.load("return { version = 1, view = function() return ui.column{ ui.list{ id = 'l', "
                        "source = data.tree{}, row = { actions = { { icon = 'plus', command = 'no.such' } } } }, "
                        "ui.editor{} } end }", &err));
        QVERIFY2(err.contains(QStringLiteral("row action")), qPrintable(err));
        QVERIFY(!h.load("return { version = 1, view = function() return ui.column{ ui.list{ id = 'l', "
                        "source = data.tree{}, row = { actions = { { icon = 'nope', command = 'page.delete' } } } }, "
                        "ui.editor{} } end }", &err));
        QVERIFY2(err.contains(QStringLiteral("known icon")), qPrintable(err));
    }

    // -------------------------------------------------------------- theme --
    void systemModeFollowsAppearanceChanges()
    {
        // The offscreen platform cannot switch its appearance, so emit the
        // change Qt sends when the OS does (the host trusts its argument).
        QStyleHints *hints = QGuiApplication::styleHints();
        const auto osAppearance = [hints](Qt::ColorScheme s) { emit hints->colorSchemeChanged(s); };
        const auto layout = [](const char *mode) {
            return spec("return ui.column{ id = 'root', style = { bg = 'sidebar' },\n"
                        "  ui.text{ id = 'dark', text = tostring(ctx.prefers.dark) }, ui.editor{} }",
                        "Sys", QByteArray("  theme = { mode = '") + mode + "', material = 'frosted' },");
        };
        const QString lightSidebar = QStringLiteral("#f6f6f8");
        const QString darkSidebar = QStringLiteral("#26262a");
        Harness h;
        QString err;
        QVERIFY2(h.load(layout("system"), &err), qPrintable(err));
        QWidget *root = h.host->widgetById(QStringLiteral("root"));
        QVERIFY(root);
        QCOMPARE(h.ectx.theme().background, EditorTheme::light().background);
        QCOMPARE(h.labelText("dark"), QStringLiteral("false"));
        QVERIFY2(root->styleSheet().contains(lightSidebar), qPrintable(root->styleSheet()));
        QCOMPARE(h.host->warnings().size(), 1); // the unknown material notice

        // OS goes dark: editor theme, named style colors and ctx.prefers follow.
        osAppearance(Qt::ColorScheme::Dark);
        QTRY_COMPARE(h.ectx.theme().background, EditorTheme::dark().background);
        QTRY_COMPARE(h.labelText("dark"), QStringLiteral("true"));
        QVERIFY2(root->styleSheet().contains(darkSidebar), qPrintable(root->styleSheet()));
        QCOMPARE(QApplication::palette().color(QPalette::Window), EditorTheme::dark().background);
        QCOMPARE(h.host->widgetById(QStringLiteral("root")), root); // re-rendered, not rebuilt
        QCOMPARE(h.host->warnings().size(), 1);                    // not duplicated by the re-apply

        osAppearance(Qt::ColorScheme::Light);
        QTRY_COMPARE(h.ectx.theme().background, EditorTheme::light().background);
        QTRY_COMPARE(h.labelText("dark"), QStringLiteral("false"));
        QVERIFY2(root->styleSheet().contains(lightSidebar), qPrintable(root->styleSheet()));

        // A fixed mode keeps its colors, but the view still sees ctx.prefers.dark.
        QVERIFY2(h.load(layout("light"), &err), qPrintable(err));
        osAppearance(Qt::ColorScheme::Dark);
        QTRY_COMPARE(h.labelText("dark"), QStringLiteral("true"));
        QCOMPARE(h.ectx.theme().background, EditorTheme::light().background);
        QVERIFY2(h.host->widgetById(QStringLiteral("root"))->styleSheet().contains(lightSidebar),
                 qPrintable(h.host->widgetById(QStringLiteral("root"))->styleSheet()));
        QVERIFY2(h.host->lastError().isEmpty(), qPrintable(h.host->lastError()));
    }

    // Liquid Glass: offscreen there is no native glass, so glass nodes fall
    // back to their bg, the view sees "opaque" and no title bar inset, and
    // "glass" itself is not a problem worth a warning.
    void glassFallsBackToBg()
    {
        Harness h;
        QString err;
        QVERIFY2(h.load(spec("return ui.row{ id = 'root',\n"
                             "  ui.column{ id = 'panel', style = { material = 'glass', bg = 'sidebar', radius = 18 },\n"
                             "    ui.text{ id = 'mat', text = ctx.material },\n"
                             "    ui.text{ id = 'tb', text = ctx.titlebar.height .. '/' .. ctx.titlebar.inset },\n"
                             "    ui.list{ id = 'outline', source = data.tree{}, row = { radius = 8 },\n"
                             "             style = { bg = 'transparent' } } },\n"
                             "  ui.column{ ui.list{ id = 'crumbs', orientation = 'horizontal', source = data.ancestors{} },\n"
                             "             ui.editor{} } }",
                             "Glass", "  theme = { material = 'glass', titlebar = 'transparent' },"),
                        &err),
                 qPrintable(err));
        QVERIFY2(h.host->warnings().isEmpty(), qPrintable(h.host->warnings().join(QLatin1Char('\n'))));
        QVERIFY(!h.host->glassActive());
        QCOMPARE(h.host->titlebarHeight(), 0);
        QCOMPARE(h.labelText("mat"), QStringLiteral("opaque"));
        QCOMPARE(h.labelText("tb"), QStringLiteral("0/0"));
        const QString sheet = h.host->widgetById(QStringLiteral("panel"))->styleSheet();
        QVERIFY2(sheet.contains(h.host->namedColor(QStringLiteral("sidebar"))), qPrintable(sheet));
        QVERIFY2(sheet.contains(QLatin1String("border-radius:18px")), qPrintable(sheet));
        auto *list = qobject_cast<QListView *>(h.host->widgetById(QStringLiteral("outline")));
        QVERIFY(list);
        QCOMPARE(qobject_cast<PageRowDelegate *>(list->itemDelegate())->tpl.radius, 8.0);
        QVERIFY(!h.host->namedColor(QStringLiteral("fill")).isEmpty());
        QCOMPARE(h.host->namedColor(QStringLiteral("window")), h.host->namedColor(QStringLiteral("background")));

        // Empty space beside the breadcrumbs is not the list's: the press goes
        // on to its parent (a transparent title bar drags the window there).
        auto *crumbs = qobject_cast<QListView *>(h.host->widgetById(QStringLiteral("crumbs")));
        QVERIFY(crumbs);
        struct Count : QObject {
            int presses = 0;
            bool eventFilter(QObject *, QEvent *e) override
            {
                presses += e->type() == QEvent::MouseButtonPress;
                return false;
            }
        } count;
        crumbs->parentWidget()->installEventFilter(&count);
        QTest::mouseClick(crumbs->viewport(), Qt::LeftButton, {}, QPoint(crumbs->viewport()->width() - 3, 3));
        QCOMPARE(count.presses, 1);

        // Bad values: an unknown style.material is a layout error; unknown
        // theme values warn.
        QVERIFY(!h.load(spec("return ui.column{ ui.row{ id = 'x', style = { material = 'frosted' } }, ui.editor{} }"),
                        &err));
        QVERIFY2(err.contains(QLatin1String("style.material")), qPrintable(err));
        QVERIFY2(h.load(spec("return ui.column{ ui.editor{} }", "T", "  theme = { titlebar = 'hidden' },"), &err),
                 qPrintable(err));
        QVERIFY2(h.host->warnings().join(QLatin1Char('\n')).contains(QLatin1String("theme.titlebar")),
                 qPrintable(h.host->warnings().join(QLatin1Char('\n'))));
    }

    // Settings overrides: layered over the layout's theme, never edit it,
    // survive reloads and a different layout, and clear back to the layout.
    void themeOverridesLayerOverLayout()
    {
        const QByteArray fields = "  theme = { mode = 'light', font = { family = 'Georgia', size = 15 },\n"
                                  "    editor = { max_width = 700, margin = 40 }, colors = { accent = '#2f6fdf' } },";
        const QByteArray code = spec("return ui.column{ id = 'root', style = { bg = 'sidebar' }, ui.editor{} }",
                                     "Layered", fields);
        const QString darkSidebar = QStringLiteral("#26262a");
        Harness h;
        QString err;
        QVERIFY2(h.load(code, &err), qPrintable(err));
        QCOMPARE(h.ectx.theme().background, EditorTheme::light().background);
        QCOMPARE(h.ectx.theme().body.pointSizeF(), 15.0);

        QVariantMap font{{QStringLiteral("size"), 20}};
        QVariantMap colors{{QStringLiteral("accent"), QStringLiteral("#ff0000")}};
        QVariantMap editor{{QStringLiteral("max_width"), 0}};
        h.host->setThemeOverrides({{QStringLiteral("mode"), QStringLiteral("dark")}, {QStringLiteral("font"), font},
                                   {QStringLiteral("colors"), colors}, {QStringLiteral("editor"), editor}});
        const EditorTheme &t = h.ectx.theme();
        QCOMPARE(t.background, EditorTheme::dark().background);
        QCOMPARE(t.body.pointSizeF(), 20.0);
        QCOMPARE(t.body.family(), QStringLiteral("Georgia")); // sibling key kept by the deep merge
        QCOMPARE(t.maxColumnWidth, 0.0);                      // "full width"
        QCOMPARE(t.sideMargin, 40.0);                         // untouched layout value
        QCOMPARE(t.accent, QColor(QStringLiteral("#ff0000")));
        // The layout's own table is unchanged; the effective theme has both.
        QCOMPARE(h.host->layoutTheme().value(QStringLiteral("mode")).toString(), QStringLiteral("light"));
        QCOMPARE(h.host->theme().value(QStringLiteral("mode")).toString(), QStringLiteral("dark"));
        // Named style colors follow on the next render, with the same widgets.
        QWidget *root = h.host->widgetById(QStringLiteral("root"));
        QTRY_VERIFY2(root->styleSheet().contains(darkSidebar), qPrintable(root->styleSheet()));
        QCOMPARE(QApplication::palette().color(QPalette::Window), EditorTheme::dark().background);

        // Reloading the layout (or loading another) keeps the overrides.
        QVERIFY2(h.load(code, &err), qPrintable(err));
        QCOMPARE(h.ectx.theme().background, EditorTheme::dark().background);
        QVERIFY2(h.load(kMinimal, &err), qPrintable(err));
        QCOMPARE(h.ectx.theme().body.pointSizeF(), 20.0);
        QCOMPARE(h.ectx.theme().accent, QColor(QStringLiteral("#ff0000")));

        // Clearing them returns to the layout's values.
        QVERIFY2(h.load(code, &err), qPrintable(err));
        h.host->setThemeOverrides({});
        QCOMPARE(h.ectx.theme().background, EditorTheme::light().background);
        QCOMPARE(h.ectx.theme().body.pointSizeF(), 15.0);
        QCOMPARE(h.ectx.theme().maxColumnWidth, 700.0);
        QCOMPARE(h.ectx.theme().accent, QColor(QStringLiteral("#2f6fdf")));
        QVERIFY2(h.host->lastError().isEmpty(), qPrintable(h.host->lastError()));

        // A "system" override follows the OS like a layout's mode = "system".
        h.host->setThemeOverrides({{QStringLiteral("mode"), QStringLiteral("system")}});
        emit QGuiApplication::styleHints()->colorSchemeChanged(Qt::ColorScheme::Dark);
        QTRY_COMPARE(h.ectx.theme().background, EditorTheme::dark().background);
        emit QGuiApplication::styleHints()->colorSchemeChanged(Qt::ColorScheme::Light);
        QTRY_COMPARE(h.ectx.theme().background, EditorTheme::light().background);
    }

    void mergeThemeIsDeep()
    {
        const QVariantMap base{{QStringLiteral("mode"), QStringLiteral("light")},
                               {QStringLiteral("font"), QVariantMap{{QStringLiteral("family"), QStringLiteral("A")},
                                                                    {QStringLiteral("size"), 15}}}};
        const QVariantMap over{{QStringLiteral("font"), QVariantMap{{QStringLiteral("size"), 18}}},
                               {QStringLiteral("colors"), QVariantMap{{QStringLiteral("accent"), QStringLiteral("#123456")}}}};
        const QVariantMap m = ShellHost::mergeTheme(base, over);
        QCOMPARE(m.value(QStringLiteral("mode")).toString(), QStringLiteral("light"));
        QCOMPARE(m.value(QStringLiteral("font")).toMap().value(QStringLiteral("family")).toString(), QStringLiteral("A"));
        QCOMPARE(m.value(QStringLiteral("font")).toMap().value(QStringLiteral("size")).toInt(), 18);
        QCOMPARE(m.value(QStringLiteral("colors")).toMap().value(QStringLiteral("accent")).toString(), QStringLiteral("#123456"));
        QCOMPARE(ShellHost::mergeTheme(base, {}), base);
    }

    // Mod+, opens Settings from the native menu; a layout cannot take it.
    void settingsShortcutIsReserved()
    {
        Harness h;
        QString err;
        QVERIFY2(h.load(spec("return ui.column{ ui.editor{} }", "Keys",
                             "  keys = { [\"Mod+,\"] = \"test.ping\", [\"Mod+.\"] = \"test.ping\" },"), &err),
                 qPrintable(err));
        const QString all = h.host->warnings().join(QLatin1Char('\n'));
        QVERIFY2(all.contains(QLatin1String("keys['Mod+,'] is reserved")), qPrintable(all));
        QVERIFY(h.shortcutsFor(QKeySequence(QStringLiteral("Ctrl+,"))).isEmpty());
        QCOMPARE(h.shortcutsFor(QKeySequence(QStringLiteral("Ctrl+."))).size(), 1);
    }

    void defaultPresetSettingsButton()
    {
        Harness h;
        QString err;
        const QString path = QStringLiteral(FOLIA_PRESET_SOURCE_DIR) + QStringLiteral("/default/shell.lua");
        QVERIFY2(h.host->loadFile(path, &err), qPrintable(err));
        QVERIFY2(!h.bannerShown(), qPrintable(h.bannerText()));
        auto *b = qobject_cast<QPushButton *>(h.host->widgetById(QStringLiteral("open_settings")));
        QVERIFY(b);
        QVERIFY(!b->icon().isNull());
        QVERIFY(b->text().isEmpty());
        QCOMPARE(b->accessibleName(), QStringLiteral("Settings"));
        b->click();
        QTRY_VERIFY(h.dispatched.contains(QStringLiteral("app.settings")));
    }

    void timingDefaultPreset()
    {
        Harness h;
        const QString path = QStringLiteral(FOLIA_PRESET_SOURCE_DIR) + QStringLiteral("/default/shell.lua");
        QFile f(path);
        QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(path));
        const QByteArray code = f.readAll();
        QString err;
        QElapsedTimer t;
        t.start();
        QVERIFY2(h.host->loadFile(path, &err), qPrintable(err));
        const double coldMs = t.nsecsElapsed() / 1e6;
        QVERIFY2(h.host->warnings().isEmpty(), qPrintable(h.host->warnings().join(QLatin1Char('\n'))));
        QCOMPARE(h.host->activeName(), QStringLiteral("Default"));
        QTest::qWait(50);
        const int n = 200;
        QVector<double> render, load;
        render.reserve(n);
        load.reserve(n);
        for (int i = 0; i < n; ++i) {
            t.restart();
            QVERIFY(h.host->renderNow(&err));
            render << t.nsecsElapsed() / 1e3;
        }
        for (int i = 0; i < n; ++i) {
            t.restart();
            QVERIFY(h.host->loadSource(code, path, &err));
            load << t.nsecsElapsed() / 1e3;
            if (i % 50 == 0)
                QCoreApplication::processEvents(); // let deleteLater/model callbacks run
        }
        qInfo("shell timing (presets/default/shell.lua, offscreen, %s build): first loadFile %.3f ms; "
              "renderNow n=%d p50=%.1f us p95=%.1f us max=%.1f us; loadSource n=%d p50=%.1f us p95=%.1f us max=%.1f us; "
              "lua memory %zu bytes",
#ifdef NDEBUG
              "release",
#else
              "debug",
#endif
              coldMs, n, nearestRank(render, 50), nearestRank(render, 95), nearestRank(render, 100), n,
              nearestRank(load, 50), nearestRank(load, 95), nearestRank(load, 100), h.host->luaMemory());
        QVERIFY(h.host->hasLayout());
    }
};

QTEST_MAIN(ShellTest)
#include "tst_shell.moc"
