// Upload tests (integrator-owned): a real AppController on a temporary data
// folder. Markdown becomes a saved page opened in a tab (not rewritten when
// opened), its images become attachments, PDFs go into the current note and
// open beside it, unsupported files are reported, and quitting mid-import
// leaves no half page.

#include "app/AppController.h"
#include "app/DocumentImport.h"
#include "app/PdfViewerPage.h"
#include "core/Attachments.h"
#include "editor/PageSession.h"
#include "shell/TabStrip.h"
#include "storage/AttachmentStore.h"
#include "storage/Store.h"

#include <QApplication>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QMainWindow>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPdfWriter>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <memory>

#include "DocxFixture.h"

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

void write(const QString &path, const QByteArray &data)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size())
        qFatal("cannot write %s", qPrintable(path));
}

PageContent openSync(Store *s, PageId id)
{
    PageContent out;
    bool done = false;
    QObject ctx;
    s->openPage(id, &ctx, [&](PageContent pc) {
        out = std::move(pc);
        done = true;
    });
    QTest::qWaitFor([&] { return done; }, 5000);
    return out;
}

} // namespace

class UploadTests : public QObject {
    Q_OBJECT
private slots:
    void markdownBecomesASavedPageInATab()
    {
        QTemporaryDir home, src;
        QDir().mkpath(src.filePath(QStringLiteral("img")));
        QImage img(40, 30, QImage::Format_RGB32);
        img.fill(Qt::darkCyan);
        QVERIFY(img.save(src.filePath(QStringLiteral("img/pic.png"))));
        const QString md = src.filePath(QStringLiteral("notes.md"));
        write(md, "# Imported\n\nSome **bold** text\nwrapped.\n\n![Pic](img/pic.png)\n\n```cpp\nint x;\n```\n");

        auto app = launch(home.path());
        QTRY_VERIFY(app->activeSession());
        const PageId first = app->currentPage();
        app->uploadFiles({md});
        QTRY_COMPARE(app->tabs().count(), 2);
        const PageId id = app->tabs().current().page;
        QVERIFY(id != first);
        QTRY_COMPARE(app->currentPage(), id); // the first upload is shown
        QCOMPARE(app->activeSession()->title(), QStringLiteral("Imported"));
        QTRY_COMPARE(app->tabStrip()->titleAt(1), QStringLiteral("Imported"));

        // Saved before it opened, and opening it does not rewrite it.
        const Revision rev = app->activeSession()->revision();
        QCOMPARE(app->activeSession()->committedRevision(), rev);
        QTest::qWait(1200); // past the autosave deadline
        QCOMPARE(app->activeSession()->revision(), rev);
        QVERIFY(!app->activeSession()->hasUnsnapshotted());

        const PageContent pc = openSync(app->store(), id);
        QVERIFY(pc.found);
        QCOMPARE(pc.blocks.size(), 3);
        QCOMPARE(pc.blocks[0].text, QStringLiteral("Some **bold** text wrapped."));
        QString hash;
        QVERIFY(attachments::parseImageMarkdown(pc.blocks[1].text, &hash, nullptr));
        QVERIFY(QFile::exists(app->attachments()->pathFor(hash)));
        QVERIFY(app->attachments()->info(hash));
        QCOMPARE(pc.blocks[2].kind, BlockKind::Code);
        QCOMPARE(pc.blocks[2].lang, QStringLiteral("cpp"));
        QVERIFY(!app->attachments()->isPinned(hash) || app->sessionCached(id)); // the import's pin is gone
    }

    void wordBecomesASavedPageWithItsImages()
    {
        using namespace docxfixture;
        QTemporaryDir home, src;
        const QByteArray body = para("Titel", run("Plan")) + para("berschrift1", run("Goals"))
            + listPara(1, 0, "ship tabs") + para({}, run("see ") + image("rIdI", "Red box"));
        const QString docx = writeFile(src, QStringLiteral("plan.docx"), makeZip(docxParts(body)));
        auto app = launch(home.path());
        QTRY_VERIFY(app->activeSession());
        app->uploadFiles({docx});
        QTRY_COMPARE(app->tabs().count(), 2);
        const PageId id = app->tabs().current().page;
        QTRY_COMPARE(app->currentPage(), id);
        QCOMPARE(app->activeSession()->title(), QStringLiteral("Plan"));
        const PageContent pc = openSync(app->store(), id);
        QCOMPARE(pc.blocks.size(), 4);
        QCOMPARE(pc.blocks[0].text, QStringLiteral("# Goals"));
        QCOMPARE(pc.blocks[1].text, QStringLiteral("- ship tabs"));
        QCOMPARE(pc.blocks[2].text, QStringLiteral("see"));
        QString hash;
        QVERIFY(attachments::parseImageMarkdown(pc.blocks[3].text, &hash, nullptr));
        QVERIFY(QFile::exists(app->attachments()->pathFor(hash))); // streamed out of the archive
        const AttachmentInfo *info = app->attachments()->info(hash);
        QVERIFY(info);
        QCOMPARE(info->width, 8);
    }

    void severalFilesOpenTheFirstAndQueueTheRest()
    {
        QTemporaryDir home, src;
        write(src.filePath(QStringLiteral("a.md")), "# A\n\ntext a\n");
        write(src.filePath(QStringLiteral("b.txt")), "line one\n\nline two\n");
        auto app = launch(home.path());
        QTRY_VERIFY(app->activeSession());
        app->uploadFiles({src.filePath(QStringLiteral("a.md")), src.filePath(QStringLiteral("b.txt"))});
        QTRY_COMPARE(app->tabs().count(), 3);
        QTRY_COMPARE(app->documentImporter()->pending(), 0);
        QCOMPARE(app->tabs().activeIndex(), 1);
        QCOMPARE(app->activeSession()->title(), QStringLiteral("A"));
        const PageId b = app->tabs().at(2).page;
        QVERIFY(!app->sessionCached(b)); // a background tab: not loaded
        const PageContent pc = openSync(app->store(), b);
        QCOMPARE(pc.title, QStringLiteral("b"));
        QCOMPARE(pc.blocks.size(), 2);
    }

    void unsupportedFilesAreReported()
    {
        QTemporaryDir home, src;
        write(src.filePath(QStringLiteral("tool.exe")), "MZ");
        auto app = launch(home.path());
        QTRY_VERIFY(app->activeSession());
        app->uploadFiles({src.filePath(QStringLiteral("tool.exe"))});
        QMessageBox *box = nullptr;
        QTRY_VERIFY((box = app->window()->findChild<QMessageBox *>()) != nullptr);
        QVERIFY(box->informativeText().contains(QLatin1String("tool.exe")));
        QCOMPARE(app->tabs().count(), 1);
        box->close();
    }

    void droppedFilesAreUploaded()
    {
        QTemporaryDir home, src;
        const QString md = src.filePath(QStringLiteral("dropped.md"));
        write(md, "dropped text\n");
        auto app = launch(home.path());
        QTRY_VERIFY(app->activeSession());
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(md)});
        // Qt delivers a drop to the widget that accepted the drag's entry.
        QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(app->window(), &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(20, 20), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(app->window(), &drop);
        QTRY_COMPARE(app->tabs().count(), 2);
        QTRY_COMPARE(app->activeSession()->title(), QStringLiteral("dropped"));
    }

    void quittingMidImportLeavesNoPage()
    {
        QTemporaryDir home, src;
        const QString md = src.filePath(QStringLiteral("long.md"));
        QByteArray text;
        for (int i = 0; i < 200000; ++i)
            text += "A line of text that goes on for a while, number " + QByteArray::number(i) + "\n\n";
        write(md, text);
        {
            auto seeded = launch(home.path()); // the first launch seeds the welcome pages
            QTRY_VERIFY(seeded->activeSession());
            QVERIFY(seeded->shutdown(false));
        }
        qint64 pages = 0;
        {
            auto app = launch(home.path());
            QTRY_VERIFY(app->activeSession());
            pages = app->store()->pageCount();
            app->uploadFiles({md});
            QElapsedTimer t;
            t.start();
            QVERIFY(app->shutdown(false));
            QVERIFY2(t.elapsed() < 3000, qPrintable(QString::number(t.elapsed())));
        }
        auto app = launch(home.path());
        QTRY_VERIFY(app->activeSession());
        QCOMPARE(app->store()->pageCount(), pages); // cancelled before its page was created
    }

#ifdef Q_OS_MACOS
    void pdfGoesIntoTheNoteAndOpensBeside()
    {
        QTemporaryDir home, src;
        const QString pdf = src.filePath(QStringLiteral("paper.pdf"));
        {
            QPdfWriter w(pdf);
            QPainter p(&w);
            p.drawText(QPointF(100, 100), QStringLiteral("Hello"));
            w.newPage();
            p.drawText(QPointF(100, 100), QStringLiteral("Page two"));
        }
        auto app = launch(home.path());
        QTRY_VERIFY(app->activeSession());
        const PageId page = app->currentPage();
        app->uploadFiles({pdf});
        QTRY_VERIFY(app->pdfOpen());
        QCOMPARE(app->pdfViewer()->name(), QStringLiteral("paper.pdf"));
        QCOMPARE(app->tabs().count(), 1);     // no new page for a PDF
        QCOMPARE(app->currentPage(), page);   // its card is in this note
        QTRY_VERIFY(app->activeSession()->hasUnsnapshotted() || app->activeSession()->revision() > 1);
    }
#endif
};

QTEST_MAIN(UploadTests)
#include "tst_upload.moc"
