// folia_mediaprobe — the cost of one image decode, in a fresh process.
//
//   folia_mediaprobe --make <dir> [photos]        write the image fixtures
//   folia_mediaprobe <image> <maxPixelSize> [imageio|qt]
//
// Prints one JSON object: the header metadata, the decoded size, wall and CPU
// time, and the peak footprint growth during the decode, measured with
// rusage's interval maximum (reset by the read right before the decode).
// `peak_over_output` is what MediaService::costFor calibrates: how many times
// the output bitmap's size one decode needs at its peak.
#include "app/MediaFixtures.h"
#include "app/Platform.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

using namespace folia;

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
