// PDF attachments (docs/ATTACHMENTS.md step 4): page geometry and the
// reference form, CoreGraphics info and rendering (whole pages and tiles
// drawn straight into tile-sized bitmaps), import and GC, the in-note card
// and its thumbnail on the PDF lane, and the viewer: prefix-sum layout,
// whole-page vs tile plans, zoom settling, stale requests dropped before
// they start, the document handle's lifetime and separate invalidation.
#include "app/AppController.h"
#include "app/PdfViewerPage.h"
#include "app/Platform.h"
#include "core/Attachments.h"
#include "core/Instrument.h"
#include "editor/EditorContext.h"
#include "editor/EditorPane.h"
#include "editor/MediaService.h"
#include "editor/NoteEditor.h"
#include "editor/PageSession.h"
#include "editor/PdfView.h"
#include "editor/VirtualLayout.h"
#include "storage/AttachmentStore.h"
#include "shell/CommandRegistry.h"
#include "storage/Store.h"

#include <QApplication>
#include <QMainWindow>
#include <QDir>
#include <QFile>
#include <QMimeData>
#include <QMutex>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QUrl>
#include <QWaitCondition>
#include <QtMath>

#include <memory>

using namespace folia;

namespace {

qint64 counter(instr::Counter c) { return instr::get(c); }

bool isRed(QRgb c) { return qRed(c) > 200 && qGreen(c) < 60 && qBlue(c) < 60; }
bool isBlue(QRgb c) { return qBlue(c) > 200 && qRed(c) < 60 && qGreen(c) < 60; }
bool isWhite(QRgb c) { return qRed(c) > 240 && qGreen(c) > 240 && qBlue(c) > 240; }

// A minimal PDF by hand: page 1 is 200×100 pt with /Rotate 90 and a red
// square in its (unrotated) top-left corner; page 2 has a 300×400 media box
// cropped to 200×300 with a blue square in the crop box's top-left corner.
QByteArray handWrittenPdf()
{
    const QByteArray c1 = "1 0 0 rg 0 80 20 20 re f\n";
    const QByteArray c2 = "0 0 1 rg 50 330 20 20 re f\n";
    QList<QByteArray> objs = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 5 0 R] /Count 2 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Rotate 90 /Contents 4 0 R >>",
        "<< /Length " + QByteArray::number(c1.size()) + " >>\nstream\n" + c1 + "endstream",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /CropBox [50 50 250 350] /Contents 6 0 R >>",
        "<< /Length " + QByteArray::number(c2.size()) + " >>\nstream\n" + c2 + "endstream",
    };
    QByteArray out = "%PDF-1.4\n";
    QList<int> offsets;
    for (int i = 0; i < objs.size(); ++i) {
        offsets << int(out.size());
        out += QByteArray::number(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
    }
    const int xref = int(out.size());
    out += "xref\n0 " + QByteArray::number(objs.size() + 1) + "\n0000000000 65535 f \n";
    for (int o : offsets)
        out += QByteArray::number(o).rightJustified(10, '0') + " 00000 n \n";
    out += "trailer\n<< /Size " + QByteArray::number(objs.size() + 1) + " /Root 1 0 R >>\nstartxref\n"
        + QByteArray::number(xref) + "\n%%EOF\n";
    return out;
}

// Blocks PDF jobs until released; records the keys in start order.
struct Gate {
    QMutex mu;
    QWaitCondition cv;
    bool open = false;
    QList<MediaKey> started;
    int count() { QMutexLocker l(&mu); return int(started.size()); }
    void release()
    {
        QMutexLocker l(&mu);
        open = true;
        cv.wakeAll();
    }
};

MediaService::Decoder gated(std::shared_ptr<Gate> g)
{
    return [g](const MediaService::Request &r, QString *) {
        QMutexLocker l(&g->mu);
        g->started << r.key;
        while (!g->open)
            g->cv.wait(&g->mu);
        QImage img(qMax(1, r.key.width), qMax(1, r.key.height), QImage::Format_RGB32);
        img.fill(Qt::lightGray);
        return img;
    };
}

MediaService::Decoder instant()
{
    return [](const MediaService::Request &r, QString *) {
        QImage img(qMax(1, r.key.width), qMax(1, r.key.height), QImage::Format_RGB32);
        img.fill(Qt::lightGray);
        return img;
    };
}

AttachmentInfo pdfInfoFor(const QString &hash, const QVector<PdfPageGeom> &pages)
{
    AttachmentInfo a;
    a.hash = hash;
    a.mime = QStringLiteral("application/pdf");
    a.pages = int(pages.size());
    a.pageGeom = attachments::encodePageGeom(pages);
    a.width = qRound(pages.first().w);
    a.height = qRound(pages.first().h);
    a.bytes = 12345;
    return a;
}

QVector<PdfPageGeom> letterPages(int n)
{
    PdfPageGeom g;
    g.w = 612;
    g.h = 792;
    return QVector<PdfPageGeom>(n, g);
}

} // namespace

class PdfTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    int m_serial = 0;
    QString fresh(const QString &name) { return m_dir.filePath(QStringLiteral("%1-%2").arg(++m_serial).arg(name)); }
    static QString fakeHash(char c) { return QString(attachments::kHashLength, QLatin1Char(c)); }
    bool cg() const
    {
#ifdef Q_OS_MACOS
        return true;
#else
        return false;
#endif
    }

    struct Rig {
        Store store;
        std::unique_ptr<AttachmentStore> as;
        std::unique_ptr<MediaService> media;
        EditorContext ctx;
        Rig(const QString &db, const QString &dir, bool withMedia, MediaService::Decoder decoder = {},
            MediaService::Config mc = {})
        {
            Store::Options o;
            o.path = db;
            QString err;
            if (!store.open(o, &err))
                qFatal("store: %s", qPrintable(err));
            AttachmentStore::Options ao;
            ao.dir = dir;
            ao.gcDelayMs = 3600000;
            ao.startupGcDelayMs = 3600000;
            as = std::make_unique<AttachmentStore>(&store, ao);
            ctx.attachments = as.get();
            if (withMedia) {
                media = std::make_unique<MediaService>(mc);
                if (decoder)
                    media->setDecoder(decoder); // otherwise the real CoreGraphics renderer
                ctx.media = media.get();
            }
        }
        ~Rig()
        {
            as->waitForImports();
            if (media)
                media->waitIdle();
        }
        BlockId alloc() { return store.allocateBlockId(); }
        AttachmentStore::ImportResult importSync(const AttachmentStore::Source &src)
        {
            AttachmentStore::ImportResult out;
            bool done = false;
            QObject c;
            as->import(src, &c, [&](AttachmentStore::ImportResult r) {
                out = r;
                done = true;
            });
            QTest::qWaitFor([&] { return done; }, 20000);
            return out;
        }
    };

    QString writePdf(const QByteArray &bytes)
    {
        const QString p = fresh(QStringLiteral("doc.pdf"));
        QFile f(p);
        if (!f.open(QIODevice::WriteOnly))
            return QString();
        f.write(bytes);
        return p;
    }

private slots:
    void initTestCase() { QVERIFY(m_dir.isValid()); }

    // --------------------------------------------------------- formats ----
    void pageGeomRoundTripsInRuns()
    {
        QVector<PdfPageGeom> pages = letterPages(150);
        PdfPageGeom land;
        land.w = 792;
        land.h = 612;
        land.rotation = 90;
        land.flags = PdfPageGeom::Layers;
        land.imagePixels = 2550u * 3300u;
        pages.insert(10, land);
        pages.append(land);
        const QByteArray blob = attachments::encodePageGeom(pages);
        QCOMPARE(blob.size(), 1 + 4 * 20); // runs: letter ×10, landscape, letter ×140, landscape
        QCOMPARE(attachments::decodePageGeom(blob), pages);
        QCOMPARE(attachments::firstPageGeom(blob), pages.first());
        // A uniform document of any length is one run.
        QCOMPARE(attachments::encodePageGeom(letterPages(5000)).size(), 21);
        // Version 1 blobs (no hints) still read; garbage reads as nothing.
        QByteArray v1(1, char(1));
        const char run[] = {3, 0, 0, 0};
        v1.append(run, 4);
        float w = 100, h = 50;
        v1.append(reinterpret_cast<const char *>(&w), 4); // little-endian hosts only (all we build for)
        v1.append(reinterpret_cast<const char *>(&h), 4);
        v1.append(char(0)).append(char(0));
        const QVector<PdfPageGeom> old = attachments::decodePageGeom(v1);
        QCOMPARE(old.size(), 3);
        QCOMPARE(old[2].w, 100.f);
        QCOMPARE(old[2].flags, quint16(0));
        QVERIFY(attachments::decodePageGeom(QByteArray("\x02junk", 5)).isEmpty());
        QVERIFY(attachments::decodePageGeom(QByteArray()).isEmpty());
    }

    void fileMarkdownRoundTripsAndIsNotAnImage()
    {
        const QString h = fakeHash('a');
        QString hash, name;
        const QString md = attachments::fileMarkdown(h, QStringLiteral("Report [final].pdf"));
        QCOMPARE(md, QStringLiteral("[Report \\[final\\].pdf](attachment:") + h + QLatin1Char(')'));
        QVERIFY(attachments::parseFileMarkdown(md, &hash, &name));
        QCOMPARE(hash, h);
        QCOMPARE(name, QStringLiteral("Report [final].pdf"));
        QVERIFY(!attachments::parseImageMarkdown(md, nullptr, nullptr));
        QVERIFY(!attachments::parseFileMarkdown(attachments::imageMarkdown(h, QStringLiteral("x")), nullptr, nullptr));
        QVERIFY(attachments::parseAnyMarkdown(md, &hash));
        QVERIFY(attachments::parseAnyMarkdown(attachments::imageMarkdown(h, QString()), &hash));
        QVERIFY(!attachments::parseFileMarkdown(QStringLiteral("[x](https://example.org/a.pdf)"), nullptr, nullptr));
        // The editor turns either form into its object line.
        QTextCharFormat f;
        QVERIFY(PageSession::attachmentFormatFor(md, &f));
        QCOMPARE(f.objectType(), kPdfObjectType);
        QVERIFY(PageSession::attachmentFormatFor(attachments::imageMarkdown(h, QString()), &f));
        QCOMPARE(f.objectType(), kImageObjectType);
    }

    // -------------------------------------------------------- platform ----
    void pdfInfoReadsCropBoxAndRotation()
    {
        if (!cg())
            QSKIP("PDF needs the CoreGraphics backend (QtPdf is step 5)");
        const QString p = writePdf(handWrittenPdf());
        QVERIFY(platform::looksLikePdf(p));
        const platform::PdfInfo i = platform::pdfInfo(p);
        QVERIFY2(i.ok, qPrintable(i.error));
        QCOMPARE(i.pages, 2);
        QCOMPARE(i.geom[0].rotation, 90);
        QCOMPARE(i.geom[0].w, 100.f); // after rotation
        QCOMPARE(i.geom[0].h, 200.f);
        QCOMPARE(i.geom[1].rotation, 0);
        QCOMPARE(i.geom[1].w, 200.f); // the crop box, not the media box
        QCOMPARE(i.geom[1].h, 300.f);
        QCOMPARE(i.geom[1].flags, quint16(0));
        // Not a PDF.
        const QString txt = fresh(QStringLiteral("x.txt"));
        QFile f(txt);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("hello");
        f.close();
        QVERIFY(!platform::looksLikePdf(txt));
        QVERIFY(!platform::pdfInfo(txt).ok);
    }

    void costHintsFindImagesAndLayers()
    {
        if (!cg())
            QSKIP("CoreGraphics only");
        const QString scan = fresh(QStringLiteral("scan.pdf"));
        const QString tr = fresh(QStringLiteral("tr.pdf"));
        const QString text = fresh(QStringLiteral("text.pdf"));
        QVERIFY(platform::writeFixturePdf(scan, QStringLiteral("scanned"), 1));
        QVERIFY(platform::writeFixturePdf(tr, QStringLiteral("transparency"), 1));
        QVERIFY(platform::writeFixturePdf(text, QStringLiteral("text"), 2));
        const PdfPageGeom s = platform::pdfInfo(scan).geom.value(0);
        QCOMPARE(s.imagePixels, quint32(2550 * 3300));
        QVERIFY(s.flags & PdfPageGeom::ReducibleImage); // a JPEG scan
        QVERIFY(platform::pdfInfo(tr).geom.value(0).flags & PdfPageGeom::Layers);
        const PdfPageGeom t = platform::pdfInfo(text).geom.value(0);
        QCOMPARE(t.imagePixels, 0u);
        QCOMPARE(t.flags, quint16(0));
        QVERIFY(!t.heavy());
        const QString vec = fresh(QStringLiteral("vec.pdf"));
        QVERIFY(platform::writeFixturePdf(vec, QStringLiteral("vector"), 1));
        const PdfPageGeom vg = platform::pdfInfo(vec).geom.value(0);
        QVERIFY(vg.flags & PdfPageGeom::HeavyContent); // 3000 curves: a long content stream
        QVERIFY(vg.heavy() && s.heavy());
        // The estimate follows: a scan at full density costs its decoded
        // image, at thumbnail size a reduced decode; layers cost more than
        // plain pages for the same bitmap.
        const qint64 tile = MediaService::pdfEstimate(s, 2000, 512, 512);
        QVERIFY(tile > 4ll * 2550 * 3300 * 2);
        QVERIFY(MediaService::pdfEstimate(s, 300, 300, 388) < tile / 4);
        PdfPageGeom layers = t;
        layers.flags = PdfPageGeom::Layers;
        QVERIFY(MediaService::pdfEstimate(layers, 2000, 512, 512) > 4 * MediaService::pdfEstimate(t, 2000, 512, 512) / 2);
    }

    void renderAppliesRotationCropAndTiles()
    {
        if (!cg())
            QSKIP("CoreGraphics only");
        const QString p = writePdf(handWrittenPdf());
        platform::PdfDocument doc(p);
        QString err;
        // Page 1, rotated 90° clockwise: the red corner is now top-right.
        QImage a = platform::renderPdfPage(&doc, 0, 100, QRect(0, 0, 100, 200), &err);
        QVERIFY2(!a.isNull(), qPrintable(err));
        QCOMPARE(a.size(), QSize(100, 200));
        QCOMPARE(a.format(), QImage::Format_RGB32);
        QVERIFY(isRed(a.pixel(95, 5)));
        QVERIFY(isWhite(a.pixel(5, 5)));
        QVERIFY(isWhite(a.pixel(95, 195)));
        // Page 2: only the crop box, the blue corner at top-left.
        QImage b = platform::renderPdfPage(&doc, 1, 400, QRect(0, 0, 400, 600), &err);
        QVERIFY(isBlue(b.pixel(10, 10)));
        QVERIFY(isWhite(b.pixel(390, 590)));
        // A tile is its region of the page and nothing more: same pixels as
        // the whole page there, in a tile-sized bitmap.
        const QString mixed = fresh(QStringLiteral("mixed.pdf"));
        QVERIFY(platform::writeFixturePdf(mixed, QStringLiteral("mixed"), 2));
        platform::PdfDocument md(mixed);
        const QImage whole = platform::renderPdfPage(&md, 0, 1224, QRect(0, 0, 1224, 1584));
        QVERIFY(isRed(whole.pixel(10, 10)));
        QVERIFY(isBlue(whole.pixel(1214, 1574)));
        const QRect region(1024, 1024, 200, 560);
        const QImage tile = platform::renderPdfPage(&md, 0, 1224, region);
        QCOMPARE(tile.size(), region.size());
        QVERIFY(isBlue(tile.pixel(190, 550)));
        int differing = 0;
        for (int y = 0; y < tile.height(); y += 3)
            for (int x = 0; x < tile.width(); x += 3) {
                const QRgb u = tile.pixel(x, y), v = whole.pixel(region.x() + x, region.y() + y);
                differing += (qAbs(qRed(u) - qRed(v)) + qAbs(qGreen(u) - qGreen(v)) + qAbs(qBlue(u) - qBlue(v))) > 24 ? 1 : 0;
            }
        QVERIFY2(differing < 20, qPrintable(QString::number(differing)));
        // Errors, not crashes.
        QVERIFY(platform::renderPdfPage(&md, 99, 100, QRect(0, 0, 10, 10)).isNull());
        platform::PdfDocument missing(fresh(QStringLiteral("nope.pdf")));
        QVERIFY(platform::renderPdfPage(&missing, 0, 100, QRect(0, 0, 10, 10), &err).isNull());
        QVERIFY(!err.isEmpty());
    }

    // ---------------------------------------------------- store and GC ----
    void importRecordsPagesAndGeometry()
    {
        if (!cg())
            QSKIP("CoreGraphics only");
        Rig rig(fresh(QStringLiteral("ws.db")), fresh(QStringLiteral("att")), false);
        const QString src = fresh(QStringLiteral("mixed.pdf"));
        QVERIFY(platform::writeFixturePdf(src, QStringLiteral("mixed"), 5));
        AttachmentStore::Source s;
        s.path = src;
        s.name = QStringLiteral("mixed.pdf");
        const AttachmentStore::ImportResult r = rig.importSync(s);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(r.info.isPdf());
        QCOMPARE(r.info.pages, 5);
        QCOMPARE(QSize(r.info.width, r.info.height), QSize(612, 792)); // first page, points
        const QVector<PdfPageGeom> g = attachments::decodePageGeom(r.info.pageGeom);
        QCOMPARE(g.size(), 5);
        QCOMPARE(QSizeF(g[2].w, g[2].h), QSizeF(792, 612)); // landscape
        QCOMPARE(QSizeF(g[3].w, g[3].h), QSizeF(226, 600)); // the receipt
        QVERIFY(QFile::exists(rig.as->pathFor(r.info.hash)));
        // Stored with the row: a new launch reads the same geometry back.
        QVERIFY(rig.store.flush());
        QVector<AttachmentInfo> rows;
        bool done = false;
        QObject ctx;
        rig.store.attachmentInfo({r.info.hash}, &ctx, [&](QVector<AttachmentInfo> got) {
            rows = got;
            done = true;
        });
        QTRY_VERIFY(done);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0].pages, 5);
        QCOMPARE(rows[0].pageGeom, r.info.pageGeom);
        QCOMPARE(rows[0].mime, QStringLiteral("application/pdf"));
        // A broken "PDF" is refused and never referenced.
        const QString bad = fresh(QStringLiteral("bad.pdf"));
        QFile f(bad);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("%PDF-1.4\nthis is not a PDF\n");
        f.close();
        AttachmentStore::Source b;
        b.path = bad;
        const AttachmentStore::ImportResult br = rig.importSync(b);
        QVERIFY(!br.ok);
        QVERIFY(br.error.contains(QStringLiteral("PDF")));
        rig.as->unpin(br.info.hash);
    }

    void pdfLinesRoundTripAndGcKeepsThem()
    {
        if (!cg())
            QSKIP("CoreGraphics only");
        Rig rig(fresh(QStringLiteral("ws.db")), fresh(QStringLiteral("att")), false);
        const QString keep = fresh(QStringLiteral("keep.pdf")), drop = fresh(QStringLiteral("drop.pdf"));
        QVERIFY(platform::writeFixturePdf(keep, QStringLiteral("text"), 1));
        QVERIFY(platform::writeFixturePdf(drop, QStringLiteral("text"), 2));
        AttachmentStore::Source s;
        s.path = keep;
        const AttachmentInfo kept = rig.importSync(s).info;
        s.path = drop;
        const AttachmentInfo dropped = rig.importSync(s).info;
        rig.as->unpin(kept.hash);
        rig.as->unpin(dropped.hash);
        const PageId p = rig.store.createPage(0, -1, QStringLiteral("Docs"));
        {
            PageSession ses(p, &rig.ctx, [&rig] { return rig.alloc(); });
            ses.initNew(0, QStringLiteral("Docs"));
            QTextCursor c(ses.document());
            c.insertText(QStringLiteral("see"));
            Edit e;
            e.op = Edit::Op::InsertBlock;
            e.block = PageSession::dataOf(ses.document()->firstBlock())->id;
            e.record.kind = BlockKind::Text;
            e.record.text = attachments::fileMarkdown(kept.hash, QStringLiteral("keep.pdf"));
            QVERIFY(ses.applyEdit(e) > 0);
            const QTextBlock line = ses.document()->findBlockByNumber(1);
            QCOMPARE(PageSession::roleOf(line), LineRole::Pdf);
            QCOMPARE(PageSession::attachmentMarkdown(line), e.record.text);
            QVERIFY(rig.as->isPinned(kept.hash));
            rig.store.commit(ses.takeSaveBatch());
            QVERIFY(rig.store.flush());
            PageContent pc;
            bool done = false;
            QObject ctx;
            rig.store.openPage(p, &ctx, [&](PageContent got) {
                pc = std::move(got);
                done = true;
            });
            QTRY_VERIFY(done);
            QCOMPARE(pc.blocks.size(), 2);
            QCOMPARE(pc.blocks[1].text, e.record.text);
        }
        QVERIFY(!rig.as->isPinned(kept.hash));
        // The sweep keeps what a page references (the `[name](attachment:…)`
        // form too), and deletes what nothing references.
        int deleted = -1;
        rig.as->collectGarbage([&](int n) { deleted = n; });
        QTRY_VERIFY(deleted >= 0);
        QVERIFY(QFile::exists(rig.as->pathFor(kept.hash)));
        QVERIFY(!QFile::exists(rig.as->pathFor(dropped.hash)));
    }

    // ------------------------------------------------------ note card ----
    void cardHeightIsFixedAndThumbFollowsFirstPage()
    {
        Rig rig(fresh(QStringLiteral("ws.db")), fresh(QStringLiteral("att")), true, instant());
        const QString portrait = fakeHash('1'), landscape = fakeHash('2');
        rig.as->remember(pdfInfoFor(portrait, letterPages(3)));
        PdfPageGeom l;
        l.w = 792;
        l.h = 612;
        rig.as->remember(pdfInfoFor(landscape, {l}));
        PageContent pc;
        pc.id = 1;
        pc.found = true;
        for (const QString &t : {QStringLiteral("above"), attachments::fileMarkdown(portrait, QStringLiteral("a.pdf")),
                                 attachments::fileMarkdown(landscape, QStringLiteral("b.pdf")), QStringLiteral("below")}) {
            BlockRecord r;
            r.id = rig.alloc();
            r.text = t;
            pc.blocks << r;
        }
        PageSession ses(1, &rig.ctx, [&rig] { return rig.alloc(); });
        ses.load(pc);
        QCOMPARE(PageSession::roleOf(ses.document()->findBlockByNumber(1)), LineRole::Pdf);
        ses.document()->setPageSize(QSizeF(800, -1));
        VirtualLayout *vl = ses.layout();
        vl->relayoutAll(); // estimates only
        const qreal estimated = vl->documentSize().height();
        const QRectF a = vl->imageRect(ses.document()->findBlockByNumber(1));
        const QRectF b = vl->imageRect(ses.document()->findBlockByNumber(2));
        QCOMPARE(a.height(), b.height()); // a fixed card
        QCOMPARE(vl->documentSize().height(), estimated); // the estimate was exact
        const QRectF ta = vl->pdfThumbRect(a, portrait), tb = vl->pdfThumbRect(b, landscape);
        QVERIFY(qAbs(ta.width() / ta.height() - 612.0 / 792) < 0.02);
        QVERIFY(qAbs(tb.width() / tb.height() - 792.0 / 612) < 0.02);
    }

    void thumbnailIsRenderedOnThePdfLane()
    {
        if (!cg())
            QSKIP("CoreGraphics only");
        Rig rig(fresh(QStringLiteral("ws.db")), fresh(QStringLiteral("att")), true); // real renderer
        const QString src = fresh(QStringLiteral("mixed.pdf"));
        QVERIFY(platform::writeFixturePdf(src, QStringLiteral("mixed"), 2));
        AttachmentStore::Source s;
        s.path = src;
        const AttachmentInfo info = rig.importSync(s).info;
        QVERIFY(info.isPdf());
        const PageId p = rig.store.createPage(0, -1, QStringLiteral("T"));
        PageSession ses(p, &rig.ctx, [&rig] { return rig.alloc(); });
        ses.initNew(0, QStringLiteral("T"));
        NoteEditor ed(&rig.ctx);
        ed.resize(800, 600);
        ed.show();
        ed.setSession(&ses);
        QSignalSpy opened(&ed, &NoteEditor::openPdfRequested);
        Edit e;
        e.op = Edit::Op::ReplaceBlockText;
        e.block = PageSession::dataOf(ses.document()->firstBlock())->id;
        e.text = attachments::fileMarkdown(info.hash, QStringLiteral("mixed.pdf"));
        ses.applyEdit(e);
        const qint64 renders = counter(instr::PdfRenders);
        ed.viewport()->repaint();
        QTRY_COMPARE(rig.media->cacheEntries(), 1);
        rig.media->waitIdle();
        QCOMPARE(counter(instr::PdfRenders) - renders, qint64(1));
        const QTextBlock line = ses.document()->firstBlock();
        BlockData *d = PageSession::dataOf(line);
        ed.viewport()->repaint();
        QCOMPARE(d->mediaKey.kind, quint8(MediaKey::PdfThumb));
        const QImage *thumb = rig.media->lookup(d->mediaKey);
        QVERIFY(thumb);
        QVERIFY(isRed(thumb->pixel(1, 1))); // the first page's top-left marker
        QVERIFY(rig.media->budget().reserved() == 0);
        // Clicking the card asks to open the viewer.
        const QRectF card = ses.layout()->imageRect(line);
        const QPoint at = (card.center() - QPointF(ed.horizontalScrollBar()->value(), ed.verticalScrollBar()->value())).toPoint();
        QTest::mouseClick(ed.viewport(), Qt::LeftButton, {}, at);
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened[0][0].toString(), info.hash);
        QCOMPARE(opened[0][1].toString(), QStringLiteral("mixed.pdf"));
        ed.setSession(nullptr);
    }

    void droppingAPdfFileImportsIt()
    {
        if (!cg())
            QSKIP("CoreGraphics only");
        Rig rig(fresh(QStringLiteral("ws.db")), fresh(QStringLiteral("att")), true, instant());
        const QString dir = fresh(QStringLiteral("drop"));
        QVERIFY(QDir().mkpath(dir));
        const QString src = dir + QStringLiteral("/Quarterly Report.pdf");
        QVERIFY(platform::writeFixturePdf(src, QStringLiteral("text"), 3));
        const PageId p = rig.store.createPage(0, -1, QStringLiteral("Drop"));
        PageSession ses(p, &rig.ctx, [&rig] { return rig.alloc(); });
        ses.initNew(0, QStringLiteral("Drop"));
        NoteEditor ed(&rig.ctx);
        ed.resize(700, 500);
        ed.show();
        ed.setSession(&ses);
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(src)});
        const QList<AttachmentStore::Source> srcs = NoteEditor::imageSources(&mime);
        QCOMPARE(srcs.size(), 1);
        QCOMPARE(srcs[0].name, QStringLiteral("Quarterly Report.pdf")); // the file name, as shown
        QVERIFY(ed.importImages(srcs));
        rig.as->waitForImports();
        QTRY_VERIFY(!ses.hasPendingImports());
        const QTextBlock line = ses.document()->firstBlock(); // took over the empty line
        QCOMPARE(PageSession::roleOf(line), LineRole::Pdf);
        QCOMPARE(PageSession::attachmentMarkdown(line),
                 attachments::fileMarkdown(PageSession::imageHash(line), QStringLiteral("Quarterly Report.pdf")));
        const AttachmentInfo *info = rig.as->info(PageSession::imageHash(line));
        QVERIFY(info);
        QCOMPARE(info->pages, 3);
        ed.setSession(nullptr);
    }

    // ---------------------------------------------------------- viewer ----
    void viewerLaysOutPagesFromPrefixSums()
    {
        EditorContext ctx; // no media: geometry only
        QVector<PdfPageGeom> pages;
        for (int i = 0; i < 500; ++i) {
            PdfPageGeom g;
            g.w = i % 7 == 3 ? 792 : 612;
            g.h = i % 7 == 3 ? 612 : (i % 11 == 5 ? 300 : 792);
            pages << g;
        }
        PdfView v(&ctx, pdfInfoFor(fakeHash('3'), pages), QString());
        v.resize(900, 700);
        v.setZoom(0.75);
        QCOMPARE(v.pageCount(), 500);
        double sum = 0;
        for (int i = 0; i < 500; ++i) {
            QVERIFY(qAbs(v.pageTop(i) - (v.config().margin + 0.75 * sum + v.config().gap * i)) < 1e-6);
            sum += pages[i].h;
        }
        for (int i : {0, 1, 3, 5, 77, 250, 499}) {
            QCOMPARE(v.pageAt(v.pageTop(i)), i);
            QCOMPARE(v.pageAt(v.pageTop(i) + 1), i);
            if (i > 0)
                QCOMPARE(v.pageAt(v.pageTop(i) - 0.5), i - 1); // in the gap above: still the previous page
        }
        QCOMPARE(v.pageAt(-100), 0);
        QCOMPARE(v.pageAt(1e9), 499);
        QVERIFY(qAbs(v.contentHeight() - (v.pageTop(499) + 0.75 * pages[499].h + v.config().margin)) < 1e-6);
        QCOMPARE(v.pageRect(3).size(), QSizeF(792 * 0.75, 612 * 0.75));
    }

    void viewerPlansWholePagesThenBoundedTiles()
    {
        auto g = std::make_shared<Gate>();
        g->release();
        Rig rig(fresh(QStringLiteral("ws.db")), fresh(QStringLiteral("att")), true, instant());
        const QString h = fakeHash('4');
        PdfView v(&rig.ctx, pdfInfoFor(h, letterPages(20)), QStringLiteral("/nowhere.pdf"));
        v.resize(1000, 800);
        v.show();
        QVERIFY(QTest::qWaitForWindowExposed(&v));
        const qreal dpr = v.devicePixelRatioUsed();
        // Small: one bitmap per page.
        PdfView::Plan small = v.planFor(0, 0.5);
        QVERIFY(!small.tiles);
        QCOMPARE(small.width, qRound(612 * 0.5 * dpr));
        // Past the whole-page threshold: tiles, never a whole-page bitmap.
        PdfView::Plan big = v.planFor(0, 4.0);
        QVERIFY(big.tiles);
        QVERIFY(4ll * big.width * big.height > v.config().wholePageMaxBytes);
        v.setZoom(4.0);
        v.viewport()->repaint();
        const QVector<MediaService::Request> wants = v.lastWants();
        QVERIFY(!wants.isEmpty());
        const int t = v.config().tileSize;
        int visibleTiles = 0;
        for (const MediaService::Request &r : wants) {
            QCOMPARE(r.lane, MediaService::Lane::Pdf);
            QCOMPARE(r.key.kind, quint8(MediaKey::PdfTile));
            QVERIFY(r.key.width <= t && r.key.height <= t); // bounded in both dimensions
            QCOMPARE(r.region.size(), QSize(r.key.width, r.key.height));
            QVERIFY(r.pdf); // the viewer's shared document
            QVERIFY(r.estimate > 0);
            visibleTiles += r.priority == 0 ? 1 : 0;
        }
        // Only what covers the viewport (plus one viewport of prefetch).
        const int across = (v.viewport()->width() * qCeil(dpr)) / t + 2;
        const int down = (v.viewport()->height() * qCeil(dpr)) / t + 2;
        QVERIFY2(visibleTiles <= across * down, qPrintable(QStringLiteral("%1").arg(visibleTiles)));
        QVERIFY(wants.size() <= 2 * across * down);
        rig.media->waitIdle();
        QTRY_VERIFY(counter(instr::PdfTiles) > 0);
    }

    void zoomGestureDrawsCachedPixelsAndRendersOnceSettled()
    {
        auto g = std::make_shared<Gate>();
        Rig rig(fresh(QStringLiteral("ws.db")), fresh(QStringLiteral("att")), true, gated(g));
        const QString h = fakeHash('5');
        PdfView::Config cfg;
        cfg.zoomSettleMs = 80;
        PdfView v(&rig.ctx, pdfInfoFor(h, letterPages(30)), QStringLiteral("/nowhere.pdf"), cfg);
        v.resize(900, 700);
        v.show();
        QVERIFY(QTest::qWaitForWindowExposed(&v));
        v.setZoom(1.0);
        v.viewport()->repaint();
        QVERIFY(!v.lastWants().isEmpty());
        QTRY_COMPARE(g->count(), 1); // the first job started; the rest queue behind it
        const qint64 dropped = counter(instr::MediaJobsDroppedQueued);
        const int queued = rig.media->queuedJobs();
        QVERIFY(queued > 0);
        // A pinch: nothing new is asked for while it moves, and what was
        // queued is dropped before it starts.
        for (int i = 0; i < 5; ++i) {
            v.zoomGesture(1.15);
            v.viewport()->repaint();
            QVERIFY(v.zooming());
            QVERIFY(v.lastWants().isEmpty());
        }
        QCOMPARE(rig.media->queuedJobs(), 0);
        QCOMPARE(counter(instr::MediaJobsDroppedQueued) - dropped, qint64(queued));
        g->release();
        rig.media->waitIdle();
        QCOMPARE(g->count(), 1); // nothing ran during the gesture but the job already started
        // Settled: the new zoom is requested (and rendered) once.
        QTRY_VERIFY(!v.zooming());
        v.viewport()->repaint();
        QVERIFY(!v.lastWants().isEmpty());
        const int expectedLevel = v.planFor(v.currentPage(), v.zoom()).width;
        for (const MediaService::Request &r : v.lastWants())
            QVERIFY(r.key.kind == MediaKey::PdfPage ? r.key.width > 0 : r.key.level == expectedLevel || r.priority == 1);
        rig.media->waitIdle();
        QVERIFY(g->count() > 1);
    }

    void scrollingDropsStaleRequestsBeforeTheyStart()
    {
        auto g = std::make_shared<Gate>();
        Rig rig(fresh(QStringLiteral("ws.db")), fresh(QStringLiteral("att")), true, gated(g));
        const QString h = fakeHash('6');
        PdfView v(&rig.ctx, pdfInfoFor(h, letterPages(200)), QStringLiteral("/nowhere.pdf"));
        v.resize(900, 700);
        v.show();
        QVERIFY(QTest::qWaitForWindowExposed(&v));
        v.setZoom(1.0);
        v.viewport()->repaint();
        QTRY_COMPARE(g->count(), 1);
        const qint64 dropped = counter(instr::MediaJobsDroppedQueued);
        QSet<int> wantedPagesBefore;
        for (const MediaService::Request &r : v.lastWants())
            wantedPagesBefore.insert(r.key.page);
        // Jump far down: the old pages' queued jobs go before starting.
        v.goToPage(150);
        v.viewport()->repaint();
        QVERIFY(counter(instr::MediaJobsDroppedQueued) > dropped);
        for (const MediaService::Request &r : v.lastWants())
            QVERIFY(!wantedPagesBefore.contains(r.key.page));
        g->release();
        rig.media->waitIdle();
        // Only the first job (already running) ran for the old position.
        int oldStarted = 0;
        for (const MediaKey &k : std::as_const(g->started))
            oldStarted += wantedPagesBefore.contains(k.page) ? 1 : 0;
        QCOMPARE(oldStarted, 1);
    }

    void flingDefersHeavyPagesOnly()
    {
        auto g = std::make_shared<Gate>();
        g->release();
        Rig rig(fresh(QStringLiteral("ws.db")), fresh(QStringLiteral("att")), true, instant());
        // Even pages are scans (heavy), odd pages plain text.
        QVector<PdfPageGeom> pages = letterPages(60);
        for (int i = 0; i < pages.size(); i += 2) {
            pages[i].imagePixels = 2550u * 3300u;
            pages[i].flags = PdfPageGeom::ReducibleImage;
        }
        QVERIFY(pages[0].heavy() && !pages[1].heavy());
        PdfView::Config cfg;
        cfg.flingSettleMs = 120;
        PdfView v(&rig.ctx, pdfInfoFor(fakeHash('b'), pages), QStringLiteral("/nowhere.pdf"), cfg);
        v.resize(800, 700);
        v.show();
        QVERIFY(QTest::qWaitForWindowExposed(&v));
        v.setZoom(0.4); // several pages per viewport
        v.viewport()->repaint();
        QVERIFY(!v.flinging());
        // Fast: a viewport per step.
        for (int i = 0; i < 6; ++i) {
            v.verticalScrollBar()->setValue(v.verticalScrollBar()->value() + v.viewport()->height());
            v.viewport()->repaint();
        }
        QVERIFY(v.flinging());
        QSet<int> wanted;
        for (const MediaService::Request &r : v.lastWants())
            wanted.insert(r.key.page);
        QVERIFY(!wanted.isEmpty());
        for (int p : std::as_const(wanted))
            QVERIFY2(!pages[p].heavy(), qPrintable(QString::number(p))); // only the cheap pages
        // Slowed down: the heavy ones are asked for too.
        QTRY_VERIFY(!v.flinging());
        v.viewport()->repaint();
        bool heavyWanted = false;
        for (const MediaService::Request &r : v.lastWants())
            heavyWanted = heavyWanted || pages[r.key.page].heavy();
        QVERIFY(heavyWanted);
        // Reading-speed scrolling never counts as a fling.
        for (int i = 0; i < 6; ++i) {
            v.verticalScrollBar()->setValue(v.verticalScrollBar()->value() + 15);
            QTest::qWait(25);
        }
        QVERIFY(!v.flinging());
    }

    void documentClosesWhenViewerCloses()
    {
        if (!cg())
            QSKIP("CoreGraphics only");
        Rig rig(fresh(QStringLiteral("ws.db")), fresh(QStringLiteral("att")), true); // real renderer
        const QString src = fresh(QStringLiteral("long.pdf"));
        QVERIFY(platform::writeFixturePdf(src, QStringLiteral("text"), 12));
        AttachmentStore::Source s;
        s.path = src;
        const AttachmentInfo info = rig.importSync(s).info;
        const int before = platform::PdfDocument::liveCount();
        PdfView::Config cfg;
        cfg.closeHiddenAfterMs = 60;
        auto *v = new PdfView(&rig.ctx, info, rig.as->pathFor(info.hash), cfg);
        v->resize(900, 700);
        v->show();
        QVERIFY(QTest::qWaitForWindowExposed(v));
        v->viewport()->repaint();
        QTRY_VERIFY(rig.media->cacheEntries() > 0);
        rig.media->waitIdle();
        QCOMPARE(platform::PdfDocument::liveCount(), before + 1); // one handle for all its jobs
        QVERIFY(v->documentOpen());
        // Hidden a while: the handle closes (and nothing renders meanwhile).
        v->hide();
        QVERIFY(v->lastWants().isEmpty());
        QTRY_VERIFY(!v->documentOpen());
        // (The last job's own reference goes on the worker, just after it
        // delivered its result.)
        QTRY_COMPARE(platform::PdfDocument::liveCount(), before);
        // Shown again: reopened lazily by the next render.
        v->show();
        v->goToPage(8);
        v->viewport()->repaint();
        rig.media->waitIdle();
        QTRY_COMPARE(platform::PdfDocument::liveCount(), before + 1);
        // Closed: gone, with its requests.
        const std::weak_ptr<platform::PdfDocument> handle = v->document();
        v->goToPage(4); // something in flight when it closes
        v->viewport()->repaint();
        delete v;
        rig.media->waitIdle();
        QTRY_VERIFY(handle.expired());
        QTRY_COMPARE(platform::PdfDocument::liveCount(), before);
        QCOMPARE(rig.media->queuedJobs(), 0);
        QCOMPARE(rig.media->cacheEntries(), 0); // a render that finished after closing is not kept
    }

    void viewerAndEditorRepaintSeparately()
    {
        Rig rig(fresh(QStringLiteral("ws.db")), fresh(QStringLiteral("att")), true, instant());
        const QString doc = fakeHash('7'), other = fakeHash('8');
        rig.as->remember(pdfInfoFor(doc, letterPages(4)));
        // A note showing the same PDF's card.
        PageContent pc;
        pc.id = 1;
        pc.found = true;
        BlockRecord r;
        r.id = rig.alloc();
        r.text = attachments::fileMarkdown(doc, QStringLiteral("d.pdf"));
        pc.blocks << r;
        PageSession ses(1, &rig.ctx, [&rig] { return rig.alloc(); });
        ses.load(pc);
        NoteEditor ed(&rig.ctx);
        ed.resize(700, 500);
        ed.show();
        QVERIFY(QTest::qWaitForWindowExposed(&ed));
        ed.setSession(&ses);
        ed.viewport()->repaint();
        QTRY_COMPARE(rig.media->cacheEntries(), 1); // the card's thumbnail
        rig.media->waitIdle();
        PdfView v(&rig.ctx, *rig.as->info(doc), QStringLiteral("/nowhere.pdf"));
        v.resize(800, 600);
        v.show();
        QVERIFY(QTest::qWaitForWindowExposed(&v));
        v.viewport()->repaint();
        QSignalSpy editorUpdates(ses.layout(), &QAbstractTextDocumentLayout::update);
        int viewerResults = 0;
        connect(rig.media.get(), &MediaService::arrived, this, [&](const MediaKey &k) {
            viewerResults += k.kind == MediaKey::PdfPage || k.kind == MediaKey::PdfTile ? 1 : 0;
        });
        // The viewer's pages arriving never touch the note.
        rig.media->waitIdle();
        QTest::qWait(30);
        QVERIFY(viewerResults > 0);
        QCOMPARE(editorUpdates.size(), 0);
        // Typing in the note never repaints the viewer, and results for
        // another document never do either.
        const int paints = v.paintCount();
        QTest::keyClicks(&ed, QStringLiteral("typing"));
        MediaKey foreign;
        foreign.hash = other;
        foreign.kind = MediaKey::PdfPage;
        foreign.width = foreign.height = 10;
        emit rig.media->arrived(foreign);
        QTest::qWait(30);
        QCOMPARE(v.paintCount(), paints);
        ed.setSession(nullptr);
    }

    void viewerTakesTheEditorsPlaceInTheApp()
    {
        if (!cg())
            QSKIP("CoreGraphics only");
        QTemporaryDir home;
        const QString src = fresh(QStringLiteral("app.pdf"));
        QVERIFY(platform::writeFixturePdf(src, QStringLiteral("text"), 6));
        AppController::Options o;
        o.homeDir = home.path();
        auto app = std::make_unique<AppController>(o);
        QString err;
        QVERIFY2(app->start(&err), qPrintable(err));
        app->window()->show();
        QTRY_VERIFY(app->activeSession());
        const int docs = platform::PdfDocument::liveCount();
        NoteEditor *ed = app->editorPane()->editor();
        AttachmentStore::Source s;
        s.path = src;
        s.name = QStringLiteral("app.pdf");
        QVERIFY(ed->importImages({s}));
        app->attachments()->waitForImports();
        QTRY_VERIFY(!app->activeSession()->hasPendingImports());
        QString hash;
        for (QTextBlock b = app->activeSession()->document()->firstBlock(); b.isValid(); b = b.next())
            if (PageSession::roleOf(b) == LineRole::Pdf)
                hash = PageSession::imageHash(b);
        QVERIFY(!hash.isEmpty());
        const QString title = app->window()->windowTitle();
        // The card's click signal opens it inside the main window.
        emit ed->openPdfRequested(hash, QStringLiteral("app.pdf"));
        QVERIFY(app->pdfOpen());
        PdfViewerPage *page = app->pdfViewer();
        QCOMPARE(page->window(), static_cast<QWidget *>(app->window()));
        QVERIFY(!app->editorPane()->isVisible());
        QCOMPARE(app->window()->windowTitle(), QStringLiteral("app.pdf — Folia"));
        QCOMPARE(page->view()->pageCount(), 6);
        QVERIFY(app->attachments()->isPinned(hash));
        QVERIFY(!app->commands()->dispatchCommand(QStringLiteral("editor.undo"), {}).ok);
        page->view()->viewport()->repaint();
        app->media()->waitIdle();
        QTRY_VERIFY(app->media()->cacheEntries() > 1); // the card's thumbnail and the viewer's tiles
        // Esc returns to the note; the viewer and its bitmaps are gone.
        QTest::keyClick(page->view(), Qt::Key_Escape);
        QVERIFY(!app->pdfOpen());
        QVERIFY(!app->pdfViewer());
        QVERIFY(app->editorPane()->isVisible());
        QCOMPARE(app->window()->windowTitle(), title);
        app->media()->waitIdle();
        QCOMPARE(app->media()->cacheEntries(), 1); // the thumbnail stays
        QTRY_COMPARE(platform::PdfDocument::liveCount(), docs);
        // Going to a page closes it; so does the command.
        QVERIFY(app->showPdf(hash, QStringLiteral("app.pdf")));
        QVERIFY(app->commands()->dispatchCommand(QStringLiteral("page.new"), {}).ok);
        QTRY_VERIFY(!app->pdfOpen());
        QVERIFY(app->showPdf(hash, QStringLiteral("app.pdf")));
        QVERIFY(app->commands()->dispatchCommand(QStringLiteral("pdf.close"), {}).ok);
        QVERIFY(!app->pdfOpen());
        // Quitting with the viewer open tears down in order (the viewer
        // calls into the media service and attachment store as it goes).
        QVERIFY(app->showPdf(hash, QStringLiteral("app.pdf")));
        app->pdfViewer()->view()->viewport()->repaint();
        app.reset();
        QTRY_COMPARE(platform::PdfDocument::liveCount(), docs);
    }

    // ------------------------------------------------------- scheduler ----
    void pdfCacheRulesArePerPage()
    {
        MediaService::Config mc;
        mc.budgetBytes = 64ll << 20;
        MediaService m(mc);
        m.setDecoder(instant());
        const QString h = fakeHash('9');
        PdfPageGeom g = letterPages(1).first();
        // Many tiles of one page, and whole pages of others: none evicts the
        // others through the "two resolutions per image" rule.
        QVector<MediaService::Request> wants;
        for (int p = 0; p < 4; ++p)
            wants << MediaService::pdfRequest(h, QString(), MediaKey::PdfPage, p, g, 100, 130);
        for (int t = 0; t < 6; ++t)
            wants << MediaService::pdfRequest(h, QString(), MediaKey::PdfTile, 0, g, 64, 64, 1000, t, 0, QRect(t * 64, 0, 64, 64));
        const qint64 tiles = counter(instr::PdfTiles), renders = counter(instr::PdfRenders);
        m.setWanted(this, wants);
        m.waitIdle();
        QCOMPARE(m.cacheEntries(), 10);
        QCOMPARE(counter(instr::PdfTiles) - tiles, qint64(6));
        QCOMPARE(counter(instr::PdfRenders) - renders, qint64(10));
        // A third resolution of one page drops that page's least recent one.
        m.setWanted(this, {MediaService::pdfRequest(h, QString(), MediaKey::PdfPage, 0, g, 120, 156)});
        m.waitIdle();
        m.setWanted(this, {MediaService::pdfRequest(h, QString(), MediaKey::PdfPage, 0, g, 140, 182)});
        m.waitIdle();
        QCOMPARE(m.cacheEntries(), 10 + 2 - 1);
        // A failed page is that page only; a failed thumbnail is the document.
        m.setDecoder([](const MediaService::Request &, QString *e) {
            *e = QStringLiteral("boom");
            return QImage();
        });
        const MediaService::Request bad = MediaService::pdfRequest(h, QString(), MediaKey::PdfPage, 7, g, 50, 65);
        m.setWanted(this, {bad});
        m.waitIdle();
        QVERIFY(m.failed(bad.key));
        QVERIFY(!m.failed(h));
        const MediaService::Request thumb = MediaService::pdfRequest(h, QString(), MediaKey::PdfThumb, 0, g, 30, 39);
        m.setWanted(this, {thumb});
        m.waitIdle();
        QVERIFY(m.failed(h));
        m.removeOwner(this);
    }
};

QTEST_MAIN(PdfTests)
#include "tst_pdf.moc"
