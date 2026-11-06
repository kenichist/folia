// Attachments (docs/ATTACHMENTS.md): content-addressed originals referenced
// from page text as Markdown, `![alt](attachment:<sha256>)` for images.
//
//  * A reference is one stored Text block whose whole text is that Markdown.
//    In the editor it is one line holding a single U+FFFC object (like a
//    child-page link), converted both ways at load and save.
//  * Metadata (dimensions after the EXIF orientation transform, MIME type,
//    byte size) lives in the `attachments` table; the original lives under
//    <data folder>/attachments/<sha256>.
#pragma once

#include <QString>
#include <QStringView>
#include <QtGlobal>

namespace folia {

struct AttachmentInfo {
    QString hash;        // lower-case hex SHA-256 of the original's bytes
    QString mime;        // "image/jpeg", "image/png", "image/heic", ...
    qint64 bytes = 0;    // size of the original
    int width = 0;       // pixels, after the orientation transform
    int height = 0;
    int orientation = 1; // EXIF orientation 1..8 (applied by the decoder)
    int pages = 0;       // PDFs (later): page count
    bool valid() const { return !hash.isEmpty() && width > 0 && height > 0; }
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

} // namespace attachments
} // namespace folia
