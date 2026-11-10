// Startup and idle tests (integrator-owned): a real AppController on a
// temporary data folder. The page read during startup is on screen with the
// window's first paint, with its caret and scroll position; an idle window
// gives up its spare buffers and keeps painting afterwards.

#include "app/AppController.h"
#include "app/WindowBufferTrimmer.h"
#include "editor/EditorPane.h"
#include "editor/NoteEditor.h"
#include "editor/PageSession.h"
#include "storage/Store.h"

#include <QApplication>
#include <QBackingStore>
#include <QMainWindow>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>
#include <memory>

using namespace folia;

namespace {

const bool s_offscreen = [] {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    return true;
}();

std::unique_ptr<AppController> start(const QString &home, bool prefetch = true, int trimMs = 0)
{
    AppController::Options o;
    o.homeDir = home;
    o.prefetchStartPage = prefetch;
    o.idleBufferTrimMs = trimMs;
    auto ctl = std::make_unique<AppController>(o);
    QString err;
    if (!ctl->start(&err))
        qFatal("AppController::start failed: %s", qPrintable(err));
    return ctl;
}

} // namespace

class LaunchTest : public QObject {
    Q_OBJECT

private slots:
    void startPageIsShownWithTheWindow()
    {
        QTemporaryDir dir;
        PageId page = 0;
        int caret = 0, scroll = 0;
        {
            // First run: the workspace is seeded; nothing to read ahead.
            auto app = start(dir.path());
            app->window()->show();
            QTRY_VERIFY(app->activeSession());
            page = app->newPage(0, -1, QStringLiteral("Long page"), false);
            NoteEditor *ed = app->editorPane()->editor();
            QStringList lines;
            for (int i = 0; i < 300; ++i)
                lines << QStringLiteral("Line %1 of a page long enough to scroll.").arg(i);
            ed->insertPlainText(lines.join(QLatin1Char('\n')));
            QTextCursor c(ed->document());
            c.setPosition(ed->document()->findBlockByNumber(200).position() + 5);
            ed->setTextCursor(c);
            ed->ensureCursorVisible();
            QTest::qWait(50);
            caret = c.position();
            scroll = ed->verticalScrollBar()->value();
            QVERIFY(scroll > 0);
            QVERIFY(app->shutdown(false));
        }
        {
            auto app = start(dir.path());
            QVERIFY(!app->activeSession()); // built, not shown yet
            app->window()->show();
            // No event loop turn since show(): the page is already the active one.
            QVERIFY(app->activeSession());
            QCOMPARE(app->currentPage(), page);
            NoteEditor *ed = app->editorPane()->editor();
            QVERIFY(!ed->isReadOnly());
            QCOMPARE(ed->textCursor().position(), caret);
            QCOMPARE(ed->verticalScrollBar()->value(), scroll);
            QTest::qWait(100);
            QCOMPARE(app->currentPage(), page);
            QCOMPARE(ed->verticalScrollBar()->value(), scroll);
            QVERIFY(app->shutdown(false));
        }
        {
            // Without the read-ahead the page arrives once the event loop runs.
            auto app = start(dir.path(), false);
            app->window()->show();
            QVERIFY(!app->activeSession());
            QTRY_VERIFY(app->activeSession());
            QCOMPARE(app->currentPage(), page);
            QCOMPARE(app->editorPane()->editor()->verticalScrollBar()->value(), scroll);
            QVERIFY(app->shutdown(false));
        }
    }

    void startPageArrivesWithoutShowingTheWindow()
    {
        QTemporaryDir dir;
        {
            auto app = start(dir.path());
            app->window()->show();
            QTRY_VERIFY(app->activeSession());
            QVERIFY(app->shutdown(false));
        }
        auto app = start(dir.path());
        QTRY_VERIFY(app->activeSession()); // never shown: on the next turn
        QVERIFY(app->shutdown(false));
    }

    void trashedLastPageFallsBackToFirstRoot()
    {
        QTemporaryDir dir;
        PageId first = 0;
        {
            auto app = start(dir.path());
            app->window()->show();
            QTRY_VERIFY(app->activeSession());
            first = app->currentPage();
            const PageId extra = app->newPage(0, -1, QStringLiteral("Soon gone"), false);
            QTRY_COMPARE(app->currentPage(), extra);
            app->flushSaves();
            QVERIFY(app->store()->flush());
            bool done = false;
            app->store()->trashPage(extra, {}, app.get(), [&](bool, PageId) { done = true; });
            QTRY_VERIFY(done);
            // "app.last_page" still names the trashed page.
            QVERIFY(app->shutdown(false));
        }
        auto app = start(dir.path());
        app->window()->show();
        QVERIFY(app->activeSession());
        QCOMPARE(app->currentPage(), first);
        QVERIFY(app->shutdown(false));
    }

    void idleWindowReleasesSpareBuffers()
    {
        if (!WindowBufferTrimmer::supported())
            QSKIP("only where the backing store keeps a swap chain (macOS)");
        QTemporaryDir dir;
        auto app = start(dir.path(), true, 150);
        app->window()->show();
        QTRY_VERIFY(app->activeSession());
        WindowBufferTrimmer *trimmer = app->bufferTrimmer();
        NoteEditor *ed = app->editorPane()->editor();
        int paints = 0;
        connect(ed, &NoteEditor::painted, this, [&] { ++paints; });
        // Not while it keeps painting.
        const int before = trimmer->trims();
        for (int i = 0; i < 12; ++i) {
            ed->insertPlainText(QStringLiteral("x"));
            QTest::qWait(40);
        }
        QCOMPARE(trimmer->trims(), before);
        QBackingStore *store = app->window()->backingStore();
        // Idle: trimmed, with a new store.
        QTRY_VERIFY_WITH_TIMEOUT(trimmer->trims() > before, 3000);
        QVERIFY(app->window()->backingStore() != store);
        // Painting goes on in the new store, and the next idle trims again
        // (the caret and the save status paint now and then on their own).
        const int painted = paints;
        const int trimmed = trimmer->trims();
        for (int i = 0; i < 6; ++i) {
            ed->insertPlainText(QStringLiteral("y"));
            QTest::qWait(40);
        }
        QVERIFY(paints > painted);
        QVERIFY(ed->toPlainText().contains(QStringLiteral("xxxxxxxxxxxxyyyyyy")));
        QTRY_VERIFY_WITH_TIMEOUT(trimmer->trims() > trimmed, 3000);
        // Off: nothing more.
        trimmer->setIdleMs(0);
        const int last = trimmer->trims();
        ed->insertPlainText(QStringLiteral("z"));
        QTest::qWait(500);
        QCOMPARE(trimmer->trims(), last);
        QVERIFY(!trimmer->trimNow() || WindowBufferTrimmer::supported()); // still available by hand
        QVERIFY(app->shutdown(false));
    }
};

QTEST_MAIN(LaunchTest)
#include "tst_launch.moc"
