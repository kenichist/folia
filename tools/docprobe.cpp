// folia_docprobe — the cost of converting one uploaded document, in a fresh
// process (docs/ATTACHMENTS.md §8, docs/BENCHMARKS.md §6e).
//
//   folia_docprobe <file.md|file.txt|file.docx> [--print] [--whole]
//
// Prints one JSON object: blocks and characters produced, images found,
// wall and CPU time, and the peak footprint growth during the conversion
// (rusage's interval maximum, reset right before it). Images are read in
// full and counted but not stored. --print also writes the page's lines to
// stderr. --whole (Word only) inflates word/document.xml into memory first
// and parses that, to show what streaming it saves.
#include "app/Platform.h"
#include "import/DocumentKinds.h"
#include "import/DocxImport.h"
#include "import/MarkdownImport.h"
#include "import/ZipReader.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QXmlStreamReader>

#include <cstdio>

using namespace folia;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList a = app.arguments();
    if (a.size() < 2) {
        fprintf(stderr, "usage: folia_docprobe <file.md|file.txt|file.docx> [--print] [--whole]\n");
        return 2;
    }
    const QString path = a[1];
    const bool print = a.contains(QStringLiteral("--print"));
    const bool whole = a.contains(QStringLiteral("--whole"));
    int images = 0;
    qint64 imageBytes = 0;
    qint64 tokens = 0; // --whole: XML tokens read
    platform::intervalPeakFootprint(); // resets the interval maximum
    const qint64 before = platform::sampleSelf().footprintBytes;
    platform::intervalPeakFootprint();
    QElapsedTimer t;
    t.start();
    const qint64 cpu0 = platform::threadCpuMicros();
    import::ImportedDoc doc;
    if (import::isWordFile(path)) {
        if (whole) {
            // What a non-streaming reader would hold: the whole part, then a parse.
            import::ZipReader z;
            QString err;
            if (z.open(path, &err)) {
                const QByteArray xml = z.readAll(z.find(QStringLiteral("word/document.xml")), 1ll << 30, &err);
                QXmlStreamReader x(xml);
                while (!x.atEnd() && x.readNext() != QXmlStreamReader::Invalid)
                    ++tokens;
                doc.outputChars = xml.size(); // the bytes held at once
            }
            doc.error = err;
        } else {
            doc = import::importDocx(path, [&](QIODevice *data, const QString &, const QString &alt) {
                QCryptographicHash h(QCryptographicHash::Sha256);
                char buf[64 * 1024];
                qint64 n;
                while ((n = data->read(buf, sizeof(buf))) > 0) {
                    h.addData(QByteArrayView(buf, n));
                    imageBytes += n;
                }
                ++images;
                return attachments::imageMarkdown(QString::fromLatin1(h.result().toHex()), alt);
            });
        }
    } else {
        doc = import::importTextFile(path, [&](const QString &p, const QString &alt) {
            ++images;
            imageBytes += QFileInfo(p).size();
            return attachments::imageMarkdown(QString(64, QLatin1Char('0')), alt);
        });
    }
    const qint64 cpu = platform::threadCpuMicros() - cpu0;
    const qint64 wall = t.nsecsElapsed() / 1000;
    const qint64 peak = platform::intervalPeakFootprint();
    QJsonObject o;
    o.insert(QStringLiteral("file"), QFileInfo(path).fileName());
    o.insert(QStringLiteral("file_bytes"), double(QFileInfo(path).size()));
    o.insert(QStringLiteral("mode"), whole ? QStringLiteral("whole") : QStringLiteral("stream"));
    o.insert(QStringLiteral("error"), doc.error);
    o.insert(QStringLiteral("title"), doc.title);
    o.insert(QStringLiteral("blocks"), int(doc.blocks.size()));
    if (whole)
        o.insert(QStringLiteral("xml_tokens"), double(tokens));
    o.insert(QStringLiteral("chars"), double(doc.outputChars));
    o.insert(QStringLiteral("images"), images);
    o.insert(QStringLiteral("image_bytes"), double(imageBytes));
    o.insert(QStringLiteral("notes"), doc.notes.join(QLatin1Char(' ')));
    o.insert(QStringLiteral("wall_us"), double(wall));
    o.insert(QStringLiteral("cpu_us"), double(cpu));
    o.insert(QStringLiteral("footprint_before"), double(before));
    o.insert(QStringLiteral("peak_growth"), double(qMax<qint64>(0, peak - before)));
    printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
    if (print)
        for (const BlockRecord &b : std::as_const(doc.blocks))
            fprintf(stderr, "%s%s\n",
                    b.kind == BlockKind::Code ? "[code] " : b.kind == BlockKind::Math ? "[math] " : "",
                    qPrintable(b.text));
    return doc.error.isEmpty() ? 0 : 1;
}
