// Identity of one decoded bitmap in the media cache: attachment hash plus
// physical size, orientation and colour handling (docs/ATTACHMENTS.md §3),
// and for PDFs the page, zoom level and tile (§4). Blocks keep keys, never
// bitmaps.
#pragma once

#include <QMetaType>
#include <QString>

namespace folia {

struct MediaKey {
    enum Kind : quint8 {
        Image = 0,
        PdfThumb = 1, // first page, in the note
        PdfPage = 2,  // a whole page in the viewer
        PdfTile = 3,  // one tile of a page drawn `level` pixels wide
    };
    QString hash;
    int width = 0;   // physical pixels of the decoded bitmap
    int height = 0;
    quint8 orientation = 1;
    quint8 color = 1; // 1: converted to sRGB
    quint8 kind = Image;
    int page = 0;    // PDFs: 0-based page
    int level = 0;   // PDF tiles: width of the whole page at this zoom (px)
    int tx = 0;      // PDF tiles: column and row in the tile grid
    int ty = 0;
    bool isNull() const { return hash.isEmpty(); }
    bool isPdf() const { return kind != Image; }
    bool operator==(const MediaKey &o) const
    {
        return width == o.width && height == o.height && orientation == o.orientation && color == o.color
            && kind == o.kind && page == o.page && level == o.level && tx == o.tx && ty == o.ty && hash == o.hash;
    }
    bool operator!=(const MediaKey &o) const { return !(*this == o); }
};
size_t qHash(const MediaKey &k, size_t seed = 0);

} // namespace folia

Q_DECLARE_METATYPE(folia::MediaKey)
