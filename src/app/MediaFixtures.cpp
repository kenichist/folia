#include "app/MediaFixtures.h"

#include "app/Platform.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>

namespace folia::fixtures {

namespace {

// Smooth colour fields plus fine grain: compresses like a camera photo
// (a few MB as JPEG), unlike pure noise.
QImage photo(int w, int h, quint32 seed)
{
    QRandomGenerator rng(seed);
    QImage small(16, 12, QImage::Format_RGB32);
    for (int y = 0; y < small.height(); ++y)
        for (int x = 0; x < small.width(); ++x)
            small.setPixel(x, y, qRgb(int(rng.bounded(256)), int(rng.bounded(256)), int(rng.bounded(256))));
    QImage img = small.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    for (int y = 0; y < h; ++y) {
        auto *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int n = int(rng.bounded(24)) - 12;
            const QRgb p = line[x];
            line[x] = qRgb(qBound(0, qRed(p) + n, 255), qBound(0, qGreen(p) + n, 255), qBound(0, qBlue(p) + n, 255));
        }
    }
    QPainter pt(&img);
    pt.setPen(QPen(Qt::white, 18));
    pt.drawEllipse(QPointF(w * 0.3, h * 0.4), w * 0.12, w * 0.12); // some edges
    return img;
}

// Flat panels, text-like stripes: compresses like a UI screenshot.
QImage screenshot(int w, int h, quint32 seed)
{
    QRandomGenerator rng(seed);
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(QColor(246, 246, 248));
    QPainter p(&img);
    p.fillRect(0, 0, w / 5, h, QColor(232, 232, 236));
    for (int y = 40; y < h - 20; y += 28) {
        const int len = int(rng.bounded(w / 2)) + w / 6;
        p.fillRect(w / 5 + 40, y, len, 12, QColor(60, 60, 64));
        if (rng.bounded(9) == 0)
            p.fillRect(w / 5 + 40, y + 20, w / 3, h / 10, QColor(int(rng.bounded(200)), 120, 200));
    }
    return img;
}

QString ensure(const QString &path, const std::function<bool(const QString &)> &make)
{
    if (QFileInfo(path).size() > 0)
        return path;
    return make(path) ? path : QString();
}

} // namespace

MediaSet makeMediaSet(const QString &dir, int photos, bool huge)
{
    QDir().mkpath(dir);
    MediaSet set;
    for (int i = 0; i < photos; ++i)
        set.photos << ensure(QDir(dir).filePath(QStringLiteral("photo_%1.jpg").arg(i, 2, 10, QLatin1Char('0'))),
                             [i](const QString &p) {
                                 return platform::encodeImage(photo(4032, 3024, 1000 + quint32(i)), p,
#ifdef Q_OS_MACOS
                                                              QStringLiteral("public.jpeg"),
#else
                                                              QStringLiteral("jpg"),
#endif
                                                              0.85);
                             });
#ifdef Q_OS_MACOS
    set.heic = ensure(QDir(dir).filePath(QStringLiteral("photo.heic")), [](const QString &p) {
        return platform::encodeImage(photo(4032, 3024, 77), p, QStringLiteral("public.heic"), 0.8);
    });
#endif
    const QString cwebp = QStandardPaths::findExecutable(QStringLiteral("cwebp"),
                                                         {QStringLiteral("/opt/homebrew/bin"), QStringLiteral("/usr/local/bin")});
    if (!cwebp.isEmpty() && !set.photos.isEmpty() && !set.photos.first().isEmpty())
        set.webp = ensure(QDir(dir).filePath(QStringLiteral("photo.webp")), [&](const QString &p) {
            return QProcess::execute(cwebp, {QStringLiteral("-quiet"), QStringLiteral("-q"), QStringLiteral("80"),
                                             set.photos.first(), QStringLiteral("-o"), p}) == 0;
        });
    for (int i = 0; i < 3; ++i)
        set.screenshots << ensure(QDir(dir).filePath(QStringLiteral("screenshot_%1.png").arg(i)), [i](const QString &p) {
            return screenshot(2880, 1800, 500 + quint32(i)).save(p);
        });
    set.tall = ensure(QDir(dir).filePath(QStringLiteral("tall.png")), [](const QString &p) {
        return screenshot(1000, 40000, 9).save(p);
    });
    if (huge)
        set.hugePng = ensure(QDir(dir).filePath(QStringLiteral("huge.png")), [](const QString &p) {
            return photo(6000, 4000, 4242).save(p);
        });
    return set;
}

PdfSet makePdfSet(const QString &dir)
{
    QDir().mkpath(dir);
    PdfSet set;
    auto pdf = [&](const QString &name, const QString &style, int pages) {
        return ensure(QDir(dir).filePath(name), [&](const QString &p) { return platform::writeFixturePdf(p, style, pages); });
    };
    set.long200 = pdf(QStringLiteral("long-200.pdf"), QStringLiteral("text"), 200);
    set.scanned = pdf(QStringLiteral("scanned-24.pdf"), QStringLiteral("scanned"), 24);
    set.vector = pdf(QStringLiteral("vector-16.pdf"), QStringLiteral("vector"), 16);
    set.transparency = pdf(QStringLiteral("transparency-16.pdf"), QStringLiteral("transparency"), 16);
    set.mixed = pdf(QStringLiteral("mixed-8.pdf"), QStringLiteral("mixed"), 8);
    return set;
}

} // namespace folia::fixtures
