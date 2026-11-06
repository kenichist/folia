// Attachments (docs/ATTACHMENTS.md): content-addressed originals referenced
// from page text as Markdown, `![alt](attachment:<sha256>)` for images and
// `[file.pdf](attachment:<sha256>)` for PDFs.
//
//  * A reference is one stored Text block whose whole text is that Markdown.
//    In the editor it is one line holding a single U+FFFC object (like a
//    child-page link), converted both ways at load and save.
//  * Metadata (dimensions after the EXIF orientation transform, MIME type,
//    byte size) lives in the `attachments` table; the original lives under
//    <data folder>/attachments/<sha256>.
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringView>
#include <QVector>
#include <QtGlobal>

namespace folia {

struct AttachmentInfo {
    QString hash;        // lower-case hex SHA-256 of the original's bytes
    QString mime;        // "image/jpeg", "image/png", "image/heic", ...
    qint64 bytes = 0;    // size of the original
    int width = 0;       // pixels, after the orientation transform
    int height = 0;
    int orientation = 1; // EXIF orientation 1..8 (applied by the decoder)
    int pages = 0;       // PDFs: page count
    // PDFs: one (w, h, rotation) per page, encoded by attachments::encodePageGeom.
    // width/height above are the first page's size in points, rotated.
    QByteArray pageGeom;
    bool valid() const { return !hash.isEmpty() && width > 0 && height > 0; }
    bool isPdf() const { return mime == QLatin1String("application/pdf"); }
};

// One PDF page as laid out: crop box size in points *after* the page's
// rotation (so layout never needs the rotation), and the rotation itself
// (0, 90, 180 or 270, clockwise) for the renderer. Plus two render-cost
// hints read from the page's resources at import (no content is drawn):
// CoreGraphics decodes embedded images in full (at most reduced by a power
// of two for JPEG) and allocates a device-resolution layer per transparency
// group, whatever the output size (docs/BENCHMARKS.md §6b, "PDF render
// cost"); and long content streams take long to walk, once per tile.
struct PdfPageGeom {
    enum Flag : quint16 {
        Layers = 1,         // transparency groups or soft masks
        ReducibleImage = 2, // the largest image is DCT/JPX: decoded at native/2^k
        HeavyContent = 4,   // content streams over 64 KB (thousands of paths)
    };
    float w = 0;
    float h = 0;
    int rotation = 0;
    quint16 flags = 0;
    quint32 imagePixels = 0; // the largest embedded image (w×h), 0 if none
    bool operator==(const PdfPageGeom &o) const
    {
        return w == o.w && h == o.h && rotation == o.rotation && flags == o.flags && imagePixels == o.imagePixels;
    }
    // Slow to render per tile: worth not starting while the view flies past.
    bool heavy() const { return (flags & (Layers | HeavyContent)) || imagePixels >= 1000000; }
};

namespace attachments {

constexpr int kHashLength = 64;

bool isHash(QStringView s);
// `![alt](attachment:<hash>)` -> true with hash/alt; anything else -> false.
// Brackets and backslashes in alt are backslash-escaped.
bool parseImageMarkdown(QStringView text, QString *hash, QString *alt);
QString imageMarkdown(const QString &hash, const QString &alt);
// Cheap pre-check used while scanning many stored blocks.
inline bool mayBeImageMarkdown(QStringView text)
{
    return text.size() > kHashLength + 14 && text.startsWith(u"![") && text.endsWith(u')');
}
// `[file.pdf](attachment:<hash>)` (no '!') -> true with hash/name. Same
// escaping as the image form.
bool parseFileMarkdown(QStringView text, QString *hash, QString *name);
QString fileMarkdown(const QString &hash, const QString &name);
inline bool mayBeFileMarkdown(QStringView text)
{
    return text.size() > kHashLength + 13 && text.startsWith(u'[') && text.endsWith(u')');
}
// Either form: the referenced hash (GC, page loads).
bool parseAnyMarkdown(QStringView text, QString *hash);

// page_geom: runs of identical pages, so a uniform document of any length
// is 21 bytes. Layout: version byte 2, then per run (little-endian): uint32
// count, float32 w, float32 h, uint16 rotation, uint16 flags, uint32 image
// pixels. Version 1 (runs without flags and image pixels) is still read.
QByteArray encodePageGeom(const QVector<PdfPageGeom> &pages);
QVector<PdfPageGeom> decodePageGeom(const QByteArray &blob);
// The first page only, without expanding the runs (thumbnails, per paint).
PdfPageGeom firstPageGeom(const QByteArray &blob);

} // namespace attachments
} // namespace folia
