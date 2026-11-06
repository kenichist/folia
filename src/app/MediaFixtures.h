// Deterministic image fixtures for the image benchmarks and the decode-cost
// probe (tools/mediaprobe.cpp): camera-like photos (JPEG, HEIC, WebP), UI
// screenshots, a very tall scrolling screenshot and a huge PNG.
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

} // namespace folia::fixtures
