// folia_mediaprobe — the cost of one image decode or PDF render, in a fresh
// process.
//
//   folia_mediaprobe --make <dir> [photos]        write the image fixtures
//   folia_mediaprobe <image> <maxPixelSize> [imageio|qt]
//   folia_mediaprobe --make-pdf <dir>             write the PDF fixtures
//   folia_mediaprobe --pdf <file> <page> <pageWidthPx> [tile] [viewW] [viewH]
//       tile 0: the whole page as one bitmap; tile N: the N×N tiles covering
//       a viewW×viewH physical-pixel viewport at the page's top left, each
//       rendered straight into its own tile-sized bitmap. The page is drawn
//       twice (cold: first use of the document; warm: CG's caches filled).
//
// Prints one JSON object: the header metadata, the decoded size, wall and CPU
// time, and the peak footprint growth during the decode, measured with
// rusage's interval maximum (reset by the read right before the decode).
// `peak_over_output` is what MediaService::costFor calibrates: how many times
// the output bitmap's size one decode needs at its peak.
#include "app/MediaFixtures.h"
#include "app/Platform.h"
#include "editor/MediaService.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QThreadPool>

#include <cmath>
#include <cstdio>
#include <memory>

using namespace folia;

namespace {

// One page (or the tiles of one viewport of it), cold then warm.
int probePdf(const QStringList &a)
{
    const QString path = a[2];
    const int page = a[3].toInt();
    const int pageW = a[4].toInt();
    const int tile = a.size() > 5 ? a[5].toInt() : 0;
    const int viewW = a.size() > 6 ? a[6].toInt() : 1600;
    const int viewH = a.size() > 7 ? a[7].toInt() : 1000;
    const platform::PdfInfo info = platform::pdfInfo(path);
    if (!info.ok || page < 0 || page >= info.pages) {
        fprintf(stderr, "cannot read %s: %s\n", qPrintable(path), qPrintable(info.error));
        return 1;
    }
    const PdfPageGeom g = info.geom[page];
    const int pageH = int(std::ceil(pageW * double(g.h) / double(g.w)));
    QVector<QRect> regions;
    if (tile <= 0) {
        regions << QRect(0, 0, pageW, pageH);
    } else {
        const QRect view(0, 0, qMin(viewW, pageW), qMin(viewH, pageH));
        for (int y = 0; y * tile < view.bottom() + 1; ++y)
            for (int x = 0; x * tile < view.right() + 1; ++x)
                regions << QRect(x * tile, y * tile, qMin(tile, pageW - x * tile), qMin(tile, pageH - y * tile));
    }
    QJsonObject o;
    o.insert(QStringLiteral("file"), QFileInfo(path).fileName());
    o.insert(QStringLiteral("page"), page);
    o.insert(QStringLiteral("page_w_px"), pageW);
    o.insert(QStringLiteral("page_h_px"), pageH);
    o.insert(QStringLiteral("tile"), tile);
    o.insert(QStringLiteral("bitmaps"), int(regions.size()));
    qint64 outBytes = 0, maxOut = 0;
    for (const QRect &r : regions) {
        outBytes += qint64(r.width()) * r.height() * 4;
        maxOut = qMax(maxOut, qint64(r.width()) * r.height() * 4);
    }
    o.insert(QStringLiteral("out_bytes"), double(outBytes));
    o.insert(QStringLiteral("max_bitmap_bytes"), double(maxOut));
    // The reservation MediaService makes for the largest of these bitmaps.
    qint64 est = 0;
    for (const QRect &r : regions)
        est = qMax(est, MediaService::pdfEstimate(g, pageW, r.width(), r.height()));
    o.insert(QStringLiteral("estimate_bytes"), double(est));
    o.insert(QStringLiteral("hint_image_pixels"), double(g.imagePixels));
    o.insert(QStringLiteral("hint_layers"), bool(g.flags & PdfPageGeom::Layers));
    o.insert(QStringLiteral("hint_reducible"), bool(g.flags & PdfPageGeom::ReducibleImage));
    const qint64 base = platform::sampleSelf().footprintBytes;
    o.insert(QStringLiteral("footprint_base"), double(base));
    auto doc = std::make_unique<platform::PdfDocument>(path);
    for (const char *phase : {"cold", "warm"}) {
        platform::intervalPeakFootprint();
        const qint64 before = platform::sampleSelf().footprintBytes;
        platform::intervalPeakFootprint();
        QElapsedTimer t;
        t.start();
        const qint64 cpu0 = platform::threadCpuMicros();
        qint64 worstPeak = 0;
        bool ok = true;
        for (const QRect &r : regions) {
            QString err;
            QImage img;
            if (qEnvironmentVariableIsSet("FOLIA_PROBE_POOL")) {
                // As in the app: on a QThreadPool worker (QoS experiments).
                QThreadPool pool;
                pool.start([&] { img = platform::renderPdfPage(doc.get(), page, pageW, r, &err); });
                pool.waitForDone();
            } else {
                img = platform::renderPdfPage(doc.get(), page, pageW, r, &err);
            }
            ok = ok && !img.isNull();
            if (!qEnvironmentVariableIsEmpty("FOLIA_PROBE_SAVE") && regions.size() == 1)
                img.save(qEnvironmentVariable("FOLIA_PROBE_SAVE")); // eyeballing only
            // Each bitmap is dropped before the next, as a tile job's result
            // goes to the cache: the per-job peak is what a reservation covers.
            worstPeak = qMax(worstPeak, platform::intervalPeakFootprint() - before);
        }
        const QString p = QString::fromLatin1(phase);
        o.insert(p + QStringLiteral("_ms"), t.nsecsElapsed() / 1e6);
        o.insert(p + QStringLiteral("_cpu_ms"), (platform::threadCpuMicros() - cpu0) / 1e3);
        o.insert(p + QStringLiteral("_peak_delta_bytes"), double(worstPeak));
        o.insert(p + QStringLiteral("_retained_bytes"), double(platform::sampleSelf().footprintBytes - before));
        o.insert(p + QStringLiteral("_ok"), ok);
    }
    o.insert(QStringLiteral("doc_open_retained_bytes"), double(platform::sampleSelf().footprintBytes - base));
    doc.reset();
    o.insert(QStringLiteral("after_close_retained_bytes"), double(platform::sampleSelf().footprintBytes - base));
    platform::releaseFreedMemory();
    o.insert(QStringLiteral("after_relief_retained_bytes"), double(platform::sampleSelf().footprintBytes - base));
    printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
    fflush(stdout);
    if (const int hold = qEnvironmentVariableIntValue("FOLIA_PROBE_HOLD_S"); hold > 0)
        QThread::sleep(hold); // inspect with vmmap / footprint meanwhile
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList a = app.arguments();
    if (a.size() >= 3 && a[1] == QLatin1String("--make")) {
        const fixtures::MediaSet s = fixtures::makeMediaSet(a[2], a.size() > 3 ? a[3].toInt() : 2, true);
        QJsonObject o;
        o.insert(QStringLiteral("photos"), QJsonValue::fromVariant(s.photos));
        o.insert(QStringLiteral("heic"), s.heic);
        o.insert(QStringLiteral("webp"), s.webp);
        o.insert(QStringLiteral("screenshots"), QJsonValue::fromVariant(s.screenshots));
        o.insert(QStringLiteral("tall"), s.tall);
        o.insert(QStringLiteral("huge_png"), s.hugePng);
        printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    if (a.size() >= 3 && a[1] == QLatin1String("--make-pdf")) {
        const fixtures::PdfSet s = fixtures::makePdfSet(a[2]);
        QJsonObject o;
        o.insert(QStringLiteral("long200"), s.long200);
        o.insert(QStringLiteral("scanned"), s.scanned);
        o.insert(QStringLiteral("vector"), s.vector);
        o.insert(QStringLiteral("transparency"), s.transparency);
        o.insert(QStringLiteral("mixed"), s.mixed);
        printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
        return s.long200.isEmpty() ? 1 : 0;
    }
    if (a.size() >= 5 && a[1] == QLatin1String("--pdf"))
        return probePdf(a);
    if (a.size() < 3) {
        fprintf(stderr, "usage: folia_mediaprobe <image> <maxPixelSize> [imageio|qt] | --make <dir>\n");
        return 2;
    }
    const QString path = a[1];
    const int maxPx = a[2].toInt();
    const bool qt = a.size() > 3 && a[3] == QLatin1String("qt");
    const platform::ImageInfo info = qt ? platform::imageInfoQt(path) : platform::imageInfo(path);
    platform::intervalPeakFootprint(); // resets the interval maximum
    const qint64 before = platform::sampleSelf().footprintBytes;
    platform::intervalPeakFootprint();
    QElapsedTimer t;
    t.start();
    const qint64 cpu0 = platform::threadCpuMicros();
    QString err;
    QImage img = qt ? platform::decodeImageQt(path, maxPx, &err) : platform::decodeImage(path, maxPx, &err);
    const qint64 cpu = platform::threadCpuMicros() - cpu0;
    const qint64 wall = t.nsecsElapsed() / 1000;
    const qint64 peak = platform::intervalPeakFootprint();
    const qint64 after = platform::sampleSelf().footprintBytes;
    QJsonObject o;
    o.insert(QStringLiteral("file"), QFileInfo(path).fileName());
    o.insert(QStringLiteral("file_bytes"), double(QFileInfo(path).size()));
    o.insert(QStringLiteral("decoder"), qt ? QStringLiteral("qimagereader") : QStringLiteral("imageio"));
    o.insert(QStringLiteral("mime"), info.mime);
    o.insert(QStringLiteral("native_w"), info.width);
    o.insert(QStringLiteral("native_h"), info.height);
    o.insert(QStringLiteral("native_bytes"), double(qint64(info.width) * info.height * 4));
    o.insert(QStringLiteral("max_pixel_size"), maxPx);
    o.insert(QStringLiteral("out_w"), img.width());
    o.insert(QStringLiteral("out_h"), img.height());
    const qint64 out = img.sizeInBytes();
    o.insert(QStringLiteral("out_bytes"), double(out));
    o.insert(QStringLiteral("decode_us"), double(wall));
    o.insert(QStringLiteral("cpu_us"), double(cpu));
    o.insert(QStringLiteral("footprint_before"), double(before));
    o.insert(QStringLiteral("footprint_after"), double(after));
    o.insert(QStringLiteral("peak_delta_bytes"), double(peak - before));
    o.insert(QStringLiteral("peak_over_output"), out > 0 ? double(peak - before) / double(out) : 0.0);
    o.insert(QStringLiteral("peak_over_native"), info.width > 0 ? double(peak - before) / (4.0 * info.width * info.height) : 0.0);
    if (img.isNull())
        o.insert(QStringLiteral("error"), err);
    printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
    return img.isNull() ? 1 : 0;
}
