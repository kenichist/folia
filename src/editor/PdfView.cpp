#include "editor/PdfView.h"

#include "app/Platform.h"
#include "core/Instrument.h"
#include "editor/EditorContext.h"

#include <QElapsedTimer>
#include <QKeyEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSet>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace folia {

namespace {

MediaKey tileKey(const QString &hash, int page, const PdfView::Plan &plan, int tile, int tx, int ty, QRect *region)
{
    const QRect r(tx * tile, ty * tile, qMin(tile, plan.width - tx * tile), qMin(tile, plan.height - ty * tile));
    if (region)
        *region = r;
    MediaKey k;
    k.hash = hash;
    k.kind = MediaKey::PdfTile;
    k.page = page;
    k.level = plan.width;
    k.tx = tx;
    k.ty = ty;
    k.width = r.width();
    k.height = r.height();
    return k;
}

MediaKey pageKey(const QString &hash, int page, const PdfView::Plan &plan)
{
    MediaKey k;
    k.hash = hash;
    k.kind = MediaKey::PdfPage;
    k.page = page;
    k.width = plan.width;
    k.height = plan.height;
    return k;
}

// Tile indices [x0, x1] × [y0, y1] covering `part` (content coordinates) of
// a page drawn at `target`, with tiles of `plan`. False when they miss.
bool tileRange(const PdfView::Plan &plan, int tile, const QRectF &target, const QRectF &part, int *x0, int *x1, int *y0,
               int *y1)
{
    const QRectF p = part & target;
    if (p.isEmpty() || target.width() <= 0)
        return false;
    const double s = plan.width / target.width(); // physical px of the plan per logical px
    const int nx = (plan.width + tile - 1) / tile;
    const int ny = (plan.height + tile - 1) / tile;
    *x0 = qBound(0, int(std::floor((p.left() - target.left()) * s / tile)), nx - 1);
    *x1 = qBound(0, int(std::ceil((p.right() - target.left()) * s / tile)) - 1, nx - 1);
    *y0 = qBound(0, int(std::floor((p.top() - target.top()) * s / tile)), ny - 1);
    *y1 = qBound(0, int(std::ceil((p.bottom() - target.top()) * s / tile)) - 1, ny - 1);
    return *x1 >= *x0 && *y1 >= *y0;
}

} // namespace

PdfView::PdfView(EditorContext *ctx, const AttachmentInfo &info, const QString &path, QWidget *parent)
    : PdfView(ctx, info, path, Config{}, parent)
{
}

PdfView::PdfView(EditorContext *ctx, const AttachmentInfo &info, const QString &path, const Config &cfg, QWidget *parent)
    : QAbstractScrollArea(parent), m_ctx(ctx), m_media(ctx ? ctx->media : nullptr), m_cfg(cfg), m_hash(info.hash),
      m_path(path)
{
    setObjectName(QStringLiteral("folia-pdf-view"));
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent); // every pixel is painted
    verticalScrollBar()->setSingleStep(40);
    horizontalScrollBar()->setSingleStep(40);
    init(info);
    m_zoomSettle.setSingleShot(true);
    connect(&m_zoomSettle, &QTimer::timeout, this, [this] {
        m_zooming = false; // refine: the new zoom is rendered now
        markDirty();
        viewport()->update();
    });
    m_flingSettle.setSingleShot(true);
    connect(&m_flingSettle, &QTimer::timeout, this, [this] {
        markDirty(); // slowed down: heavy pages are requested now
        viewport()->update();
    });
    m_scrollClock.start();
    m_hiddenClose.setSingleShot(true);
    connect(&m_hiddenClose, &QTimer::timeout, this, [this] {
        // In-flight jobs keep their own reference: the last one closes it.
        m_doc.reset();
    });
    if (m_media) {
        connect(m_media, &MediaService::arrived, this, &PdfView::arrived);
        connect(m_media, &MediaService::refused, this, &PdfView::arrived);
        connect(m_media, &MediaService::budgetChanged, this, [this] {
            markDirty(); // plans follow the budget
            if (isVisible())
                viewport()->update();
        });
    }
}

PdfView::~PdfView()
{
    if (m_media) {
        m_media->removeOwner(this); // queued renders for it are dropped
        m_media->dropViewerBitmaps(m_hash); // closed: its pages and tiles go too
    }
    m_doc.reset();
}

void PdfView::init(const AttachmentInfo &info)
{
    m_pages = attachments::decodePageGeom(info.pageGeom);
    if (m_pages.size() != info.pages && info.pages > 0 && info.width > 0 && info.height > 0) {
        // No usable geometry stored: every page like the first.
        PdfPageGeom g;
        g.w = float(info.width);
        g.h = float(info.height);
        m_pages.fill(g, info.pages);
    }
    m_cum.resize(m_pages.size() + 1);
    m_cum[0] = 0;
    m_maxW = 1;
    for (int i = 0; i < m_pages.size(); ++i) {
        m_cum[i + 1] = m_cum[i] + m_pages[i].h;
        m_maxW = qMax<double>(m_maxW, m_pages[i].w);
    }
}

// ------------------------------------------------------------ geometry ----
qreal PdfView::pageTop(int page) const
{
    return m_cfg.margin + m_zoom * m_cum[qBound(0, page, int(m_pages.size()))] + m_cfg.gap * page;
}

qreal PdfView::contentHeight() const
{
    const int n = pageCount();
    return n == 0 ? 0 : pageTop(n) - m_cfg.gap + m_cfg.margin;
}

qreal PdfView::contentWidth() const
{
    return qMax<qreal>(viewport()->width(), m_maxW * m_zoom + 2 * m_cfg.margin);
}

QRectF PdfView::pageRect(int page) const
{
    if (page < 0 || page >= pageCount())
        return QRectF();
    // Snapped to device pixels, as wide as the page bitmap at this zoom, so
    // a page or tile drawn at its own level is blitted without scaling.
    const PdfPageGeom &g = m_pages[page];
    const qreal d = m_dpr;
    const qreal w = qMax(1, qRound(g.w * m_zoom * d)) / d;
    const qreal h = qMax(1, qRound(g.h * m_zoom * d)) / d;
    const qreal x = std::round((contentWidth() - w) / 2 * d) / d;
    const qreal y = std::round(pageTop(page) * d) / d;
    return QRectF(x, y, w, h);
}

int PdfView::pageAt(qreal y) const
{
    // The last page whose top is at or above y: binary search over the
    // prefix sums (pageTop is monotonic).
    int lo = 0, hi = pageCount() - 1;
    if (hi < 0)
        return -1;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (pageTop(mid) <= y)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

QRectF PdfView::visibleRect() const
{
    return QRectF(contentPos(), QSizeF(viewport()->size()));
}

int PdfView::currentPage() const
{
    return pageAt(contentPos().y() + viewport()->height() / 2.0);
}

void PdfView::updateScrollBars()
{
    const QSize vs = viewport()->size();
    verticalScrollBar()->setPageStep(vs.height());
    verticalScrollBar()->setRange(0, qMax(0, int(std::ceil(contentHeight())) - vs.height()));
    horizontalScrollBar()->setPageStep(vs.width());
    horizontalScrollBar()->setRange(0, qMax(0, int(std::ceil(contentWidth())) - vs.width()));
}

// ---------------------------------------------------------- navigation ----
qreal PdfView::fitWidthZoom() const
{
    const qreal avail = qMax<qreal>(80, viewport()->width() - 2 * m_cfg.margin);
    return qBound(m_cfg.minZoom, avail / m_maxW, m_cfg.maxZoom);
}

void PdfView::fitWidth()
{
    setZoom(fitWidthZoom());
    m_fitOnShow = true; // keeps fitting as the window resizes
}

void PdfView::setZoom(qreal zoom, QPointF anchor)
{
    zoom = qBound(m_cfg.minZoom, zoom, m_cfg.maxZoom);
    m_fitOnShow = false;
    if (pageCount() == 0 || qFuzzyCompare(zoom, m_zoom)) {
        m_zoom = zoom;
        updateScrollBars();
        return;
    }
    const QPointF a = anchor.x() < 0 ? QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0) : anchor;
    // Keep the document point under the anchor where it is.
    const QPointF c = contentPos() + a;
    const int page = pageAt(c.y());
    const QRectF pr = pageRect(page);
    const double fx = (c.x() - pr.left()) / m_zoom;
    const double fy = (c.y() - pr.top()) / m_zoom;
    m_zoom = zoom;
    updateScrollBars();
    const QRectF npr = pageRect(page);
    horizontalScrollBar()->setValue(qRound(npr.left() + fx * zoom - a.x()));
    verticalScrollBar()->setValue(qRound(npr.top() + fy * zoom - a.y()));
    markDirty();
    viewport()->update();
    emit zoomChanged(m_zoom);
}

void PdfView::zoomGesture(qreal factor, QPointF anchor)
{
    if (!m_zooming) {
        // Existing pixels only while it moves: what was queued is dropped.
        m_zooming = true;
        if (m_media)
            m_media->setWanted(this, {});
        m_lastWants.clear();
    }
    const bool zooming = m_zooming;
    setZoom(m_zoom * factor, anchor);
    m_zooming = zooming;
    m_zoomSettle.start(m_cfg.zoomSettleMs);
}

void PdfView::goToPage(int page)
{
    if (pageCount() == 0)
        return;
    page = qBound(0, page, pageCount() - 1);
    verticalScrollBar()->setValue(qRound(pageRect(page).top() - m_cfg.margin));
}

void PdfView::scrollToY(qreal y)
{
    verticalScrollBar()->setValue(qRound(y));
}

// ------------------------------------------------------------ planning ----
PdfView::Plan PdfView::planFor(int page, qreal zoom) const
{
    Plan pl;
    if (page < 0 || page >= pageCount())
        return pl;
    const PdfPageGeom &g = m_pages[page];
    pl.width = qMax(1, qRound(g.w * zoom * m_dpr));
    pl.height = qMax(1, qRound(g.h * zoom * m_dpr));
    // One render never needs more than kMaxDecodeShare of the current
    // (possibly lowered) budget, as for images.
    const double room = m_media ? double(m_media->budget().limit()) * MediaService::kMaxDecodeShare : 1e18;
    const qint64 bytes = 4ll * pl.width * pl.height;
    if (bytes <= m_cfg.wholePageMaxBytes && double(MediaService::pdfEstimate(g, pl.width, pl.width, pl.height)) <= room)
        return pl; // one bitmap
    const int t = m_cfg.tileSize;
    if (double(MediaService::pdfEstimate(g, pl.width, qMin(t, pl.width), qMin(t, pl.height))) <= room) {
        pl.tiles = true;
        return pl;
    }
    // Not even a tile fits (a pressure cool-down, a huge scan): the whole
    // page at a lower density, drawn scaled up, rather than nothing at all.
    for (int i = 0; i < 64 && pl.width > 16 && double(MediaService::pdfEstimate(g, pl.width, pl.width, pl.height)) > room; ++i) {
        pl.width = qMax(16, int(pl.width * 0.85));
        pl.height = qMax(16, qRound(double(pl.width) * g.h / g.w));
    }
    return pl;
}

std::shared_ptr<platform::PdfDocument> PdfView::doc()
{
    if (!m_doc)
        m_doc = std::make_shared<platform::PdfDocument>(m_path); // opened by the first render, on the worker
    return m_doc;
}

QVector<MediaService::Request> PdfView::requestsFor(int page, const Plan &plan, const QRectF &want, int priority)
{
    QVector<MediaService::Request> out;
    const QRectF target = pageRect(page);
    if (!target.intersects(want))
        return out;
    if (!plan.tiles) {
        MediaService::Request r =
            MediaService::pdfRequest(m_hash, m_path, MediaKey::PdfPage, page, m_pages[page], plan.width, plan.height);
        r.pdf = doc();
        r.priority = priority;
        out << r;
        return out;
    }
    int x0, x1, y0, y1;
    if (!tileRange(plan, m_cfg.tileSize, target, want, &x0, &x1, &y0, &y1))
        return out;
    const QPointF c = (want & target).center();
    const double s = plan.width / target.width();
    struct T { double d; MediaService::Request r; };
    QVector<T> tiles;
    for (int ty = y0; ty <= y1; ++ty)
        for (int tx = x0; tx <= x1; ++tx) {
            QRect region;
            const MediaKey k = tileKey(m_hash, page, plan, m_cfg.tileSize, tx, ty, &region);
            MediaService::Request r = MediaService::pdfRequest(m_hash, m_path, MediaKey::PdfTile, page, m_pages[page],
                                                               k.width, k.height, k.level, tx, ty, region);
            r.pdf = doc();
            r.priority = priority;
            const QPointF tc(target.left() + (region.x() + region.width() / 2.0) / s,
                             target.top() + (region.y() + region.height() / 2.0) / s);
            tiles.push_back({std::hypot(tc.x() - c.x(), tc.y() - c.y()), r});
        }
    std::sort(tiles.begin(), tiles.end(), [](const T &a, const T &b) { return a.d < b.d; }); // middle first
    for (const T &t : std::as_const(tiles))
        out << t.r;
    return out;
}

void PdfView::markDirty()
{
    m_wantsDirty = true;
}

void PdfView::updateWants()
{
    m_wantsDirty = false;
    if (!m_media)
        return;
    if (!isVisible() || pageCount() == 0) {
        m_media->removeOwner(this);
        m_lastWants.clear();
        return;
    }
    if (m_zooming) { // existing pixels only, until the zoom settles
        m_media->setWanted(this, {});
        m_lastWants.clear();
        return;
    }
    const QRectF view = visibleRect();
    const int first = pageAt(view.top());
    const int last = pageAt(view.bottom());
    QVector<int> order;
    for (int i = first; i <= last; ++i)
        order << i;
    const qreal mid = view.center().y();
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return qAbs(pageRect(a).center().y() - mid) < qAbs(pageRect(b).center().y() - mid);
    });
    QVector<MediaService::Request> wants;
    QSet<MediaKey> seen;
    auto add = [&](const QVector<MediaService::Request> &rs) {
        for (const MediaService::Request &r : rs)
            if (!seen.contains(r.key)) {
                seen.insert(r.key);
                wants << r;
            }
    };
    const bool fling = m_flingSettle.isActive();
    for (int i : std::as_const(order))
        if (!fling || !m_pages[i].heavy())
            add(requestsFor(i, planFor(i, m_zoom), view, 0));
    // Prefetch: at most one viewport ahead in the scroll direction.
    const QRectF next = view.translated(0, m_scrollDir * view.height());
    const int nf = pageAt(next.top()), nl = pageAt(next.bottom());
    for (int k = 0; k <= nl - nf; ++k) {
        const int i = m_scrollDir > 0 ? nf + k : nl - k;
        if (!fling || !m_pages[i].heavy())
            add(requestsFor(i, planFor(i, m_zoom), next, 1));
    }
    m_media->setWanted(this, wants);
    m_lastWants = wants;
    // Forget what was shown on pages far away (keys only; cheap either way).
    if (m_shown.size() > 64)
        for (auto it = m_shown.begin(); it != m_shown.end();)
            it = (it.key() < first - 16 || it.key() > last + 16) ? m_shown.erase(it) : std::next(it);
}

// --------------------------------------------------------------- paint ----
bool PdfView::drawPlan(QPainter *p, int page, const Plan &plan, const QRectF &target, const QRectF &visible, bool smooth)
{
    if (!m_media || plan.isNull())
        return false;
    if (!plan.tiles) {
        const QImage *img = m_media->lookup(pageKey(m_hash, page, plan));
        if (!img)
            return false;
        if (p) {
            p->setRenderHint(QPainter::SmoothPixmapTransform, smooth);
            p->drawImage(target, *img);
        }
        return true;
    }
    int x0, x1, y0, y1;
    if (!tileRange(plan, m_cfg.tileSize, target, visible, &x0, &x1, &y0, &y1))
        return true;
    const double s = plan.width / target.width();
    bool all = true;
    if (p)
        p->setRenderHint(QPainter::SmoothPixmapTransform, smooth);
    for (int ty = y0; ty <= y1; ++ty)
        for (int tx = x0; tx <= x1; ++tx) {
            QRect region;
            const QImage *img = m_media->lookup(tileKey(m_hash, page, plan, m_cfg.tileSize, tx, ty, &region));
            if (!img) {
                all = false;
                continue;
            }
            if (p)
                p->drawImage(QRectF(target.left() + region.x() / s, target.top() + region.y() / s, region.width() / s,
                                    region.height() / s),
                             *img);
        }
    return all;
}

void PdfView::paintEvent(QPaintEvent *e)
{
    QElapsedTimer t;
    t.start();
    ++m_paints;
    const qreal dpr = viewport()->devicePixelRatioF();
    if (dpr > 0 && dpr != m_dpr) {
        m_dpr = dpr;
        markDirty();
    }
    QPainter p(viewport());
    const EditorTheme &th = m_ctx->theme();
    p.fillRect(e->rect(), th.codeBackground);
    if (pageCount() == 0) {
        p.setPen(th.dim);
        p.drawText(viewport()->rect(), Qt::AlignCenter, tr("Cannot show this PDF"));
        return;
    }
    const QPointF off = contentPos();
    const QRectF dirty = QRectF(e->rect()).translated(off);
    p.translate(-off);
    QColor edge = th.dim;
    edge.setAlpha(70);
    const int first = pageAt(dirty.top());
    const int last = pageAt(dirty.bottom());
    for (int i = first; i <= last; ++i) {
        const QRectF pr = pageRect(i);
        if (!pr.intersects(dirty))
            continue;
        const QRectF vis = pr & dirty;
        p.fillRect(pr.adjusted(-1, -1, 1, 1), edge);
        p.fillRect(vis, Qt::white);
        const Plan want = planFor(i, m_zoom);
        const bool reduced = !want.tiles && want.width != qRound(m_pages[i].w * m_zoom * m_dpr);
        QVector<Plan> &shown = m_shown[i];
        if (!m_zooming && drawPlan(nullptr, i, want, pr, vis, false)) {
            drawPlan(&p, i, want, pr, vis, reduced); // all there: drawn as is
            if (shown.isEmpty() || !(shown.first() == want)) {
                shown.removeAll(want);
                shown.prepend(want);
                if (shown.size() > 2)
                    shown.resize(2);
            }
            continue;
        }
        // Not (all) there yet, or zooming: what was last drawn in full,
        // scaled (oldest under newest), then whatever of the new plan exists.
        p.save();
        p.setClipRect(vis);
        for (int k = int(shown.size()) - 1; k >= 0; --k)
            if (!(shown[k] == want) || m_zooming)
                drawPlan(&p, i, shown[k], pr, vis, !m_zooming);
        if (!m_zooming)
            drawPlan(&p, i, want, pr, vis, reduced);
        p.restore();
    }
    p.end();
    if (m_wantsDirty)
        updateWants();
    instr::record("pdf_view_paint_us", t.nsecsElapsed() / 1000);
}

QRectF PdfView::keyRect(const MediaKey &key) const
{
    const QRectF pr = pageRect(key.page);
    if (key.kind != MediaKey::PdfTile || pr.isEmpty())
        return pr;
    const Plan now = planFor(key.page, m_zoom);
    if (!now.tiles || now.width != key.level)
        return pr;
    const double s = key.level / pr.width();
    return QRectF(pr.left() + key.tx * m_cfg.tileSize / s, pr.top() + key.ty * m_cfg.tileSize / s, key.width / s,
                  key.height / s);
}

void PdfView::arrived(const MediaKey &key)
{
    // Only this document's pages and tiles, only where they are shown.
    if (key.hash != m_hash || (key.kind != MediaKey::PdfPage && key.kind != MediaKey::PdfTile) || !isVisible())
        return;
    const QRectF r = keyRect(key) & visibleRect();
    if (!r.isEmpty())
        viewport()->update(r.translated(-contentPos()).toAlignedRect().adjusted(-1, -1, 1, 1));
}

// -------------------------------------------------------------- events ----
void PdfView::resizeEvent(QResizeEvent *e)
{
    QAbstractScrollArea::resizeEvent(e);
    if (m_fitOnShow && pageCount() > 0) {
        const int page = currentPage();
        const qreal into = (contentPos().y() - pageRect(page).top()) / m_zoom;
        m_zoom = fitWidthZoom();
        updateScrollBars();
        verticalScrollBar()->setValue(qRound(pageRect(page).top() + into * m_zoom));
        emit zoomChanged(m_zoom);
    } else {
        updateScrollBars();
    }
    markDirty();
}

void PdfView::scrollContentsBy(int dx, int dy)
{
    if (dy < 0)
        m_scrollDir = 1;
    else if (dy > 0)
        m_scrollDir = -1;
    Q_UNUSED(dx);
    if (dy != 0) {
        // Speed over the last 100 ms: a fling defers heavy pages.
        const qint64 now = m_scrollClock.elapsed();
        const int y = verticalScrollBar()->value();
        m_scrollTrail.push_back({now, y});
        while (!m_scrollTrail.isEmpty() && now - m_scrollTrail.first().first > 100)
            m_scrollTrail.removeFirst();
        if (qAbs(y - m_scrollTrail.first().second) > viewport()->height() / 4)
            m_flingSettle.start(m_cfg.flingSettleMs);
    }
    markDirty();
    viewport()->update();
    const int cur = currentPage();
    if (cur != m_lastCurrent) {
        m_lastCurrent = cur;
        emit currentPageChanged(cur);
    }
}

bool PdfView::viewportEvent(QEvent *e)
{
    if (e->type() == QEvent::NativeGesture) {
        auto *g = static_cast<QNativeGestureEvent *>(e);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            zoomGesture(1.0 + g->value(), g->position());
            return true;
        }
        if (g->gestureType() == Qt::SmartZoomNativeGesture) {
            if (qFuzzyCompare(m_zoom, fitWidthZoom()))
                setZoom(m_zoom * 2, g->position());
            else
                fitWidth();
            return true;
        }
    }
    return QAbstractScrollArea::viewportEvent(e);
}

void PdfView::wheelEvent(QWheelEvent *e)
{
    if (e->modifiers() & Qt::ControlModifier) { // Mod+wheel zooms
        const int dy = e->angleDelta().y() != 0 ? e->angleDelta().y() : e->pixelDelta().y();
        if (dy != 0)
            zoomGesture(std::pow(1.0015, dy), e->position());
        e->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(e);
}

void PdfView::keyPressEvent(QKeyEvent *e)
{
    const bool mod = e->modifiers() & Qt::ControlModifier;
    switch (e->key()) {
    case Qt::Key_Home:
        goToPage(0);
        return;
    case Qt::Key_End:
        goToPage(pageCount() - 1);
        return;
    case Qt::Key_Space:
        verticalScrollBar()->triggerAction((e->modifiers() & Qt::ShiftModifier) ? QAbstractSlider::SliderPageStepSub
                                                                                 : QAbstractSlider::SliderPageStepAdd);
        return;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        if (mod) {
            zoomGesture(1.25);
            return;
        }
        break;
    case Qt::Key_Minus:
        if (mod) {
            zoomGesture(0.8);
            return;
        }
        break;
    case Qt::Key_0:
        if (mod) {
            fitWidth();
            return;
        }
        break;
    default:
        break;
    }
    QAbstractScrollArea::keyPressEvent(e);
}

void PdfView::showEvent(QShowEvent *e)
{
    QAbstractScrollArea::showEvent(e);
    m_hiddenClose.stop();
    if (const qreal d = viewport()->devicePixelRatioF(); d > 0)
        m_dpr = d;
    if (m_fitOnShow && pageCount() > 0) {
        m_zoom = fitWidthZoom();
        emit zoomChanged(m_zoom);
    }
    updateScrollBars();
    markDirty();
}

void PdfView::hideEvent(QHideEvent *e)
{
    QAbstractScrollArea::hideEvent(e);
    if (m_media)
        m_media->removeOwner(this); // nothing renders for a hidden viewer
    m_lastWants.clear();
    m_zoomSettle.stop();
    m_zooming = false;
    m_hiddenClose.start(m_cfg.closeHiddenAfterMs);
}

} // namespace folia
