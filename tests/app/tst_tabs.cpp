// Note tab tests (integrator-owned): a real AppController on a temporary data
// folder. Background tabs must not load their pages, a switch must cost one
// shell render, history is per tab, tabs survive a restart lazily, and
// trashed pages take their tabs with them.

#include "app/AppController.h"
#include "core/Instrument.h"
#include "shell/CommandRegistry.h"
#include "storage/Store.h"

#include <QApplication>
#include <QMainWindow>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

using namespace folia;

namespace {

const bool s_offscreen = [] {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    return true;
}();

std::unique_ptr<AppController> launch(const QString &home)
{
    AppController::Options o;
    o.homeDir = home;
    auto ctl = std::make_unique<AppController>(o);
    QString err;
    if (!ctl->start(&err))
        qFatal("AppController::start failed: %s", qPrintable(err));
    ctl->window()->show();
    return ctl;
}

// A page that exists on disk but has no session (nothing has opened it).
PageId storedPage(AppController *app, const QString &title, PageId parent = 0)
{
    const PageId id = app->store()->createPage(parent, -1, title, parent != 0);
    app->store()->flush(3000);
    return id;
}

CommandResult run(AppController *app, const char *cmd, const QVariantMap &args = {})
{
    return app->commands()->dispatchCommand(QString::fromLatin1(cmd), args);
}

QVariantMap args(std::initializer_list<std::pair<const char *, QVariant>> kv)
{
    QVariantMap m;
    for (const auto &p : kv)
        m.insert(QString::fromLatin1(p.first), p.second);
    return m;
}

QVector<PageId> tabPages(const AppController *app)
{
    QVector<PageId> v;
    for (int i = 0; i < app->tabs().count(); ++i)
        v << app->tabs().at(i).page;
    return v;
}

} // namespace

class TabTests : public QObject {
    Q_OBJECT
private slots:
    void backgroundTabsDoNotLoadTheirPages()
    {
        QTemporaryDir dir;
        auto app = launch(dir.path());
        QTRY_VERIFY(app->activeSession());
        QTest::qWait(50); // let the first render settle
        QVector<PageId> ids;
        for (int i = 0; i < 20; ++i)
            ids << storedPage(app.get(), QStringLiteral("Page %1").arg(i));
        const qint64 renders = instr::get(instr::LuaRenders);
        const int cached = app->docCacheEntries();
        for (PageId id : ids)
            QVERIFY(run(app.get(), "tab.open", args({{"id", qint64(id)}, {"background", true}})).ok);
        QTest::qWait(50);
        QCOMPARE(app->tabs().count(), 21);
        QCOMPARE(app->tabs().activeIndex(), 0);
        // In the order they were opened, right of the tab they came from.
        QCOMPARE(tabPages(app.get()).mid(1), ids);
        for (PageId id : ids)
            QVERIFY(!app->sessionCached(id));
        QCOMPARE(app->docCacheEntries(), cached);
        QCOMPARE(instr::get(instr::LuaRenders), renders); // no shell render either
    }

    void aSwitchCostsOneRender()
    {
        QTemporaryDir dir;
        auto app = launch(dir.path());
        QTRY_VERIFY(app->activeSession());
        const PageId first = app->currentPage();
        const PageId other = storedPage(app.get(), QStringLiteral("Other"));
        app->openInNewTab(other, true);
        QTest::qWait(50);
        qint64 renders = instr::get(instr::LuaRenders);
        QVERIFY(run(app.get(), "tab.next").ok);
        QTRY_COMPARE(app->currentPage(), other); // uncached: loaded now
        QTest::qWait(50);
        QCOMPARE(instr::get(instr::LuaRenders) - renders, 1);
        renders = instr::get(instr::LuaRenders);
        QVERIFY(run(app.get(), "tab.select", args({{"index", 1}})).ok);
        QCOMPARE(app->currentPage(), first); // cached: at once
        QTest::qWait(50);
        QCOMPARE(instr::get(instr::LuaRenders) - renders, 1);
    }

    void historyIsPerTab()
    {
        QTemporaryDir dir;
        auto app = launch(dir.path());
        QTRY_VERIFY(app->activeSession());
        const PageId first = app->currentPage();
        const PageId a = storedPage(app.get(), QStringLiteral("A"));
        const PageId b = storedPage(app.get(), QStringLiteral("B"));
        app->openPage(a);
        QTRY_COMPARE(app->currentPage(), a);
        app->openInNewTab(b, false);
        QTRY_COMPARE(app->currentPage(), b);
        QVERIFY(!run(app.get(), "nav.back").ok); // the new tab has no history
        QVERIFY(run(app.get(), "tab.prev").ok);
        QTRY_COMPARE(app->currentPage(), a);
        QVERIFY(run(app.get(), "nav.back").ok);
        QTRY_COMPARE(app->currentPage(), first);
        QVERIFY(run(app.get(), "nav.forward").ok);
        QTRY_COMPARE(app->currentPage(), a);
        QCOMPARE(tabPages(app.get()), QVector<PageId>({a, b})); // switching tabs did not navigate them
    }

    void tabsComeBackAfterARestartLazily()
    {
        QTemporaryDir dir;
        PageId a = 0, b = 0, c = 0;
        {
            auto app = launch(dir.path());
            QTRY_VERIFY(app->activeSession());
            a = storedPage(app.get(), QStringLiteral("A"));
            b = storedPage(app.get(), QStringLiteral("B"));
            c = storedPage(app.get(), QStringLiteral("C"));
            app->openPage(a);
            QTRY_COMPARE(app->currentPage(), a);
            app->openInNewTab(b, false);
            app->openInNewTab(c, true);
            QTRY_COMPARE(app->currentPage(), b);
            QVERIFY(app->shutdown(false));
        }
        auto app = launch(dir.path());
        QTRY_COMPARE(app->currentPage(), b);
        QCOMPARE(tabPages(app.get()), QVector<PageId>({a, b, c}));
        QCOMPARE(app->tabs().activeIndex(), 1);
        QVERIFY(!app->sessionCached(a));
        QVERIFY(!app->sessionCached(c));
    }

    void trashedPagesTakeTheirTabs()
    {
        QTemporaryDir dir;
        auto app = launch(dir.path());
        QTRY_VERIFY(app->activeSession());
        const PageId first = app->currentPage();
        const PageId parent = storedPage(app.get(), QStringLiteral("Parent"), first);
        const PageId child = storedPage(app.get(), QStringLiteral("Child"), parent);
        const PageId other = storedPage(app.get(), QStringLiteral("Other"));
        app->openInNewTab(parent, true);
        app->openInNewTab(child, true);
        app->openInNewTab(other, true);
        QVERIFY(run(app.get(), "tab.select", args({{"index", 3}})).ok); // the child's tab
        QTRY_COMPARE(app->currentPage(), child);
        QVERIFY(run(app.get(), "page.delete", args({{"id", qint64(parent)}})).ok);
        // Parent and child are both trashed: the parent's tab closes, and the
        // current tab (the child's) moves to the trashed page's parent.
        QTRY_COMPARE(app->currentPage(), first);
        QCOMPARE(tabPages(app.get()), QVector<PageId>({first, first, other}));
        QCOMPARE(app->tabs().activeIndex(), 1);
        QVERIFY(!app->tabs().current().back.contains(child));
        QVERIFY(!app->tabs().current().back.contains(parent));
    }

    // Regression: a slow load started before a navigation to a page that is
    // already open (or cached) must not land afterwards.
    void aStaleLoadDoesNotLand()
    {
        QTemporaryDir dir;
        auto app = launch(dir.path());
        QTRY_VERIFY(app->activeSession());
        const PageId first = app->currentPage();
        const PageId slow = storedPage(app.get(), QStringLiteral("Slow"));
        app->openPage(slow);  // uncached: its load is queued
        app->openPage(first); // already active: returns at once
        QTest::qWait(200);    // the load completes
        QCOMPARE(app->currentPage(), first);
        QCOMPARE(app->tabs().current().page, first);
    }

    void closingATabFreesItsPage()
    {
        QTemporaryDir dir;
        auto app = launch(dir.path());
        QTRY_VERIFY(app->activeSession());
        const PageId page = storedPage(app.get(), QStringLiteral("Big"));
        app->openInNewTab(page, false);
        QTRY_COMPARE(app->currentPage(), page);
        QVERIFY(app->sessionCached(page));
        QVERIFY(run(app.get(), "tab.close").ok);
        QTRY_VERIFY(!app->sessionCached(page));
        QCOMPARE(app->tabs().count(), 1);
        // Reopening brings it back, loaded again.
        QVERIFY(run(app.get(), "tab.reopen").ok);
        QTRY_COMPARE(app->currentPage(), page);
        QCOMPARE(app->tabs().count(), 2);
    }

    void newTabAndTheLastTab()
    {
        QTemporaryDir dir;
        auto app = launch(dir.path());
        QTRY_VERIFY(app->activeSession());
        const PageId first = app->currentPage();
        QVERIFY(run(app.get(), "tab.close").ok); // the last tab: nothing happens, quietly
        QCOMPARE(app->tabs().count(), 1);
        QVERIFY(run(app.get(), "tab.new").ok);
        QCOMPARE(app->tabs().count(), 2);
        QCOMPARE(app->tabs().activeIndex(), 1);
        const PageId created = app->currentPage();
        QVERIFY(created != first);
        QCOMPARE(app->tabs().current().page, created);
        QVERIFY(app->tabs().current().back.isEmpty()); // a new tab starts without history
        QVERIFY(run(app.get(), "tab.select", args({{"index", -1}})).ok);
        QCOMPARE(app->tabs().activeIndex(), 1);
        QVERIFY(run(app.get(), "tab.select", args({{"index", 9}})).ok); // no such tab: nothing happens
        QCOMPARE(app->tabs().activeIndex(), 1);
        app->moveTab(1, 0);
        QCOMPARE(tabPages(app.get()), QVector<PageId>({created, first}));
        QCOMPARE(app->tabs().activeIndex(), 0);
        QCOMPARE(app->currentPage(), created);
    }
};

QTEST_MAIN(TabTests)
#include "tst_tabs.moc"
