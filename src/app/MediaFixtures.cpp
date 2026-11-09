#include "app/MediaFixtures.h"

#include "app/Platform.h"

#include <QDir>
#include <QFile>
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

// ------------------------------------------------- upload fixtures ----
namespace {

quint32 crc32(const QByteArray &d)
{
    static quint32 table[256];
    static bool init = false;
    if (!init) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    quint32 c = 0xFFFFFFFFu;
    for (char ch : d)
        c = table[(c ^ quint8(ch)) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void le16(QByteArray &o, quint16 v) { o.append(char(v & 0xff)).append(char(v >> 8)); }
void le32(QByteArray &o, quint32 v) { le16(o, quint16(v & 0xffff)); le16(o, quint16(v >> 16)); }

// Deflated entries: qCompress's zlib stream without its header and checksum.
QByteArray zip(const QList<QPair<QString, QByteArray>> &entries)
{
    QByteArray out, central;
    for (const auto &e : entries) {
        const QByteArray name = e.first.toUtf8();
        const QByteArray z = qCompress(e.second, 6);
        const QByteArray data = z.mid(6, z.size() - 10);
        const quint32 offset = quint32(out.size());
        const quint32 crc = crc32(e.second);
        le32(out, 0x04034b50); le16(out, 20); le16(out, 0); le16(out, 8); le16(out, 0); le16(out, 0x21);
        le32(out, crc); le32(out, quint32(data.size())); le32(out, quint32(e.second.size()));
        le16(out, quint16(name.size())); le16(out, 0);
        out += name + data;
        le32(central, 0x02014b50); le16(central, 20); le16(central, 20); le16(central, 0); le16(central, 8);
        le16(central, 0); le16(central, 0x21); le32(central, crc); le32(central, quint32(data.size()));
        le32(central, quint32(e.second.size())); le16(central, quint16(name.size()));
        for (int i = 0; i < 4; ++i)
            le16(central, 0);
        le32(central, 0); le32(central, offset);
        central += name;
    }
    const quint32 cd = quint32(out.size());
    out += central;
    le32(out, 0x06054b50); le16(out, 0); le16(out, 0); le16(out, quint16(entries.size()));
    le16(out, quint16(entries.size())); le32(out, quint32(central.size())); le32(out, cd); le16(out, 0);
    return out;
}

bool writeAll(const QString &path, const QByteArray &data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

} // namespace

UploadSet makeUploadSet(const QString &dir, int markdownMiB, int docxParagraphs)
{
    QDir().mkpath(dir);
    UploadSet set;
    set.markdown = ensure(QDir(dir).filePath(QStringLiteral("long-%1mib.md").arg(markdownMiB)), [&](const QString &p) {
        QByteArray md = "# Long Markdown\n\n";
        for (int i = 0; md.size() < qint64(markdownMiB) << 20; ++i) {
            if (i % 40 == 0)
                md += "## Section " + QByteArray::number(i / 40 + 1) + "\n\n";
            if (i % 40 == 10)
                md += "```cpp\nint answer() { return 42; }\n```\n\n";
            if (i % 40 == 20)
                md += "- first item\n- second item with **bold**\n\n";
            md += "Paragraph " + QByteArray::number(i) + " is soft-wrapped across two lines, with *italic*,\n"
                  "`code` and a [link](https://example.com/" + QByteArray::number(i) + ").\n\n";
        }
        return writeAll(p, md);
    });
    set.docx = ensure(QDir(dir).filePath(QStringLiteral("long-%1p.docx").arg(docxParagraphs)), [&](const QString &p) {
        const QByteArray w = "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"";
        QByteArray body;
        for (int i = 0; i < docxParagraphs; ++i) {
            if (i % 50 == 0)
                body += "<w:p><w:pPr><w:pStyle w:val=\"Heading2\"/></w:pPr><w:r><w:t>Section " + QByteArray::number(i / 50 + 1)
                    + "</w:t></w:r></w:p>";
            body += "<w:p><w:r><w:rPr><w:rFonts w:ascii=\"Calibri\"/><w:sz w:val=\"24\"/></w:rPr><w:t xml:space=\"preserve\">"
                    "Paragraph " + QByteArray::number(i) + " has </w:t></w:r><w:r><w:rPr><w:b/><w:sz w:val=\"24\"/></w:rPr>"
                    "<w:t>some bold</w:t></w:r><w:r><w:rPr><w:sz w:val=\"24\"/></w:rPr><w:t xml:space=\"preserve\"> and plain "
                    "text written to make the document long enough to measure.</w:t></w:r></w:p>";
        }
        const QByteArray doc = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><w:document " + w + "><w:body>" + body
            + "</w:body></w:document>";
        const QByteArray styles = "<?xml version=\"1.0\"?><w:styles " + w + "><w:style w:type=\"paragraph\" "
                                  "w:styleId=\"Heading2\"><w:name w:val=\"heading 2\"/></w:style></w:styles>";
        const QByteArray rels = "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/"
                                "relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/"
                                "officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/></Relationships>";
        const QByteArray docRels = "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/"
                                   "2006/relationships\"><Relationship Id=\"rIdS\" Type=\"http://schemas.openxmlformats.org/"
                                   "officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/></Relationships>";
        return writeAll(p, zip({{QStringLiteral("[Content_Types].xml"), "<Types/>"},
                                {QStringLiteral("_rels/.rels"), rels},
                                {QStringLiteral("word/_rels/document.xml.rels"), docRels},
                                {QStringLiteral("word/styles.xml"), styles},
                                {QStringLiteral("word/document.xml"), doc}}));
    });
    return set;
}

} // namespace folia::fixtures
