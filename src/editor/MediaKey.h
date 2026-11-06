// Identity of one decoded bitmap in the media cache: attachment hash plus
// physical size, orientation and colour handling (docs/ATTACHMENTS.md §3).
// Blocks keep keys, never bitmaps.
#pragma once

#include <QMetaType>
#include <QString>

namespace folia {

struct MediaKey {
    QString hash;
    int width = 0;   // physical pixels of the decoded bitmap
    int height = 0;
    quint8 orientation = 1;
    quint8 color = 1; // 1: converted to sRGB
    quint8 kind = 0;  // 0: image (later: PDF thumbnail, PDF tile)
    bool isNull() const { return hash.isEmpty(); }
    bool operator==(const MediaKey &o) const
    {
        return width == o.width && height == o.height && orientation == o.orientation && color == o.color
            && kind == o.kind && hash == o.hash;
    }
    bool operator!=(const MediaKey &o) const { return !(*this == o); }
};
size_t qHash(const MediaKey &k, size_t seed = 0);

} // namespace folia

Q_DECLARE_METATYPE(folia::MediaKey)
