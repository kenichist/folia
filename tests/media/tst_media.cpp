// Attachments and images (docs/ATTACHMENTS.md steps 1-3): the store and its
// streaming import and GC, the media budget and two-lane scheduler, ImageIO
// decoding, and the image block in the editor (headless).
#include "app/Platform.h"
#include "core/Attachments.h"
#include "core/Instrument.h"
#include "editor/EditorContext.h"
#include "editor/MediaService.h"
#include "editor/NoteEditor.h"
#include "editor/PageSession.h"
#include "editor/VirtualLayout.h"
#include "storage/AttachmentStore.h"
#include "storage/Store.h"

#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QMimeData>
#include <QMutex>
#include <QPainter>
#include <QProcess>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QWaitCondition>

#include <memory>

using namespace folia;

namespace {

QString sha256Of(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    return QString::fromLatin1(h.result().toHex());
}

QImage testPattern(int w, int h, bool alpha = false)
{
    QImage img(w, h, alpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    QPainter p(&img);
    QLinearGradient g(0, 0, w, h);
    g.setColorAt(0, alpha ? QColor(255, 0, 0, 120) : QColor(255, 0, 0));
    g.setColorAt(1, QColor(0, 0, 255));
    p.fillRect(img.rect(), g);
    p.fillRect(QRect(0, 0, w / 3, h / 3), Qt::green); // top-left marker
    return img;
}

// A decoder whose jobs block until the gate opens, recording the order.
struct Gate {
    QMutex mu;
    QWaitCondition cv;
    bool open = false;
    QStringList order;
    int started() { QMutexLocker l(&mu); return int(order.size()); }
    void release()
    {
        QMutexLocker l(&mu);
        open = true;
        cv.wakeAll();
    }
};

MediaService::Decoder gatedDecoder(std::shared_ptr<Gate> g)
{
    return [g](const MediaService::Request &r, QString *) {
        QMutexLocker l(&g->mu);
        g->order << r.key.hash;
        while (!g->open)
            g->cv.wait(&g->mu);
        QImage img(qMax(1, r.key.width), qMax(1, r.key.height), QImage::Format_RGB32);
        img.fill(Qt::gray);
        return img;
    };
}

MediaService::Decoder instantDecoder()
{
    return [](const MediaService::Request &r, QString *) {
        QImage img(qMax(1, r.key.width), qMax(1, r.key.height), QImage::Format_RGB32);
        img.fill(Qt::darkGray);
        return img;
    };
}

MediaService::Request req(const QString &hash, int w, int h, int priority, qint64 estimate = -1,
                          MediaService::Lane lane = MediaService::Lane::Image)
{
    MediaService::Request r;
    r.key.hash = hash;
    r.key.width = w;
    r.key.height = h;
    r.priority = priority;
    r.lane = lane;
    r.estimate = estimate >= 0 ? estimate : qint64(w) * h * 4 * 2;
    return r;
}

qint64 counter(instr::Counter c) { return instr::get(c); }

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

} // namespace

class MediaTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    int m_serial = 0;
    QString fresh(const QString &name) { return m_dir.filePath(QStringLiteral("%1-%2").arg(++m_serial).arg(name)); }

    // A store, an attachment store and an editor context wired like the app.
    struct Rig {
        Store store;
        std::unique_ptr<AttachmentStore> as;
        std::unique_ptr<MediaService> media;
        EditorContext ctx;
        Rig(const QString &db, const QString &dir, bool withMedia, MediaService::Config mc = {})
        {
            Store::Options o;
            o.path = db;
            QString err;
            if (!store.open(o, &err))
                qFatal("store: %s", qPrintable(err));
            AttachmentStore::Options ao;
            ao.dir = dir;
            ao.gcDelayMs = 3600000; // tests sweep explicitly
            ao.startupGcDelayMs = 3600000;
            as = std::make_unique<AttachmentStore>(&store, ao);
            ctx.attachments = as.get();
            if (withMedia) {
                media = std::make_unique<MediaService>(mc);
                media->setDecoder(instantDecoder());
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
            QTest::qWaitFor([&] { return done; }, 10000);
            return out;
        }
        AttachmentInfo remembered(const QString &hash, int w, int h, const QString &mime = QStringLiteral("image/jpeg"))
        {
            AttachmentInfo a;
            a.hash = hash;
            a.width = w;
            a.height = h;
            a.mime = mime;
            as->remember(a);
            return a;
        }
    };

    static QString fakeHash(char c) { return QString(attachments::kHashLength, QLatin1Char(c)); }

private slots:
    void initTestCase() { QVERIFY(m_dir.isValid()); }

    // ------------------------------------------------------------ markdown --
    void imageMarkdownRoundTrips()
    {
        const QString h = fakeHash('a');
        QString hash, alt;
        QVERIFY(attachments::parseImageMarkdown(QStringLiteral("![photo](attachment:") + h + QLatin1Char(')'), &hash, &alt));
        QCOMPARE(hash, h);
        QCOMPARE(alt, QStringLiteral("photo"));
        const QString md = attachments::imageMarkdown(h, QStringLiteral("a [b] \\c"));
        QVERIFY(attachments::parseImageMarkdown(md, &hash, &alt));
        QCOMPARE(alt, QStringLiteral("a [b] \\c"));
        QVERIFY(attachments::parseImageMarkdown(QStringLiteral("![](attachment:") + h + QLatin1Char(')'), &hash, &alt));
        QVERIFY(alt.isEmpty());
        // Not ours: other URLs, bad hashes, trailing text, a link without '!'.
        QVERIFY(!attachments::parseImageMarkdown(QStringLiteral("![x](https://e.org/a.png)"), nullptr, nullptr));
        QVERIFY(!attachments::parseImageMarkdown(QStringLiteral("![x](attachment:") + h.left(63) + QStringLiteral("Z)"), nullptr, nullptr));
        QVERIFY(!attachments::parseImageMarkdown(QStringLiteral("[x](attachment:") + h + QLatin1Char(')'), nullptr, nullptr));
        QVERIFY(!attachments::parseImageMarkdown(QStringLiteral("![x](attachment:") + h + QStringLiteral(") more"), nullptr, nullptr));
    }

    // -------------------------------------------------------------- budget --
    void budgetReservesWaitsAndRefuses()
    {
        MediaBudget b(100);
        QCOMPARE(b.check(101, 0), MediaBudget::Admit::TooLarge);
        b.reserve(60);
        QCOMPARE(b.check(50, 0), MediaBudget::Admit::Wait); // in flight: cannot be evicted
        b.release(60);
        QCOMPARE(b.reserved(), qint64(0));
        b.addCached(80);
        QCOMPARE(b.check(50, 0), MediaBudget::Admit::Wait);
        QCOMPARE(b.check(50, 80), MediaBudget::Admit::Run); // fits once caches give way
        QCOMPARE(b.overBy(50), qint64(30));
        b.lower(0.5); // pressure cool-down
        QCOMPARE(b.limit(), qint64(50));
        QVERIFY(b.coolingDown());
        QCOMPARE(b.check(60, 80), MediaBudget::Admit::Wait); // fits the base: waits, not refused
        QCOMPARE(b.check(40, 80), MediaBudget::Admit::Run);
        b.restore();
        QCOMPARE(b.limit(), qint64(100));
    }

    // ----------------------------------------------------------- scheduler --
    void schedulerOnePendingDecodePerKey()
    {
        auto g = std::make_shared<Gate>();
        MediaService m;
        m.setDecoder(gatedDecoder(g));
        int a = 0, b = 0, c = 0;
        const qint64 decodes = counter(instr::ImageDecodes);
        m.setWanted(&a, {req(QStringLiteral("x"), 10, 10, 0)});
        m.setWanted(&b, {req(QStringLiteral("x"), 10, 10, 0)}); // another block, same bitmap
        QTRY_COMPARE(g->started(), 1);
        QCOMPARE(m.queuedJobs() + m.runningJobs(), 1);
        g->release();
        MediaKey k = req(QStringLiteral("x"), 10, 10, 0).key;
        QTRY_VERIFY(m.contains(k));
        QCOMPARE(counter(instr::ImageDecodes) - decodes, qint64(1));
        const qint64 hits = counter(instr::ImageCacheHits);
        m.setWanted(&c, {req(QStringLiteral("x"), 10, 10, 0)});
        QCOMPARE(counter(instr::ImageCacheHits) - hits, qint64(1));
        QCOMPARE(g->started(), 1);
    }

    void schedulerVisibleFirstAndDropsStaleBeforeStart()
    {
        auto g = std::make_shared<Gate>();
        MediaService m;
        m.setDecoder(gatedDecoder(g));
        int owner = 0;
        // A prefetch request listed first still goes after the visible ones.
        m.setWanted(&owner, {req(QStringLiteral("ahead"), 8, 8, 1), req(QStringLiteral("a"), 8, 8, 0),
                             req(QStringLiteral("b"), 8, 8, 0)});
        QTRY_COMPARE(g->started(), 1);
        QCOMPARE(g->order.first(), QStringLiteral("a"));
        // Scrolled away: "b" and "ahead" were never started -> dropped; "a"
        // is running and cannot be interrupted.
        const qint64 dropped = counter(instr::MediaJobsDroppedQueued);
        const qint64 discarded = counter(instr::MediaJobsDiscarded);
        m.setWanted(&owner, {req(QStringLiteral("c"), 8, 8, 0)});
        QCOMPARE(counter(instr::MediaJobsDroppedQueued) - dropped, qint64(2));
        g->release();
        QTRY_VERIFY(m.contains(req(QStringLiteral("c"), 8, 8, 0).key));
        QCOMPARE(counter(instr::MediaJobsDiscarded) - discarded, qint64(1)); // "a": finished, unwanted
        QVERIFY(!g->order.contains(QStringLiteral("b")));
        QVERIFY(!g->order.contains(QStringLiteral("ahead")));
    }

    void schedulerWaitsForBudgetAndRefusesTooLarge()
    {
        auto g = std::make_shared<Gate>();
        MediaService::Config cfg;
        cfg.budgetBytes = 1000;
        MediaService m(cfg);
        m.setDecoder(gatedDecoder(g));
        int owner = 0;
        const qint64 waits = counter(instr::MediaBudgetWaits);
        // Two lanes: the PDF lane's job would run beside the image decode,
        // but both reservations do not fit, so it waits.
        m.setWanted(&owner, {req(QStringLiteral("img"), 1, 1, 0, 600),
                             req(QStringLiteral("pdf"), 1, 1, 0, 600, MediaService::Lane::Pdf)});
        QTRY_COMPARE(g->started(), 1);
        QCOMPARE(m.budget().reserved(), qint64(600));
        QCOMPARE(counter(instr::MediaBudgetWaits) - waits, qint64(1));
        g->release();
        QTRY_COMPARE(g->started(), 2);
        QTRY_COMPARE(m.runningJobs(), 0);
        QCOMPARE(m.budget().reserved(), qint64(0));
        // Larger than the whole budget: refused, the block says so.
        const qint64 tooLarge = counter(instr::MediaTooLarge);
        MediaKey refused;
        connect(&m, &MediaService::refused, this, [&](const MediaKey &k) { refused = k; });
        const MediaService::Request big = req(QStringLiteral("huge"), 1, 1, 0, 2000);
        m.setWanted(&owner, {big});
        QCOMPARE(refused, big.key);
        QVERIFY(m.tooLarge(big.key));
        QCOMPARE(counter(instr::MediaTooLarge) - tooLarge, qint64(1));
        QCOMPARE(g->started(), 2);
    }

    void prefetchOnlyWhenIdleAndUnderHalfBudget()
    {
        MediaService::Config cfg;
        cfg.budgetBytes = 1000;
        MediaService m(cfg);
        m.setDecoder(instantDecoder());
        int owner = 0;
        const MediaService::Request big = req(QStringLiteral("big"), 150, 1, 0, 600); // 600 B bitmap
        m.setWanted(&owner, {big});
        QTRY_VERIFY(m.contains(big.key));
        QCOMPARE(m.cacheBytes(), qint64(600));
        const MediaService::Request ahead = req(QStringLiteral("ahead"), 10, 1, 1, 40);
        m.setWanted(&owner, {big, ahead});
        QTest::qWait(50);
        QVERIFY(!m.contains(ahead.key)); // over half the budget: no prefetch
        QCOMPARE(m.queuedJobs(), 1);
        m.trim(0);
        m.setWanted(&owner, {big, ahead}); // "big" is visible again and wins
        QTRY_VERIFY(m.contains(big.key));
        m.setWanted(&owner, {ahead});
        m.trim(0);
        m.setWanted(&owner, {ahead});
        QTRY_VERIFY(m.contains(ahead.key));
    }

    void cacheCapEvictsLeastRecentButNeverWhatIsShown()
    {
        MediaService::Config cfg;
        cfg.budgetBytes = 4000;
        cfg.cacheMaxBytes = 1000; // cached pixels; the rest is headroom for work in flight
        MediaService m(cfg);
        m.setDecoder(instantDecoder());
        int owner = 0;
        auto r = [](const char *name) { return req(QString::fromLatin1(name), 100, 1, 0, 400); }; // 400 B bitmaps
        // Shown one after another: the cap keeps the two most recent.
        for (const char *n : {"a", "b", "c"}) {
            m.setWanted(&owner, {r(n)});
            QTRY_VERIFY(m.contains(r(n).key));
        }
        QCOMPARE(m.cacheEntries(), 2);
        QVERIFY(!m.contains(r("a").key));
        QCOMPARE(m.cacheBytes(), qint64(800));
        // A visible set over the cap stays whole: what is on screen is not evicted.
        m.setWanted(&owner, {r("d"), r("e"), r("f")});
        QTRY_VERIFY(m.contains(r("d").key) && m.contains(r("e").key) && m.contains(r("f").key));
        QCOMPARE(m.cacheBytes(), qint64(1200));
        QVERIFY(!m.contains(r("b").key) && !m.contains(r("c").key));
        // Work whose estimate is over the cap but within the budget still runs.
        const MediaService::Request big = req(QStringLiteral("big"), 100, 1, 0, 3000);
        m.setWanted(&owner, {big});
        QTRY_VERIFY(m.contains(big.key));
        QVERIFY(m.cacheBytes() <= 1000);
    }

    void memoryPressureTrimsAndCoolsDown()
    {
        MediaService::Config cfg;
        cfg.budgetBytes = 4000;
        cfg.coolDownMs = 150;
        cfg.warningFloorBytes = 100;
        MediaService m(cfg);
        m.setDecoder(instantDecoder());
        int owner = 0;
        m.setWanted(&owner, {req(QStringLiteral("a"), 100, 1, 0, 400), req(QStringLiteral("b"), 100, 1, 0, 400)});
        QTRY_COMPARE(m.cacheEntries(), 2);
        m.memoryPressure(false); // warning: toward the floor, budget halved
        QVERIFY(m.cacheBytes() <= 100);
        QCOMPARE(m.budget().limit(), qint64(2000));
        m.memoryPressure(true); // critical: empty, a quarter
        QCOMPARE(m.cacheBytes(), qint64(0));
        QCOMPARE(m.budget().limit(), qint64(1000));
        QTRY_COMPARE_WITH_TIMEOUT(m.budget().limit(), qint64(4000), 3000); // cool-down over
    }

    void coolDownShrinksRequestsInsteadOfStalling()
    {
        MediaService::Config cfg;
        cfg.coolDownMs = 150;
        MediaService m(cfg);
        AttachmentInfo ai;
        ai.hash = fakeHash('7');
        ai.mime = QStringLiteral("image/jpeg");
        ai.width = 4032;
        ai.height = 3024;
        const MediaService::Request normal = m.imageRequest(ai, QString(), 760, 2.0);
        QCOMPARE(normal.key.width, 1520);
        int changed = 0;
        connect(&m, &MediaService::budgetChanged, this, [&] { ++changed; });
        m.memoryPressure(true);
        QCOMPARE(changed, 1);
        // A lower density that fits the lowered budget, so it can run now.
        const MediaService::Request low = m.imageRequest(ai, QString(), 760, 2.0);
        QVERIFY(low.key.width < normal.key.width);
        QVERIFY(low.estimate <= m.budget().limit() * MediaService::kMaxDecodeShare);
        QCOMPARE(m.budget().check(low.estimate, 0), MediaBudget::Admit::Run);
        QVERIFY(m.budget().check(normal.estimate, 0) != MediaBudget::Admit::Run);
        QTRY_COMPARE_WITH_TIMEOUT(changed, 2, 3000); // cool-down over: full density again
        QCOMPARE(m.imageRequest(ai, QString(), 760, 2.0).key, normal.key);
    }

    void evictionReturnsMemory()
    {
        // Decoded bitmaps live in their own mappings: trimming the cache
        // lowers the footprint (freed malloc blocks of this size would not).
        MediaService m;
        m.setDecoder([](const MediaService::Request &r, QString *) {
            QImage img = platform::allocateImage(r.key.width, r.key.height, QImage::Format_RGB32);
            img.fill(Qt::red); // touch every page
            return img;
        });
        int owner = 0;
        QVector<MediaService::Request> wants;
        for (int i = 0; i < 8; ++i)
            wants << req(QStringLiteral("mem%1").arg(i), 1520, 1140, 0, 8 << 20);
        const qint64 before = platform::sampleSelf().footprintBytes;
        m.setWanted(&owner, wants);
        QTRY_COMPARE_WITH_TIMEOUT(m.cacheEntries(), 8, 10000);
        const qint64 cached = m.cacheBytes();
        QCOMPARE(cached, qint64(8) * 1520 * 1140 * 4);
        const qint64 full = platform::sampleSelf().footprintBytes;
        QVERIFY2(full - before > cached * 9 / 10, qPrintable(QString::number(full - before)));
        m.setWanted(&owner, {});
        m.trim(0);
        const qint64 after = platform::sampleSelf().footprintBytes;
        QVERIFY2(full - after > cached * 9 / 10, qPrintable(QString::number(full - after)));
    }

    void atMostTwoResolutionsPerAttachment()
    {
        MediaService m;
        m.setDecoder(instantDecoder());
        int owner = 0;
        for (int w : {10, 20, 30}) {
            m.setWanted(&owner, {req(QStringLiteral("h"), w, w, 0)});
            QTRY_VERIFY(m.contains(req(QStringLiteral("h"), w, w, 0).key));
        }
        QVERIFY(!m.contains(req(QStringLiteral("h"), 10, 10, 0).key));
        QVERIFY(m.contains(req(QStringLiteral("h"), 20, 20, 0).key));
        MediaKey found;
        QVERIFY(m.nearest(QStringLiteral("h"), 21, 0.10, &found));
        QCOMPARE(found.width, 20);
        QVERIFY(!m.nearest(QStringLiteral("h"), 40, 0.10, nullptr));
        QVERIFY(m.nearest(QStringLiteral("h"), 40, -1, &found));
        QCOMPARE(found.width, 30);
    }

    // ------------------------------------------------------------ decoding --
    void imageIODecodesWithOrientation()
    {
#ifndef Q_OS_MACOS
        QSKIP("ImageIO is macOS only");
#endif
        const QString jpg = fresh(QStringLiteral("rot.jpg"));
        QVERIFY(platform::encodeImage(testPattern(300, 100), jpg, QStringLiteral("public.jpeg"), 0.9, 6));
        const platform::ImageInfo info = platform::imageInfo(jpg);
        QVERIFY2(info.ok, qPrintable(info.error));
        QCOMPARE(info.mime, QStringLiteral("image/jpeg"));
        QCOMPARE(info.orientation, 6);
        QCOMPARE(info.width, 100); // after the orientation transform
        QCOMPARE(info.height, 300);
        QString err;
        const QImage img = platform::decodeImage(jpg, 150, &err);
        QVERIFY2(!img.isNull(), qPrintable(err));
        QCOMPARE(img.size(), QSize(50, 150));
        QCOMPARE(img.format(), QImage::Format_RGB32);
        // Rotated 90° clockwise: the green top-left marker is now top-right.
        const QColor tr = img.pixelColor(img.width() - 5, 5);
        QVERIFY2(tr.green() > 150 && tr.red() < 100, qPrintable(tr.name()));

        const QString png = fresh(QStringLiteral("alpha.png"));
        QVERIFY(testPattern(64, 32, true).save(png));
        const QImage a = platform::decodeImage(png, 1000, &err);
        QCOMPARE(a.size(), QSize(64, 32)); // never upscaled
        QCOMPARE(a.format(), QImage::Format_ARGB32_Premultiplied);
        // The portable fallback agrees.
        const QImage q = platform::decodeImageQt(png, 32, &err);
        QCOMPARE(q.size(), QSize(32, 16));
        QCOMPARE(platform::imageInfoQt(png).width, 64);
    }

    void heicAndWebpDecode()
    {
#ifndef Q_OS_MACOS
        QSKIP("ImageIO is macOS only");
#endif
        const QString heic = fresh(QStringLiteral("photo.heic"));
        if (!platform::encodeImage(testPattern(4032, 3024), heic, QStringLiteral("public.heic"), 0.8))
            QSKIP("this Mac cannot encode HEIC");
        platform::ImageInfo info = platform::imageInfo(heic);
        QVERIFY2(info.ok, qPrintable(info.error));
        QCOMPARE(info.mime, QStringLiteral("image/heic"));
        QCOMPARE(QSize(info.width, info.height), QSize(4032, 3024));
        // Shown 700 pt wide on a 2x display, within the default budget.
        MediaService m;
        AttachmentInfo ai;
        ai.hash = fakeHash('e');
        ai.mime = info.mime;
        ai.width = info.width;
        ai.height = info.height;
        const MediaService::Request r = m.imageRequest(ai, heic, 700, 2.0);
        QCOMPARE(QSize(r.key.width, r.key.height), QSize(1400, 1050));
        QVERIFY(r.estimate <= m.budget().base() / 2);
        int owner = 0;
        m.setWanted(&owner, {r});
        QTRY_VERIFY_WITH_TIMEOUT(m.contains(r.key), 10000);
        QVERIFY(!m.tooLarge(r.key));
        MediaKey k;
        const QImage *img = m.nearest(ai.hash, 1400, 0, &k);
        QVERIFY(img);
        QVERIFY(qAbs(img->width() - 1400) <= 1 && qAbs(img->height() - 1050) <= 1);

        const QString cwebp = QStandardPaths::findExecutable(QStringLiteral("cwebp"),
                                                             {QStringLiteral("/opt/homebrew/bin"), QStringLiteral("/usr/local/bin")});
        if (cwebp.isEmpty())
            QSKIP("cwebp not installed: WebP not checked");
        const QString png = fresh(QStringLiteral("w.png"));
        QVERIFY(testPattern(320, 200).save(png));
        const QString webp = fresh(QStringLiteral("w.webp"));
        QCOMPARE(QProcess::execute(cwebp, {QStringLiteral("-quiet"), png, QStringLiteral("-o"), webp}), 0);
        info = platform::imageInfo(webp);
        QVERIFY2(info.ok, qPrintable(info.error));
        QCOMPARE(info.mime, QStringLiteral("image/webp"));
        QCOMPARE(platform::decodeImage(webp, 160).size(), QSize(160, 100));
    }

    void tallScreenshotDecodesWithinBudget()
    {
        // 1000×40000 (40 MP): a scrolling screenshot. Its decode is clamped in
        // total pixels and fits the default budget instead of being refused.
        const QString png = fresh(QStringLiteral("tall.png"));
        {
            QImage tall(1000, 40000, QImage::Format_Grayscale8);
            for (int y = 0; y < tall.height(); ++y)
                memset(tall.scanLine(y), (y / 40) % 2 ? 230 : 40, size_t(tall.width()));
            QVERIFY(tall.save(png));
        }
        const platform::ImageInfo info = platform::imageInfo(png);
        QVERIFY2(info.ok, qPrintable(info.error));
        QCOMPARE(QSize(info.width, info.height), QSize(1000, 40000));
        MediaService m;
        AttachmentInfo ai;
        ai.hash = fakeHash('t');
        ai.mime = info.mime;
        ai.width = info.width;
        ai.height = info.height;
        const MediaService::Request r = m.imageRequest(ai, png, 700, 2.0);
        QVERIFY(qint64(r.key.width) * r.key.height <= m.config().maxPixels);
        QVERIFY2(r.estimate <= m.budget().base(), qPrintable(QString::number(r.estimate)));
        QVERIFY(qAbs(double(r.key.height) / r.key.width - 40.0) < 0.5); // aspect kept
        int owner = 0;
        m.setWanted(&owner, {r});
        QTRY_VERIFY_WITH_TIMEOUT(m.contains(r.key) || m.tooLarge(r.key) || m.failed(ai.hash), 20000);
        QVERIFY(m.contains(r.key));
        const QImage *img = m.lookup(r.key);
        QVERIFY(qAbs(img->height() - r.key.height) <= 2);
        QVERIFY(m.budget().used() <= m.budget().base());
    }

    // --------------------------------------------------------------- store --
    void importStreamsHashesAndStoresMetadata()
    {
        Rig rig(fresh(QStringLiteral("ws.db")), fresh(QStringLiteral("att")), false);
        const QString src = fresh(QStringLiteral("pic.png"));
        QVERIFY(testPattern(64, 48).save(src));
        const qint64 imports = counter(instr::AttachmentImports);
        AttachmentStore::Source s;
        s.path = src;
        s.name = QStringLiteral("pic");
        const AttachmentStore::ImportResult r = rig.importSync(s);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.info.hash, sha256Of(src));
        QCOMPARE(sha256Of(rig.as->pathFor(r.info.hash)), r.info.hash);
        QCOMPARE(QSize(r.info.width, r.info.height), QSize(64, 48));
        QCOMPARE(r.info.mime, QStringLiteral("image/png"));
        QCOMPARE(r.info.bytes, QFileInfo(src).size());
        QCOMPARE(r.alt, QStringLiteral("pic"));
        QCOMPARE(counter(instr::AttachmentImports) - imports, qint64(1));
        QVERIFY(rig.as->info(r.info.hash));
        QVERIFY(!rig.as->isPinned(r.info.hash)); // the import's pin is released
        // Metadata row written through the store's writer.
        QVERIFY(rig.store.flush());
        bool got = false;
        QObject ctx;
        rig.store.attachmentInfo({r.info.hash}, &ctx, [&](QVector<AttachmentInfo> rows) {
            got = rows.size() == 1 && rows[0].width == 64 && rows[0].height == 48;
        });
        QTRY_VERIFY(got);
        // Same bytes again: same file, no temp files left behind.
        QCOMPARE(rig.importSync(s).info.hash, r.info.hash);
        QCOMPARE(QDir(rig.as->directory()).entryList(QDir::Files | QDir::Hidden).size(), 1);
        // A pasted in-memory image is encoded to PNG on the worker.
        AttachmentStore::Source pix;
        pix.image = testPattern(30, 20);
        const AttachmentStore::ImportResult r2 = rig.importSync(pix);
        QVERIFY2(r2.ok, qPrintable(r2.error));
        QCOMPARE(r2.info.mime, QStringLiteral("image/png"));
        QCOMPARE(QSize(r2.info.width, r2.info.height), QSize(30, 20));
        // Not an image: refused, and nothing references the file.
        const QString txt = fresh(QStringLiteral("notes.txt"));
        QFile f(txt);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("plain text, not pixels");
        f.close();
        AttachmentStore::Source bad;
        bad.path = txt;
        QVERIFY(!rig.importSync(bad).ok);
    }

    void placeholderIsNeverSerializedAndFileComesFirst()
    {
        Rig rig(fresh(QStringLiteral("ws.db")), fresh(QStringLiteral("att")), false);
        const PageId p = rig.store.createPage(0, -1, QStringLiteral("Note"));
        PageSession ses(p, &rig.ctx, [&rig] { return rig.alloc(); });
        ses.initNew(0, QStringLiteral("Note"));
        ses.document()->setPageSize(QSizeF(800, -1));
        QTextCursor c(ses.document());
        c.insertText(QStringLiteral("Hello"));
        rig.store.commit(ses.takeSaveBatch());
        VirtualLayout *vl = ses.layout();
        const QTextBlock line = ses.document()->firstBlock();
        const qreal before = vl->blockBoundingRect(line).height();
        const int batch = ses.beginImport(c.position(), 1);
        QVERIFY(ses.hasPendingImports());
        QVERIFY(vl->blockBoundingRect(line).height() > before); // the placeholder takes room
        // A save while importing carries no trace of the placeholder.
        const SaveBatch during = ses.takeSaveBatch();
        for (const BlockRecord &r : during.upserts)
            QVERIFY(!r.text.contains(QLatin1String("attachment:")));
        QVERIFY(!during.hasOrder || during.order.size() == 1);
        QCOMPARE(ses.document()->blockCount(), 1);

        const QString src = fresh(QStringLiteral("photo.png"));
        QVERIFY(testPattern(80, 60).save(src));
        AttachmentStore::Source s;
        s.path = src;
        s.name = QStringLiteral("photo");
        bool fileWasThere = false;
        bool done = false;
        rig.as->import(s, &ses, [&](AttachmentStore::ImportResult r) {
            QVERIFY(r.ok);
            // File first, reference second.
            fileWasThere = sha256Of(rig.as->pathFor(r.info.hash)) == r.info.hash;
            QCOMPARE(ses.document()->blockCount(), 1);
            ses.finishImport(batch, r.ok, r.info, r.alt);
            done = true;
        });
        QTRY_VERIFY(done);
        QVERIFY(fileWasThere);
        QVERIFY(!ses.hasPendingImports());
        QCOMPARE(vl->blockBoundingRect(ses.document()->firstBlock()).height(), before);
        QCOMPARE(ses.document()->blockCount(), 2);
        const QTextBlock img = ses.document()->lastBlock();
        QCOMPARE(PageSession::roleOf(img), LineRole::Image);
        const QString hash = PageSession::imageHash(img);
        QCOMPARE(hash, sha256Of(src));
        QVERIFY(rig.as->isPinned(hash)); // the open session pins what it shows
        const SaveBatch after = ses.takeSaveBatch();
        QVERIFY(after.hasOrder);
        QCOMPARE(after.order.size(), 2);
        bool found = false;
        for (const BlockRecord &r : after.upserts)
            if (r.text == QStringLiteral("![photo](attachment:") + hash + QLatin1Char(')')) {
                found = true;
                QCOMPARE(int(r.kind), int(BlockKind::Text));
            }
        QVERIFY(found);
        // An image pasted on an empty line takes that line over.
        c.movePosition(QTextCursor::End);
        c.insertText(QStringLiteral("\n"));
        const int lines = ses.document()->blockCount();
        const int b2 = ses.beginImport(c.position(), 1);
        AttachmentStore::Source s2;
        s2.image = testPattern(10, 10);
        done = false;
        rig.as->import(s2, &ses, [&](AttachmentStore::ImportResult r) {
            ses.finishImport(b2, r.ok, r.info, QStringLiteral("image"));
            done = true;
        });
        QTRY_VERIFY(done);
        QCOMPARE(ses.document()->blockCount(), lines);
        QCOMPARE(PageSession::roleOf(ses.document()->lastBlock()), LineRole::Image);
    }

    void imageLinesRoundTripThroughStorage()
    {
        Rig rig(fresh(QStringLiteral("ws.db")), fresh(QStringLiteral("att")), false);
        const QString src = fresh(QStringLiteral("p.png"));
        QVERIFY(testPattern(400, 200).save(src));
        AttachmentStore::Source s;
        s.path = src;
        const AttachmentInfo info = rig.importSync(s).info;
        const PageId p = rig.store.createPage(0, -1, QStringLiteral("Imgs"));
        PageSession ses(p, &rig.ctx, [&rig] { return rig.alloc(); });
        ses.initNew(0, QStringLiteral("Imgs"));
        QTextCursor c(ses.document());
        c.insertText(QStringLiteral("before"));
        const BlockId first = PageSession::dataOf(ses.document()->firstBlock())->id;
        Edit e;
        e.op = Edit::Op::InsertBlock;
        e.block = first;
        e.record.kind = BlockKind::Text;
        e.record.text = attachments::imageMarkdown(info.hash, QStringLiteral("a [b]"));
        QVERIFY(ses.applyEdit(e) > 0);
        c.movePosition(QTextCursor::End);
        c.insertText(QStringLiteral("\nafter"), QTextCharFormat());
        QCOMPARE(PageSession::roleOf(ses.document()->findBlockByNumber(1)), LineRole::Image);
        QCOMPARE(PageSession::roleOf(ses.document()->findBlockByNumber(2)), LineRole::Para);
        rig.store.commit(ses.takeSaveBatch());
        QVERIFY(rig.store.flush());
        // A fresh attachment store (a new launch) learns the size from the
        // page load itself, before the page's layout needs it.
        AttachmentStore::Options ao;
        ao.dir = rig.as->directory();
        ao.startupGcDelayMs = 3600000;
        AttachmentStore second(&rig.store, ao);
        bool knownAtLoad = false;
        PageContent pc;
        bool done = false;
        QObject ctx;
        rig.store.openPage(p, &ctx, [&](PageContent got) {
            knownAtLoad = second.info(info.hash) != nullptr;
            pc = std::move(got);
            done = true;
        });
        QTRY_VERIFY(done);
        QVERIFY(knownAtLoad);
        QCOMPARE(pc.blocks.size(), 3);
        QCOMPARE(pc.blocks[1].text, QStringLiteral("![a \\[b\\]](attachment:") + info.hash + QLatin1Char(')'));
        PageSession again(p, &rig.ctx, [&rig] { return rig.alloc(); });
        again.load(pc);
        const QTextBlock img = again.document()->findBlockByNumber(1);
        QCOMPARE(PageSession::roleOf(img), LineRole::Image);
        QCOMPARE(img.text(), QString(QChar::ObjectReplacementCharacter));
        QCOMPARE(img.begin().fragment().charFormat().property(kImageAltProperty).toString(), QStringLiteral("a [b]"));
        // Copy keeps the reference.
        NoteEditor ed(&rig.ctx);
        ed.setSession(&again);
        QTextCursor sel(again.document());
        sel.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        ed.setTextCursor(sel);
        ed.copy();
        QVERIFY(QApplication::clipboard()->text().contains(pc.blocks[1].text));
        ed.setSession(nullptr);
    }

    void sessionsPinUntilClosed()
    {
        Rig rig(fresh(QStringLiteral("ws.db")), fresh(QStringLiteral("att")), false);
        const QString h = fakeHash('c');
        PageContent pc;
        pc.id = 1;
        pc.found = true;
        BlockRecord r;
        r.id = rig.alloc();
        r.text = attachments::imageMarkdown(h, QString());
        pc.blocks << r;
        {
            PageSession ses(1, &rig.ctx, [&rig] { return rig.alloc(); });
            ses.load(pc);
            QVERIFY(rig.as->isPinned(h));
            Edit rm;
            rm.op = Edit::Op::RemoveBlock;
            rm.block = r.id;
            ses.applyEdit(rm);
            QVERIFY(rig.as->isPinned(h)); // undo could bring it back
            ses.document()->undo();
            QCOMPARE(PageSession::roleOf(ses.document()->firstBlock()), LineRole::Image);
        }
        QVERIFY(!rig.as->isPinned(h));
    }

    void garbageCollectionIsReferenceBased()
    {
        Rig rig(fresh(QStringLiteral("ws.db")), fresh(QStringLiteral("att")), false);
        QStringList hashes;
        for (int i = 0; i < 4; ++i) {
            AttachmentStore::Source s;
            s.image = testPattern(10 + i, 10);
            const AttachmentStore::ImportResult res = rig.importSync(s);
            QVERIFY(res.ok);
            hashes << res.info.hash;
        }
        const QString live = hashes[0], trashed = hashes[1], pinned = hashes[2], unreferenced = hashes[3];
        auto pageWith = [&](const QString &hash) {
            const PageId p = rig.store.createPage(0, -1, QStringLiteral("P"));
            SaveBatch b;
            b.page = p;
            b.rev = 1;
            BlockRecord r;
            r.id = rig.alloc();
            r.text = attachments::imageMarkdown(hash, QStringLiteral("x"));
            b.upserts << r;
            b.hasOrder = true;
            b.order << r.id;
            rig.store.commit(b);
            return p;
        };
        pageWith(live);
        const PageId gone = pageWith(trashed);
        bool trashedOk = false;
        QObject ctx;
        rig.store.trashPage(gone, {}, &ctx, [&](bool ok, PageId) { trashedOk = ok; });
        QTRY_VERIFY(trashedOk);
        rig.as->pin(pinned); // an open session's undo stack
        // Files with no metadata row: one fresh (an import in progress
        // elsewhere would look like this), one old (a crash leftover).
        const QString young = fakeHash('1'), old = fakeHash('2');
        for (const QString &h : {young, old}) {
            QFile f(rig.as->pathFor(h));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("x");
        }
        {
            QFile f(rig.as->pathFor(old));
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.setFileTime(QDateTime::currentDateTime().addDays(-1), QFileDevice::FileModificationTime));
        }
        int deleted = -1;
        rig.as->collectGarbage([&](int n) { deleted = n; });
        QTRY_VERIFY(deleted >= 0);
        QCOMPARE(deleted, 2);
        QVERIFY(QFile::exists(rig.as->pathFor(live)));
        QVERIFY(QFile::exists(rig.as->pathFor(trashed)));
        QVERIFY(QFile::exists(rig.as->pathFor(pinned)));
        QVERIFY(QFile::exists(rig.as->pathFor(young)));
        QVERIFY(!QFile::exists(rig.as->pathFor(unreferenced)));
        QVERIFY(!QFile::exists(rig.as->pathFor(old)));
        int rows = -1;
        rig.store.attachmentInfo({live, unreferenced}, &ctx, [&](QVector<AttachmentInfo> v) { rows = int(v.size()); });
        QTRY_COMPARE(rows, 1);
        // The session closes: its pin goes, and the next sweep takes the file.
        rig.as->unpin(pinned);
        deleted = -1;
        rig.as->collectGarbage([&](int n) { deleted = n; });
        QTRY_COMPARE(deleted, 1);
        QVERIFY(!QFile::exists(rig.as->pathFor(pinned)));
    }

    // -------------------------------------------------------- image block --
    void layoutHeightComesFromStoredDimensions()
    {
        Rig rig(fresh(QStringLiteral("ws.db")), fresh(QStringLiteral("att")), false);
        const QString h = fakeHash('d');
        rig.remembered(h, 2000, 1000);
        PageContent pc;
        pc.id = 1;
        pc.found = true;
        for (const QString &t : {QStringLiteral("above"), attachments::imageMarkdown(h, QString()), QStringLiteral("below")}) {
            BlockRecord r;
            r.id = rig.alloc();
            r.text = t;
            pc.blocks << r;
        }
        PageSession ses(1, &rig.ctx, [&rig] { return rig.alloc(); });
        ses.load(pc);
        ses.document()->setPageSize(QSizeF(800, -1));
        VirtualLayout *vl = ses.layout();
        vl->relayoutAll(); // estimates only
        const qreal estimated = vl->documentSize().height();
        const QTextBlock img = ses.document()->findBlockByNumber(1);
        const qreal col = vl->columnWidth();
        QCOMPARE(vl->imageRect(img).size(), QSizeF(col, std::round(col / 2))); // laid out now
        QCOMPARE(vl->documentSize().height(), estimated); // the estimate was exact: no jump
    }

    void scrollingAwayDropsKeysAndBackHitsCache()
    {
        MediaService::Config mc;
        Rig rig(fresh(QStringLiteral("ws.db")), fresh(QStringLiteral("att")), true, mc);
        const QString h = fakeHash('f');
        rig.remembered(h, 1600, 1200);
        PageContent pc;
        pc.id = 1;
        pc.found = true;
        BlockRecord ir;
        ir.id = rig.alloc();
        ir.text = attachments::imageMarkdown(h, QString());
        pc.blocks << ir;
        for (int i = 0; i < 3000; ++i) {
            BlockRecord r;
            r.id = rig.alloc();
            r.text = QStringLiteral("Line %1 of filler text to scroll past.").arg(i);
            pc.blocks << r;
        }
        PageSession ses(1, &rig.ctx, [&rig] { return rig.alloc(); });
        ses.load(pc);
        NoteEditor ed(&rig.ctx);
        ed.resize(800, 600);
        ed.show();
        ed.setSession(&ses);
        const qint64 decodes = counter(instr::ImageDecodes);
        QTRY_COMPARE(rig.media->cacheEntries(), 1);
        QCOMPARE(counter(instr::ImageDecodes) - decodes, qint64(1));
        ed.viewport()->repaint();
        BlockData *d = PageSession::dataOf(ses.document()->firstBlock());
        QVERIFY(!d->mediaKey.isNull());
        // Far away: the layout is evicted, the block forgets its key, the
        // bitmap stays cached (the budget allows it).
        ses.layout()->setLayoutCap(8);
        ed.verticalScrollBar()->setValue(ed.verticalScrollBar()->maximum());
        ed.viewport()->repaint();
        QTest::qWait(20);
        ed.viewport()->repaint();
        QVERIFY(d->mediaKey.isNull());
        QCOMPARE(rig.media->cacheEntries(), 1);
        // Back up: drawn from the cache, no second decode.
        const qint64 hits = counter(instr::ImageCacheHits);
        ed.verticalScrollBar()->setValue(0);
        ed.viewport()->repaint();
        QTest::qWait(50);
        QVERIFY(!d->mediaKey.isNull());
        QCOMPARE(counter(instr::ImageDecodes) - decodes, qint64(1));
        QVERIFY(counter(instr::ImageCacheHits) > hits);
        ed.setSession(nullptr);
    }

    void resizeReusesNearbySizeAndCoalesces()
    {
        Rig rig(fresh(QStringLiteral("ws.db")), fresh(QStringLiteral("att")), true);
        const QString h = fakeHash('9');
        rig.remembered(h, 4000, 3000);
        PageContent pc;
        pc.id = 1;
        pc.found = true;
        BlockRecord ir;
        ir.id = rig.alloc();
        ir.text = attachments::imageMarkdown(h, QString());
        pc.blocks << ir;
        PageSession ses(1, &rig.ctx, [&rig] { return rig.alloc(); });
        ses.load(pc);
        NoteEditor ed(&rig.ctx);
        ed.resize(800, 700);
        ed.show();
        ed.setSession(&ses);
        QTRY_COMPARE(rig.media->cacheEntries(), 1);
        const int w0 = qRound(ses.layout()->columnWidth() * ses.layout()->devicePixelRatio());
        const qint64 decodes = counter(instr::ImageDecodes);
        // Within ±10 %: the cached bitmap is reused, nothing is decoded.
        ed.resize(800 - int(w0 * 0.06), 700);
        ed.viewport()->repaint();
        QTest::qWait(400);
        QCOMPARE(counter(instr::ImageDecodes), decodes);
        // A big change while the divider moves: nothing is requested until the
        // width settles, then one decode at the final width.
        for (int w = 760; w >= 560; w -= 20) {
            ed.resize(w, 700);
            ed.viewport()->repaint();
            QCOMPARE(rig.media->queuedJobs() + rig.media->runningJobs(), 0);
        }
        const int wFinal = qRound(ses.layout()->columnWidth() * ses.layout()->devicePixelRatio());
        QTRY_VERIFY(rig.media->nearest(h, wFinal, 0.0, nullptr));
        QCOMPARE(counter(instr::ImageDecodes) - decodes, qint64(1));
        ed.setSession(nullptr);
    }

    void pastingAnImageImportsIt()
    {
        Rig rig(fresh(QStringLiteral("ws.db")), fresh(QStringLiteral("att")), true);
        const PageId p = rig.store.createPage(0, -1, QStringLiteral("Paste"));
        PageSession ses(p, &rig.ctx, [&rig] { return rig.alloc(); });
        ses.initNew(0, QStringLiteral("Paste"));
        NoteEditor ed(&rig.ctx);
        ed.resize(700, 500);
        ed.show();
        ed.setSession(&ses);
        QTextCursor c = ed.textCursor();
        c.insertText(QStringLiteral("Look:"));
        ed.setTextCursor(c);
        auto *mime = new QMimeData;
        mime->setImageData(testPattern(120, 90));
        QApplication::clipboard()->setMimeData(mime);
        ed.paste();
        QVERIFY(ses.hasPendingImports());
        rig.as->waitForImports();
        QTRY_VERIFY(!ses.hasPendingImports());
        QCOMPARE(ses.document()->blockCount(), 2);
        const QTextBlock img = ses.document()->lastBlock();
        QCOMPARE(PageSession::roleOf(img), LineRole::Image);
        const AttachmentInfo *info = rig.as->info(PageSession::imageHash(img));
        QVERIFY(info);
        QCOMPARE(QSize(info->width, info->height), QSize(120, 90));
        // Typing right after the image starts a plain paragraph below it.
        QTextCursor end(ses.document());
        end.movePosition(QTextCursor::End);
        ed.setTextCursor(end);
        QTest::keyClicks(&ed, QStringLiteral("ok"));
        QCOMPARE(ses.document()->lastBlock().text(), QStringLiteral("ok"));
        QCOMPARE(PageSession::roleOf(ses.document()->lastBlock()), LineRole::Para);
        QVERIFY(ses.document()->lastBlock().charFormat().objectType() != kImageObjectType);
        ed.setSession(nullptr);
    }
};

QTEST_MAIN(MediaTests)
#include "tst_media.moc"
