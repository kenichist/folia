// Storage, session and editor integration tests (headless).
#include "core/Instrument.h"
#include "core/LucideIcons.h"
#include "editor/EditorContext.h"
#include "editor/LinkPopups.h"
#include "editor/Links.h"
#include "editor/NoteEditor.h"
#include "editor/PageSession.h"
#include "editor/PreviewService.h"
#include "editor/VirtualLayout.h"
#ifdef FOLIA_WITH_MATH
#include "math/MathRenderer.h"
#endif
#include "storage/FaultVfs.h"
#include "storage/Store.h"

#include <QApplication>
#include <QClipboard>
#include <QImage>
#include <QInputMethodEvent>
#include <QPainter>
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

    void lucideIconsRender()
    {
        QVERIFY(icons::names().size() >= 12);
        QVERIFY(!icons::has(QStringLiteral("no-such-icon")));
        auto render = [](const QString &name) {
            QImage img(48, 48, QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::transparent);
            QPainter p(&img);
            const bool ok = icons::paint(&p, name, QRectF(0, 0, 48, 48), Qt::black);
            p.end();
            return ok ? img : QImage();
        };
        for (const QString &n : icons::names()) {
            const QImage img = render(n);
            QVERIFY2(!img.isNull(), qPrintable(n));
            int ink = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x)
                    ink += qAlpha(img.pixel(x, y)) > 0;
            QVERIFY2(ink > 40, qPrintable(n + QStringLiteral(": %1 pixels").arg(ink)));
        }
        // Elliptical arcs bulge: the globe's meridians pass x≈9.3 and x≈14.7
        // at y=6 (icon units); a straight chord would cross x=12 instead.
        const QImage globe = render(QStringLiteral("globe"));
        QVERIFY(qAlpha(globe.pixel(19, 12)) > 0);
        QVERIFY(qAlpha(globe.pixel(29, 12)) > 0);
        QCOMPARE(qAlpha(globe.pixel(24, 12)), 0);
        QVERIFY(!icons::icon(QStringLiteral("trash-2"), Qt::black).pixmap(QSize(16, 16), 2.0).isNull());
    }

    void namedLinksRoundTrip()
    {
        const QString src = QStringLiteral("See [Folia docs](https://example.com/a_(b)) or `[x](y)` and \\[no](link), "
                                           "[a\\]b](https://x.org/?q=1) $f[i](j)$.");
        PageContent pc;
        pc.id = 9;
        pc.found = true;
        BlockRecord r;
        r.id = 1;
        r.text = src;
        pc.blocks = {r};
        BlockId next = 100;
        PageSession ses(9, m_ctx, [&next] { return next++; });
        ses.load(pc);
        QTextBlock b = ses.document()->firstBlock();
        QCOMPARE(b.text(), QStringLiteral("See Folia docs or `[x](y)` and \\[no](link), a]b $f[i](j)$."));
        QStringList hrefs;
        for (auto it = b.begin(); !it.atEnd(); ++it)
            if (links::isLink(it.fragment().charFormat()))
                hrefs << it.fragment().text() + QLatin1Char('>') + it.fragment().charFormat().anchorHref();
        QCOMPARE(hrefs, (QStringList{QStringLiteral("Folia docs>https://example.com/a_(b)"),
                                     QStringLiteral("a]b>https://x.org/?q=1")}));
        QCOMPARE(links::blockSource(b), src);
        // Loading is not an edit; editing elsewhere on the line keeps the links.
        QCOMPARE(ses.takeSaveBatch().upserts.size(), 0);
        QTextCursor c(b);
        c.movePosition(QTextCursor::EndOfBlock);
        c.insertText(QStringLiteral(" End"));
        const SaveBatch batch = ses.takeSaveBatch();
        QCOMPARE(batch.upserts.size(), 1);
        QCOMPARE(batch.upserts[0].text, src + QStringLiteral(" End"));
        // Serialization escapes what the parser needs escaped.
        QCOMPARE(links::toMarkdown(QStringLiteral("[1] a\\"), QStringLiteral("https://e.org/a b(")),
                 QStringLiteral("[\\[1\\] a\\\\](https://e.org/a%20b%28)"));
        QString label, href;
        int end = 0;
        QVERIFY(links::parseLink(QStringLiteral("[\\[1\\] a\\\\](https://e.org/a%20b%28)"), 0, &label, &href, &end));
        QCOMPARE(label, QStringLiteral("[1] a\\"));
        QCOMPARE(links::normalizeUrl(QStringLiteral("example.com")), QStringLiteral("https://example.com"));
        QCOMPARE(links::normalizeUrl(QStringLiteral("me@example.com")), QStringLiteral("mailto:me@example.com"));
        QCOMPARE(links::normalizeUrl(QStringLiteral("http://x.y")), QStringLiteral("http://x.y"));
        QVERIFY(links::looksLikeUrl(QStringLiteral("docs.example.com/x")));
        QVERIFY(!links::looksLikeUrl(QStringLiteral("two words")));
    }

    void linksInEditor()
    {
        BlockId next = 1;
        PageSession ses(11, m_ctx, [&next] { return next++; });
        ses.initNew(0, QStringLiteral("Links"));
        NoteEditor ed(m_ctx);
        ed.resize(700, 400);
        ed.show();
        ed.setSession(&ses);
        ed.activateWindow();
        ed.setFocus();
        QTest::qWait(20);
        QTextDocument *d = ses.document();
        auto source = [&] { return links::blockSource(d->firstBlock()); };
        auto caretAt = [&](int pos) {
            QTextCursor c(d);
            c.setPosition(pos);
            ed.setTextCursor(c);
        };

        // Markdown typed in prose becomes a named link at the closing ')'.
        QTest::keyClicks(&ed, QStringLiteral("Go [here](https://x.y) now"));
        QCOMPARE(d->firstBlock().text(), QStringLiteral("Go here now"));
        QCOMPARE(source(), QStringLiteral("Go [here](https://x.y) now"));
        NoteEditor::LinkHit h = ed.linkAt(4);
        QVERIFY(h.valid() && h.named);
        QCOMPARE(h.start, 3);
        QCOMPARE(h.end, 7);
        QCOMPARE(h.href, QStringLiteral("https://x.y"));
        QVERIFY(!ed.linkAt(7).valid());

        // Typing at a link's end stays plain; typing inside extends the label.
        caretAt(7);
        QTest::keyClicks(&ed, QStringLiteral("X"));
        caretAt(4);
        QTest::keyClicks(&ed, QStringLiteral("Y"));
        QCOMPARE(source(), QStringLiteral("Go [hYere](https://x.y)X now"));
        caretAt(3);
        QTest::keyClicks(&ed, QStringLiteral("Z"));
        QCOMPARE(source(), QStringLiteral("Go Z[hYere](https://x.y)X now"));

        // Mod+K on a selection: Text + Link form; the address is normalized.
        const QString line = d->firstBlock().text();
        QTextCursor sel(d);
        sel.setPosition(int(line.indexOf(QStringLiteral("now"))));
        sel.setPosition(int(line.size()), QTextCursor::KeepAnchor);
        ed.setTextCursor(sel);
        QString why;
        QVERIFY2(ed.editLink(&why), qPrintable(why));
        auto *form = ed.findChild<LinkEditor *>();
        QVERIFY(form && form->isVisible());
        QCOMPARE(form->text(), QStringLiteral("now"));
        emit form->applied(QStringLiteral("Now!"), QStringLiteral("example.org"));
        QVERIFY(!form->isVisible());
        QCOMPARE(source(), QStringLiteral("Go Z[hYere](https://x.y)X [Now!](https://example.org)"));
        // Editing an existing link from the caret pre-fills both fields.
        caretAt(d->firstBlock().text().size() - 1);
        QVERIFY(ed.editLink());
        QCOMPARE(form->text(), QStringLiteral("Now!"));
        QCOMPARE(form->href(), QStringLiteral("https://example.org"));
        emit form->cancelled();
        QVERIFY(!form->isVisible());

        // Undo restores the text before the link was applied.
        ed.undo();
        QCOMPARE(source(), QStringLiteral("Go Z[hYere](https://x.y)X now"));

        // Copy gives Markdown; pasting it back recreates the link.
        QTextCursor all(d);
        all.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        ed.setTextCursor(all);
        ed.copy();
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("Go Z[hYere](https://x.y)X now"));
        QTextCursor end(d);
        end.movePosition(QTextCursor::End);
        ed.setTextCursor(end);
        QTest::keyClick(&ed, Qt::Key_Return);
        ed.paste();
        QCOMPARE(links::blockSource(d->lastBlock()), QStringLiteral("Go Z[hYere](https://x.y)X now"));
        QVERIFY(ed.linkAt(d->lastBlock().position() + 5).named);

        // Pasting an address onto selected words links them.
        QApplication::clipboard()->setText(QStringLiteral("https://paste.example/p"));
        QTextCursor word(d);
        word.setPosition(d->lastBlock().position() + d->lastBlock().text().size() - 3);
        word.setPosition(d->lastBlock().position() + d->lastBlock().text().size(), QTextCursor::KeepAnchor);
        ed.setTextCursor(word);
        ed.paste();
        QCOMPARE(links::blockSource(d->lastBlock()), QStringLiteral("Go Z[hYere](https://x.y)X [now](https://paste.example/p)"));

        // Bare URLs are links too (trailing punctuation excluded), but not in code spans.
        QTest::keyClick(&ed, Qt::Key_Return);
        QTest::keyClicks(&ed, QStringLiteral("visit https://example.com/a_b. or `www.no.example`"));
        const int bp = d->lastBlock().position();
        QTest::qWait(10);
        h = ed.linkAt(bp + 10);
        QVERIFY(h.valid() && !h.named);
        QCOMPARE(h.href, QStringLiteral("https://example.com/a_b"));
        QCOMPARE(h.end - bp, int(QStringLiteral("visit https://example.com/a_b").size()));
        QVERIFY(!ed.linkAt(bp + d->lastBlock().text().indexOf(QStringLiteral("www")) + 2).valid());

        // Links are refused inside code.
        QTest::keyClick(&ed, Qt::Key_Return);
        QTest::keyClicks(&ed, QStringLiteral("```js"));
        QTest::keyClick(&ed, Qt::Key_Return);
        QVERIFY(!ed.editLink(&why));
        QVERIFY(!why.isEmpty());
    }

    void inlineMathRendersInPlace()
    {
#ifndef FOLIA_WITH_MATH
        QSKIP("built without the math renderer");
#else
        EditorContext ctx;
        PreviewService previews(1 << 20, 1);
        ctx.previews = &previews;
        BlockId next = 1;
        PageSession ses(31, &ctx, [&next] { return next++; });
        ses.initNew(0, QStringLiteral("Inline"));
        NoteEditor ed(&ctx);
        ed.resize(760, 420);
        ed.show();
        ed.setSession(&ses);
        const QString line = QStringLiteral("Energy $E=mc^2$, ratio $\\frac{a}{b}$ and broken $\\frac{x$ end");
        QTextCursor c(ses.document());
        c.insertText(line + QStringLiteral("\nplain second line"));
        QTextCursor away(ses.document());
        away.movePosition(QTextCursor::End);
        ed.setTextCursor(away);
        const QTextBlock b = ses.document()->firstBlock();
        VirtualLayout *vl = ses.layout();
        auto spans = [&]() -> const std::vector<InlineMathState::Span> & {
            static const std::vector<InlineMathState::Span> none;
            const BlockData *d = PageSession::dataOf(b);
            return d && d->inlineMath ? d->inlineMath->spans : none;
        };
        auto rendered = [&] {
            ed.viewport()->repaint(); // lines waiting for a render re-probe on paint
            int n = 0;
            for (const InlineMathState::Span &sp : spans())
                n += bool(sp.list);
            return n;
        };
        // Rendered off the main thread, then drawn in place; the broken one
        // keeps its source. The text itself never changes.
        QTRY_COMPARE(rendered(), 2);
        QCOMPARE(int(spans().size()), 3);
        QVERIFY(!spans()[2].list);
        QCOMPARE(b.text(), line);
        QTextLayout *tl = b.layout();
        const QTextLine l0 = tl->lineAt(0);
        // Each rendered formula occupies exactly its own width (plus padding).
        for (int k = 0; k < 2; ++k) {
            const InlineMathState::Span &sp = spans()[size_t(k)];
            const qreal slot = l0.cursorToX(sp.end) - l0.cursorToX(sp.start);
            QVERIFY2(qAbs(slot - (sp.list->width + 3.0)) < 1.5,
                     qPrintable(QStringLiteral("slot %1 vs formula %2").arg(slot).arg(sp.list->width)));
        }
        // A fraction is taller than the text: its line gets room for it.
        QVERIFY(l0.y() > 0 || spans()[1].list->ascent <= l0.ascent());

        // The caret touching a formula shows its source; leaving renders it again.
        QTextCursor in(ses.document());
        in.setPosition(b.position() + spans()[0].start + 2);
        ed.setTextCursor(in);
        QCOMPARE(PageSession::dataOf(b)->inlineMath->revealed, 0);
        QCOMPARE(rendered(), 1);
        ed.setTextCursor(away);
        QCOMPARE(rendered(), 2); // from the cache: no waiting

        // A click on a rendered formula puts the caret just inside its source.
        const InlineMathState::Span frac = spans()[1];
        const QRectF br = vl->blockBoundingRect(b);
        const QTextLine fl = tl->lineForTextPosition(frac.start);
        const QPointF on((fl.cursorToX(frac.start) + fl.cursorToX(frac.end)) / 2, br.top() + fl.y() + fl.height() / 2);
        QCOMPARE(vl->hitTest(on, Qt::FuzzyHit), b.position() + frac.start + 1);

        // Copying the line gives the source.
        QTextCursor all(b);
        all.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        ed.setTextCursor(all);
        ed.copy();
        QCOMPARE(QApplication::clipboard()->text(), line);

        // Typing elsewhere on the line keeps the formulas rendered.
        QTextCursor tail(b);
        tail.movePosition(QTextCursor::EndOfBlock);
        ed.setTextCursor(tail);
        QTest::keyClicks(&ed, QStringLiteral("!!"));
        ed.setTextCursor(away);
        QCOMPARE(rendered(), 2);
#endif
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

    void resizingLaysOutTheViewAndEstimatesOnceSettled()
    {
        // A live resize (the split's divider, the window) of a long note:
        // each step lays out what is drawn and estimates nothing; the rest
        // is estimated once, after the width stops changing, to the same
        // heights an immediate full pass would give.
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
            r.text = QStringLiteral("Paragraph %1 lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do "
                                    "eiusmod tempor incididunt ut labore.").arg(i);
            pc.blocks << r;
        }
        PageSession ses(1, m_ctx, alloc);
        ses.load(pc);
        NoteEditor ed(m_ctx);
        ed.resize(560, 600); // narrower than the column: every width step rewraps
        ed.show();
        ed.setSession(&ses);
        QTest::qWait(50);
        ed.viewport()->repaint();
        VirtualLayout *vl = ses.layout();
        const qreal before = vl->documentSize().height();
        QVERIFY(vl->laidOutCount() > 0 && vl->laidOutCount() < 200);
        const qint64 est0 = instr::get(instr::BlockHeightEstimates);
        for (int i = 1; i <= 20; ++i) {
            ed.resize(560 - 8 * i, 600);
            ed.viewport()->repaint();
            QVERIFY2(vl->laidOutCount() < 200, qPrintable(QString::number(vl->laidOutCount())));
        }
        // No whole-document pass during the drag (a full pass is 20 000).
        QVERIFY2(instr::get(instr::BlockHeightEstimates) - est0 < 1000,
                 qPrintable(QString::number(instr::get(instr::BlockHeightEstimates) - est0)));
        // Only the drawn lines were rewrapped; far heights wait for the settle.
        QVERIFY(qAbs(vl->documentSize().height() - before) < before * 0.001);
        // Settled: one pass, and the document is taller at the narrower width.
        QTRY_VERIFY(vl->documentSize().height() > before * 1.05);
        const qint64 est1 = instr::get(instr::BlockHeightEstimates);
        QVERIFY(est1 - est0 >= 19000 && est1 - est0 < 22000);
        const qreal settled = vl->documentSize().height();
        vl->relayoutAll(); // an immediate full pass at this width
        ed.viewport()->repaint();
        QVERIFY2(qAbs(vl->documentSize().height() - settled) < settled * 0.002,
                 qPrintable(QStringLiteral("%1 vs %2").arg(vl->documentSize().height()).arg(settled)));
        // Wider than the column + margins: the column keeps its width, so
        // moving it costs the visible lines only, and nothing is re-estimated.
        ed.resize(1400, 600);
        QTest::qWait(250);
        ed.viewport()->repaint();
        const qint64 est2 = instr::get(instr::BlockHeightEstimates);
        const qreal wide = vl->documentSize().height();
        for (int i = 1; i <= 10; ++i) {
            ed.resize(1400 + 10 * i, 600);
            ed.viewport()->repaint();
        }
        QTest::qWait(250);
        QVERIFY2(instr::get(instr::BlockHeightEstimates) - est2 < 200,
                 qPrintable(QString::number(instr::get(instr::BlockHeightEstimates) - est2)));
        QCOMPARE(vl->documentSize().height(), wide);
        ed.setSession(nullptr);
    }
};

QTEST_MAIN(CoreTests)
#include "test_core.moc"
