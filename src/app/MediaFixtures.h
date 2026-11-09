// Deterministic image fixtures for the image benchmarks and the decode-cost
// probe (tools/mediaprobe.cpp): camera-like photos (JPEG, HEIC, WebP), UI
// screenshots, a very tall scrolling screenshot and a huge PNG; and the PDF
// fixtures for the PDF benchmarks.
#pragma once

#include <QString>
#include <QStringList>

namespace folia::fixtures {

struct MediaSet {
    QStringList photos;     // 4032×3024 JPEG, distinct content each
    QString heic;           // 4032×3024 (empty if this Mac cannot encode HEIC)
    QString webp;           // 4032×3024 (empty without cwebp)
    QStringList screenshots; // 2880×1800 PNG, flat UI-like content
    QString tall;           // 1000×40000 PNG
    QString hugePng;        // 6000×4000 noisy PNG (tens of MB)
};

// Writes the set into `dir` (reusing files already there). `photos` JPEGs;
// the huge PNG only when `huge` is set (it takes seconds to encode).
MediaSet makeMediaSet(const QString &dir, int photos, bool huge);

// PDF fixtures (docs/ATTACHMENTS.md §6), written with CoreGraphics; empty
// paths where this platform cannot write PDFs.
struct PdfSet {
    QString long200;      // 200 Letter pages of body text, a vector figure every 5th page
    QString scanned;      // 24 pages, one 300 dpi JPEG scan each (2550×3300)
    QString vector;       // 16 pages, 3000 curves and 400 polygons each
    QString transparency; // 16 pages of text under 24 nested transparency groups with blend modes and soft masks
    QString mixed;        // 8 pages: Letter, A4, landscape, receipt, with corner markers
};
PdfSet makePdfSet(const QString &dir);

// Upload fixtures (docs/ATTACHMENTS.md §8): a long Markdown file of
// `markdownMiB` MiB and a Word file of `docxParagraphs` paragraphs.
struct UploadSet {
    QString markdown;
    QString docx;
};
UploadSet makeUploadSet(const QString &dir, int markdownMiB, int docxParagraphs);

} // namespace folia::fixtures
