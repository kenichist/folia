// The PDF viewer's viewport (docs/ATTACHMENTS.md §4): one custom scroll
// area, not a widget per page.
//
//  * Page offsets come from the stored page geometry (page_geom): a prefix
//    sum of page heights, and a binary search for the pages in view. Nothing
//    is rendered or opened to lay pages out.
//  * At normal zoom each visible page is one bitmap; past the whole-page
//    threshold (or when a whole page would not fit the current budget) only
//    the visible tiles are rendered, each bounded in both dimensions and
//    drawn straight into its own tile-sized bitmap.
//  * While a zoom (pinch, Mod+wheel, keys) is in progress, the pixels already
//    cached are drawn scaled and nothing new is requested; the new zoom is
//    rendered once it has settled.
//  * Bitmaps come from MediaService (one budget, the PDF lane, one render at
//    a time, so calls on the document are serialized). Requests that scroll
//    away are dropped before they start; during a fling, heavy pages are not
//    requested at all until the view slows down. The viewer keeps keys,
//    never pixels.
//  * Its invalidation is its own: only results for this document, only the
//    page or tile they belong to, only while it is visible. The note editor
//    never repaints it and it never repaints the note editor.
//  * The document handle is shared with the render jobs and opened on the
//    worker. It closes when the viewer is destroyed, or once it has been
//    hidden for `closeHiddenAfterMs` (reopened lazily when shown again).
//    Destroying the viewer also frees its pages and tiles from the cache.
#pragma once

#include "core/Attachments.h"
#include "editor/MediaKey.h"
#include "editor/MediaService.h"

#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QHash>
#include <QScrollBar>
#include <QTimer>
#include <QVector>

#include <memory>

namespace folia {

class EditorContext;

class PdfView : public QAbstractScrollArea {
    Q_OBJECT
public:
    struct Config {
        // Measured (docs/BENCHMARKS.md §6, "PDF render cost"): 512 px tiles
        // bound CoreGraphics' own working set too (transparency layers are
        // allocated at the clipped size: ~11 MiB per tile against ~45 MiB for
        // 1024 px tiles and ~157 MiB for a whole 2000 px page), and what the
        // allocator keeps after a render follows that working set. A whole
        // page is cheaper in CPU only while it is small: at 1.8 MiB it peaks
        // ~1.5× a tile's working set, at 7 MiB already ~5×.
        int tileSize = 512;                      // physical px, both dimensions
        qint64 wholePageMaxBytes = 2ll << 20;    // a page bitmap above this is tiled
        int zoomSettleMs = 150;                  // a zoom is rendered once it stops changing
        // Moving faster than a quarter viewport per 100 ms (a heavy tile takes
        // 30-250 ms, so it would be out of view when done), heavy pages (layers,
        // big images, long content streams) are not requested until the view
        // slows down for this long: a render cannot be interrupted, and one
        // started mid-fling is thrown away when it finishes.
        // 150 ms, like the zoom settle: a fling whose frames come up to
        // ~7 per second apart (a busy machine, a dragged scroll bar) still
        // counts as one.
        int flingSettleMs = 150;
        int closeHiddenAfterMs = 30000;          // hidden this long: the document closes
        qreal margin = 16;                       // logical px around the pages
        qreal gap = 12;                          // between pages
        qreal minZoom = 0.25;                    // logical px per PDF point
        qreal maxZoom = 8.0;
    };
    // How one page is drawn at one zoom.
    struct Plan {
        bool tiles = false;
        int width = 0;   // physical px of the whole page at this zoom (the tile level)
        int height = 0;
        bool operator==(const Plan &o) const { return tiles == o.tiles && width == o.width && height == o.height; }
        bool isNull() const { return width <= 0; }
    };

    PdfView(EditorContext *ctx, const AttachmentInfo &info, const QString &path, QWidget *parent = nullptr);
    PdfView(EditorContext *ctx, const AttachmentInfo &info, const QString &path, const Config &cfg,
            QWidget *parent = nullptr);
    ~PdfView() override;

    const Config &config() const { return m_cfg; }
    QString hash() const { return m_hash; }
    int pageCount() const { return int(m_pages.size()); }
    const QVector<PdfPageGeom> &pages() const { return m_pages; }

    // ---- geometry (logical px, content coordinates)
    qreal zoom() const { return m_zoom; }
    qreal pageTop(int page) const;
    QRectF pageRect(int page) const;
    int pageAt(qreal y) const; // binary search over the prefix sums
    qreal contentHeight() const;
    qreal contentWidth() const;
    int currentPage() const;   // the page at the middle of the view
    QRectF visibleRect() const;

    // ---- navigation
    void setZoom(qreal zoom, QPointF anchor = QPointF(-1, -1)); // anchor: viewport coordinates
    // A step of a continuous zoom (pinch, Mod+wheel, repeated keys): drawn
    // from cached pixels, rendered after zoomSettleMs without another step.
    void zoomGesture(qreal factor, QPointF anchor = QPointF(-1, -1));
    void fitWidth();
    qreal fitWidthZoom() const;
    void goToPage(int page);
    void scrollToY(qreal y);
    bool zooming() const { return m_zooming; }
    bool flinging() const { return m_flingSettle.isActive(); }

    // ---- rendering plan (tests, benchmarks)
    Plan planFor(int page, qreal zoom) const;
    qreal devicePixelRatioUsed() const { return m_dpr; }
    // Recompute what this viewer wants (normally after a paint).
    void updateWants();
    const QVector<MediaService::Request> &lastWants() const { return m_lastWants; }
    bool documentOpen() const { return m_doc != nullptr; }
    std::weak_ptr<platform::PdfDocument> document() const { return m_doc; }
    int paintCount() const { return m_paints; }
    // What the viewport shows with the cache as it is now (benchmarks): the
    // fraction of the visible page area with no pixels yet (white) and the
    // fraction drawn only from an older plan, scaled (blurry until sharp).
    struct Coverage {
        double blank = 0;
        double scaled = 0;
    };
    Coverage viewportCoverage() const;

signals:
    void currentPageChanged(int page);
    void zoomChanged(qreal zoom);

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void scrollContentsBy(int dx, int dy) override;
    bool viewportEvent(QEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    void init(const AttachmentInfo &info);
    void updateScrollBars();
    std::shared_ptr<platform::PdfDocument> doc();
    QVector<MediaService::Request> requestsFor(int page, const Plan &plan, const QRectF &want, int priority);
    // Draws what the cache holds of `plan` into `target` (the page's rect at
    // the current zoom); returns true when it covered all of `visible`.
    bool drawPlan(QPainter *p, int page, const Plan &plan, const QRectF &target, const QRectF &visible, bool smooth);
    // The part of `visible` (logical px²) that the cache holds nothing of `plan` for.
    double missingArea(int page, const Plan &plan, const QRectF &target, const QRectF &visible) const;
    void arrived(const MediaKey &key);
    QRectF keyRect(const MediaKey &key) const; // content coordinates, at the current zoom
    QPointF contentPos() const { return QPointF(horizontalScrollBar()->value(), verticalScrollBar()->value()); }
    void markDirty();

    EditorContext *m_ctx;
    MediaService *m_media;
    Config m_cfg;
    QString m_hash;
    QString m_path;
    QVector<PdfPageGeom> m_pages;
    QVector<double> m_cum; // m_cum[i] = sum of page heights (pt) before page i
    double m_maxW = 1;     // widest page (pt)
    qreal m_zoom = 1;
    qreal m_dpr = 1;
    bool m_fitOnShow = true;
    bool m_zooming = false;
    bool m_wantsDirty = true;
    int m_scrollDir = 1;
    int m_lastCurrent = -1;
    int m_paints = 0;
    QTimer m_zoomSettle;
    QTimer m_flingSettle;
    QElapsedTimer m_scrollClock;
    QVector<QPair<qint64, int>> m_scrollTrail; // (ms, scroll value): the last 100 ms and one sample before
    QTimer m_hiddenClose;
    std::shared_ptr<platform::PdfDocument> m_doc;
    // Per page, the last two plans drawn in full (newest first): drawn
    // scaled under a plan that is not all there yet, e.g. at a new zoom
    // (keys only, never pixels).
    QHash<int, QVector<Plan>> m_shown;
    QVector<MediaService::Request> m_lastWants;
};

} // namespace folia
