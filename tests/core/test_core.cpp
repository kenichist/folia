// Storage, session and editor integration tests (headless).
#include "core/Instrument.h"
#include "editor/EditorContext.h"
#include "editor/NoteEditor.h"
#include "editor/PageSession.h"
#include "editor/VirtualLayout.h"
#include "storage/FaultVfs.h"
#include "storage/Store.h"

#include <QApplication>
#include <QInputMethodEvent>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>

using namespace folia;

namespace {

PageContent openSync(Store &s, PageId id)
{
    PageContent out;
    bool done = false;
    QObject ctx;
    s.openPage(id, &ctx, [&](PageContent pc) {
        out = std::move(pc);
        done = true;
    });
    QTest::qWaitFor([&] { return done; }, 5000);
    return out;
}

QVector<PageMeta> childrenSync(Store &s, PageId parent, int *total = nullptr)
{
    QVector<PageMeta> out;
    bool done = false;
    QObject ctx;
    s.fetchChildren(parent, 0, 1000, &ctx, [&](QVector<PageMeta> rows, int t) {
        out = rows;
        if (total)
            *total = t;
        done = true;
    });
    QTest::qWaitFor([&] { return done; }, 5000);
    return out;
}

} // namespace

class CoreTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    EditorContext *m_ctx = nullptr;
    int m_dbSerial = 0;

    QString freshDb() { return m_dir.filePath(QStringLiteral("ws%1.db").arg(++m_dbSerial)); }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_ctx = new EditorContext(this);
    }

    void storeRoundTrip()
    {
        Store s;
        QString err;
        Store::Options o;
        o.path = freshDb();
        QVERIFY2(s.open(o, &err), qPrintable(err));
        const PageId a = s.createPage(0, -1, QStringLiteral("Alpha"));
        const PageId b = s.createPage(a, -1, QStringLiteral("Beta"));
        SaveBatch batch;
        batch.page = b;
        batch.rev = 3;
        batch.hasOrder = true;
        for (int i = 0; i < 5; ++i) {
            BlockRecord r;
            r.id = s.allocateBlockId();
            r.text = QStringLiteral("line %1 — ünïcødé 漢字 🙂").arg(i);
            batch.upserts << r;
            batch.order << r.id;
        }
        QSignalSpy committed(&s, &Store::committed);
        s.commit(batch);
        QVERIFY(s.flush());
        QTRY_COMPARE(committed.count(), 1);
        PageContent pc = openSync(s, b);
        QVERIFY(pc.found);
        QCOMPARE(pc.title, QStringLiteral("Beta"));
        QCOMPARE(pc.rev, Revision(3));
        QCOMPARE(pc.blocks.size(), 5);
        QCOMPARE(pc.blocks[2].text, batch.upserts[2].text);
        // Reorder: move last to first, delete one.
        SaveBatch b2;
        b2.page = b;
        b2.rev = 4;
        b2.hasOrder = true;
        b2.order = {batch.order[4], batch.order[0], batch.order[1], batch.order[3]};
        b2.deletes = {batch.order[2]};
        s.commit(b2);
        QVERIFY(s.flush());
        pc = openSync(s, b);
        QCOMPARE(pc.blocks.size(), 4);
        QCOMPARE(pc.blocks[0].id, batch.order[4]);
        QCOMPARE(pc.blocks[3].id, batch.order[3]);
        int total = 0;
        const auto kids = childrenSync(s, a, &total);
        QCOMPARE(total, 1);
        QCOMPARE(kids[0].title, QStringLiteral("Beta"));
    }

    void moveRejectsCycles()
    {
        Store s;
        QString err;
        Store::Options o;
        o.path = freshDb();
        QVERIFY(s.open(o, &err));
        const PageId a = s.createPage(0, -1, QStringLiteral("A"));
        const PageId b = s.createPage(a, -1, QStringLiteral("B"));
        const PageId c = s.createPage(b, -1, QStringLiteral("C"));
        QObject ctx;
        MoveResult res = MoveResult::StorageError;
        bool done = false;
        s.movePage(a, c, -1, {}, &ctx, [&](MoveResult r, PageId) { res = r; done = true; });
        QTRY_VERIFY(done);
        QCOMPARE(res, MoveResult::Cycle);
        done = false;
        s.movePage(a, a, -1, {}, &ctx, [&](MoveResult r, PageId) { res = r; done = true; });
        QTRY_VERIFY(done);
        QCOMPARE(res, MoveResult::Cycle);
        // Legal move + reorder among siblings.
        const PageId d = s.createPage(0, -1, QStringLiteral("D"));
        done = false;
        s.movePage(c, 0, 0, {}, &ctx, [&](MoveResult r, PageId) { res = r; done = true; });
        QTRY_VERIFY(done);
        QCOMPARE(res, MoveResult::Ok);
        const auto roots = childrenSync(s, 0);
        QCOMPARE(roots.size(), 3);
        QCOMPARE(roots[0].id, c);
        QCOMPARE(roots[1].id, a);
        QCOMPARE(roots[2].id, d);
    }

    void siblingOrderRenumbersWhenGapExhausted()
    {
        Store s;
        QString err;
        Store::Options o;
        o.path = freshDb();
        QVERIFY(s.open(o, &err));
        const PageId first = s.createPage(0, -1, QStringLiteral("first"));
        s.createPage(0, -1, QStringLiteral("last"));
        // Keep inserting at index 1: forces repeated halving, then renumbering.
        for (int i = 0; i < 40; ++i)
            s.createPage(0, 1, QStringLiteral("mid%1").arg(i));
        QVERIFY(s.flush());
        const auto roots = childrenSync(s, 0);
        QCOMPARE(roots.size(), 42);
        QCOMPARE(roots.first().id, first);
        QCOMPARE(roots[1].title, QStringLiteral("mid39"));
        QCOMPARE(roots.last().title, QStringLiteral("last"));
        for (int i = 1; i < roots.size(); ++i)
            QVERIFY(roots[i].ord > roots[i - 1].ord);
    }

    void sessionRoundTripAndIdentity()
    {
        Store s;
        QString err;
        Store::Options o;
        o.path = freshDb();
        QVERIFY(s.open(o, &err));
        const PageId p = s.createPage(0, -1, QStringLiteral("Note"));
        auto alloc = [&s] { return s.allocateBlockId(); };
        PageSession ses(p, m_ctx, alloc);
        ses.initNew(0, QStringLiteral("Note"));
        QTextCursor c(ses.document());
        c.insertText(QStringLiteral("# Heading\nPara with $x^2$ inline.\n```python\ndef f():\n    return 1\n```\n$$\n\\frac{a}{b}\n$$\n$$E=mc^2$$\n- item"));
        const QString text = ses.document()->toPlainText();
        SaveBatch b = ses.takeSaveBatch();
        QVERIFY(b.hasOrder);
        // heading, para, code, math, single math, list item
        QCOMPARE(b.order.size(), 6);
        QCOMPARE(b.upserts.size(), 6);
        QCOMPARE(int(b.upserts[2].kind), int(BlockKind::Code));
        QCOMPARE(b.upserts[2].lang, QStringLiteral("python"));
        QCOMPARE(b.upserts[2].text, QStringLiteral("def f():\n    return 1"));
        QCOMPARE(int(b.upserts[3].kind), int(BlockKind::Math));
        QCOMPARE(b.upserts[4].lang, QStringLiteral("single"));
        s.commit(b);
        QVERIFY(s.flush());
        // No change -> empty batch.
        QCOMPARE(ses.takeSaveBatch().rev, Revision(0));
        // Typing in one paragraph -> exactly one upsert, no order change.
        QTextCursor c2(ses.document()->findBlockByNumber(1));
        c2.movePosition(QTextCursor::EndOfBlock);
        c2.insertText(QStringLiteral(" More."));
        SaveBatch b2 = ses.takeSaveBatch();
        QCOMPARE(b2.upserts.size(), 1);
        QVERIFY(!b2.hasOrder);
        QCOMPARE(b2.upserts[0].id, b.order[1]);
        s.commit(b2);
        // Edit inside the code block -> the code segment (opener id) is upserted.
        QTextCursor c3(ses.document()->findBlockByNumber(3));
        c3.insertText(QStringLiteral("# comment\n"));
        SaveBatch b3 = ses.takeSaveBatch();
        QCOMPARE(b3.upserts.size(), 1);
        QCOMPARE(b3.upserts[0].id, b.order[2]);
        QVERIFY(b3.upserts[0].text.startsWith(QStringLiteral("# comment\ndef f()")));
        s.commit(b3);
        QVERIFY(s.flush());
        const QString expected = ses.document()->toPlainText();
        // Reload: identical source text and identities.
        PageContent pc = openSync(s, p);
        PageSession again(p, m_ctx, alloc);
        again.load(pc);
        QCOMPARE(again.document()->toPlainText(), expected);
        QCOMPARE(PageSession::dataOf(again.document()->findBlockByNumber(1))->id, b.order[1]);
        Q_UNUSED(text);
    }

    void unclosedFenceRoundTrips()
    {
        Store s;
        QString err;
        Store::Options o;
        o.path = freshDb();
        QVERIFY(s.open(o, &err));
        const PageId p = s.createPage(0, -1, QStringLiteral("U"));
        auto alloc = [&s] { return s.allocateBlockId(); };
        PageSession ses(p, m_ctx, alloc);
        ses.initNew(0, QStringLiteral("U"));
        QTextCursor c(ses.document());
        c.insertText(QStringLiteral("before\n```js\nlet a = 1;\nstill code"));
        const QString expected = ses.document()->toPlainText();
        SaveBatch b = ses.takeSaveBatch();
        for (const BlockRecord &r : b.upserts)
            QCOMPARE(int(r.kind), int(BlockKind::Text));
        s.commit(b);
        QVERIFY(s.flush());
        PageSession again(p, m_ctx, alloc);
        again.load(openSync(s, p));
        QCOMPARE(again.document()->toPlainText(), expected);
        QCOMPARE(int(PageSession::roleOf(again.document()->findBlockByNumber(3))), int(LineRole::CodeLine));
    }

    void diskFullKeepsPendingEditsAndRecovers()
    {
        Store s;
        QString err;
        Store::Options o;
        o.path = freshDb();
        o.faultInjectable = true;
        o.retryInitialMs = 20;
        o.retryMaxMs = 40;
        QVERIFY(s.open(o, &err));
        const PageId p = s.createPage(0, -1, QStringLiteral("F"));
        QVERIFY(s.flush());
        auto alloc = [&s] { return s.allocateBlockId(); };
        PageSession ses(p, m_ctx, alloc);
        ses.initNew(0, QStringLiteral("F"));
        QTextCursor c(ses.document());
        c.insertText(QStringLiteral("first"));
        QSignalSpy failed(&s, &Store::commitFailed);
        QSignalSpy committed(&s, &Store::committed);
        faultvfs::arm(faultvfs::Mode::DiskFull);
        s.commit(ses.takeSaveBatch());
        QTRY_VERIFY(failed.count() >= 1);
        QVERIFY(s.hasFailingJob());
        // Keep editing while the disk is full: newer batches merge into the failing one.
        c.insertText(QStringLiteral(" second\nthird line"));
        s.commit(ses.takeSaveBatch());
        QTest::qWait(100);
        QCOMPARE(committed.count(), 0);
        QVERIFY(!s.flush(50));
        faultvfs::disarm();
        QTRY_VERIFY_WITH_TIMEOUT(committed.count() >= 1, 3000);
        QVERIFY(s.flush());
        QVERIFY(!s.hasFailingJob());
        PageSession again(p, m_ctx, alloc);
        again.load(openSync(s, p));
        QCOMPARE(again.document()->toPlainText(), QStringLiteral("first second\nthird line"));
        QCOMPARE(committed.last().at(1).value<Revision>(), ses.revision());
    }

    void preeditDoesNotBumpRevision()
    {
        Store s;
        QString err;
        Store::Options o;
        o.path = freshDb();
        QVERIFY(s.open(o, &err));
        const PageId p = s.createPage(0, -1, QStringLiteral("IME"));
        auto alloc = [&s] { return s.allocateBlockId(); };
        PageSession ses(p, m_ctx, alloc);
        ses.initNew(0, QStringLiteral("IME"));
        NoteEditor ed(m_ctx);
        ed.resize(600, 400);
        ed.show();
        ed.setSession(&ses);
        QTextCursor c(ses.document());
        c.insertText(QStringLiteral("abc "));
        ed.setTextCursor(c);
        const Revision r0 = ses.revision();
        // Compose "にほん" then commit "日本".
        QInputMethodEvent pre1(QStringLiteral("に"), {});
        QApplication::sendEvent(&ed, &pre1);
        QInputMethodEvent pre2(QStringLiteral("にほん"), {});
        QApplication::sendEvent(&ed, &pre2);
        QVERIFY(ed.hasPreedit());
        QCOMPARE(ses.revision(), r0);
        QCOMPARE(ses.document()->toPlainText(), QStringLiteral("abc "));
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral("日本"));
        QApplication::sendEvent(&ed, &commit);
        QVERIFY(!ed.hasPreedit());
        QCOMPARE(ses.document()->toPlainText(), QStringLiteral("abc 日本"));
        QVERIFY(ses.revision() > r0);
        // Undo removes the committed composition as one step.
        ed.undo();
        QCOMPARE(ses.document()->toPlainText(), QStringLiteral("abc "));
    }

    void pageLinksLoadAsLinkLines()
    {
        PageContent pc;
        pc.id = 7;
        pc.found = true;
        BlockRecord t; t.id = 1; t.text = QStringLiteral("## Child pages");
        BlockRecord l1; l1.id = 2; l1.kind = BlockKind::PageLink; l1.target = 20;
        BlockRecord l2; l2.id = 3; l2.kind = BlockKind::PageLink; l2.target = 21;
        BlockRecord after; after.id = 4; after.text = QStringLiteral("after");
        pc.blocks = {t, l1, l2, after};
        BlockId next = 100;
        PageSession ses(7, m_ctx, [&next] { return next++; });
        ses.load(pc);
        QTextDocument *d = ses.document();
        QCOMPARE(d->blockCount(), 4);
        QCOMPARE(int(PageSession::roleOf(d->findBlockByNumber(1))), int(LineRole::PageLink));
        QCOMPARE(int(PageSession::roleOf(d->findBlockByNumber(2))), int(LineRole::PageLink));
        QCOMPARE(ses.linkTargets(), (QVector<PageId>{20, 21}));
        NoteEditor ed(m_ctx);
        ed.resize(700, 400);
        ed.show();
        ed.setSession(&ses);
        QTest::qWait(30);
        const QRectF r = ses.layout()->blockBoundingRect(d->findBlockByNumber(1));
        QVERIFY2(r.height() > 10, qPrintable(QString::number(r.height())));
        QTextLayout *tl = d->findBlockByNumber(1).layout();
        QVERIFY(tl->lineCount() == 1);
        QVERIFY2(tl->lineAt(0).naturalTextWidth() > 30, qPrintable(QString::number(tl->lineAt(0).naturalTextWidth())));
    }

    void layoutIsLazyOnHugeDocument()
    {
        Store s;
        QString err;
        Store::Options o;
        o.path = freshDb();
        QVERIFY(s.open(o, &err));
        auto alloc = [&s] { return s.allocateBlockId(); };
        PageContent pc;
        pc.id = 1;
        pc.found = true;
        for (int i = 0; i < 20000; ++i) {
            BlockRecord r;
            r.id = s.allocateBlockId();
            r.text = QStringLiteral("Paragraph %1 lorem ipsum dolor sit amet, consectetur adipiscing elit.").arg(i);
            pc.blocks << r;
        }
        PageSession ses(1, m_ctx, alloc);
        ses.load(pc);
        NoteEditor ed(m_ctx);
        ed.resize(800, 600);
        ed.show();
        ed.setSession(&ses);
        QTest::qWait(50);
        VirtualLayout *vl = ses.layout();
        QVERIFY2(vl->laidOutCount() < 200, qPrintable(QString::number(vl->laidOutCount())));
        // Jump to the end: still only a screenful laid out.
        QTextCursor c(ses.document());
        c.movePosition(QTextCursor::End);
        ed.setTextCursor(c);
        ed.ensureCursorVisible();
        QTest::qWait(50);
        ed.viewport()->repaint();
        QVERIFY2(vl->laidOutCount() < 400, qPrintable(QString::number(vl->laidOutCount())));
        c.insertText(QStringLiteral(" typed at end"));
        QVERIFY(ses.document()->lastBlock().text().endsWith(QStringLiteral("typed at end")));
    }
};

QTEST_MAIN(CoreTests)
#include "test_core.moc"
