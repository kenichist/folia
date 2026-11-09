#include "editor/VirtualLayout.h"

#include "core/Instrument.h"
#include "core/LucideIcons.h"
#include "editor/Links.h"
#include "editor/MediaService.h"
#include "storage/AttachmentStore.h"
#ifdef FOLIA_WITH_MATH
#include "math/MathRenderer.h"
#endif

#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPaintDevice>
#include <QPaintEngine>
#include <QScreen>
#include <QPainter>
#include <QPainterPath>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextLayout>
#include <QTextObjectInterface>

#include <algorithm>
#include <cmath>
#include <optional>

namespace folia {

namespace {

constexpr qreal kBottomSlack = 160; // scroll past the end
constexpr qreal kCodeInset = 14;
constexpr qreal kPreviewPad = 8;
constexpr qreal kCopyIcon = 13;
constexpr qreal kInlinePad = 1.5; // space on each side of a formula drawn in place
constexpr qreal kImageTop = 6;     // above an image line
constexpr qreal kImageBelow = 6;   // below it, before the paragraph spacing
constexpr qreal kImportH = 56;     // placeholder of attachments being imported
constexpr qreal kPdfCardH = 132;   // a PDF line: first-page thumbnail, name, page count
constexpr qreal kPdfPad = 10;
constexpr qreal kPdfCardMaxW = 460;
constexpr int kResizeSettleMs = 150; // decodes at a new width wait this long
constexpr qreal kCodePad = 3;     // hidden backticks keep this much of the code background per side
constexpr qreal kFenceSliver = 6; // height of a ``` / $$ line whose marker is hidden

// Lines holding only a fence marker, laid out as a sliver while it is hidden.
bool collapsibleFence(LineRole r)
{
    return r == LineRole::CodeClose || r == LineRole::MathOpen || r == LineRole::MathClose;
}

LineRole roleOf(const QTextBlock &b)
{
    const int s = b.userState();
    return s < 0 ? LineRole::Para : LineRole(s);
}

BlockData *dataOf(const QTextBlock &b) { return static_cast<BlockData *>(b.userData()); }

QString imageHashOf(const QTextBlock &b)
{
    return b.begin().fragment().charFormat().property(kAttachmentHashProperty).toString();
}

QString formatSize(qint64 bytes)
{
    if (bytes >= (1ll << 20))
        return QStringLiteral("%1 MB").arg(double(bytes) / (1 << 20), 0, 'f', 1);
    return QStringLiteral("%1 KB").arg(qMax<qint64>(1, (bytes + 1023) / 1024));
}

int headingLevel(QStringView t)
{
    int n = 0;
    while (n < t.size() && n < 4 && t[n] == QLatin1Char('#'))
        ++n;
    if (n >= 1 && n <= 3 && n < t.size() && t[n] == QLatin1Char(' '))
        return n;
    return 0;
}

int listMarkerLength(QStringView t)
{
    if (t.size() >= 2 && (t[0] == QLatin1Char('-') || t[0] == QLatin1Char('*') || t[0] == QLatin1Char('+'))
        && t[1] == QLatin1Char(' '))
        return 2;
    int i = 0;
    while (i < t.size() && i < 4 && t[i].isDigit())
        ++i;
    if (i > 0 && i + 1 < t.size() && t[i] == QLatin1Char('.') && t[i + 1] == QLatin1Char(' '))
        return i + 2;
    return 0;
}

QTextCharFormat marked(QTextCharFormat f)
{
    f.setProperty(kLayoutFormatMarker, true);
    return f;
}

// Qt only treats U+FFFC as an inline object when the layout has a handler
// registered (QAbstractTextDocumentLayoutPrivate::hasHandlers). Sizing and
// painting are done by VirtualLayout's overrides; this just opts in.
class PageLinkHandler : public QObject, public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)
public:
    using QObject::QObject;
    QSizeF intrinsicSize(QTextDocument *, int, const QTextFormat &) override { return QSizeF(80, 20); }
    void drawObject(QPainter *, const QRectF &, QTextDocument *, int, const QTextFormat &) override {}
};

} // namespace

VirtualLayout::VirtualLayout(QTextDocument *doc, EditorContext *ctx)
    : QAbstractTextDocumentLayout(doc), m_ctx(ctx)
{
    setPaintDevice(nullptr);
    registerHandler(kPageLinkObjectType, new PageLinkHandler(this));
    registerHandler(kImageObjectType, new PageLinkHandler(this)); // sized/painted below too
    registerHandler(kPdfObjectType, new PageLinkHandler(this));
    // Formulas rendered on the worker are shown in one repaint per burst,
    // a couple of frames later, never right after the frame that asked.
    m_inlineRepaint.setSingleShot(true);
    m_inlineRepaint.setInterval(40);
    connect(&m_inlineRepaint, &QTimer::timeout, this, [this] {
        emit update(QRectF(0, m_viewTop, 1e9, qMax<qreal>(1, m_viewBottom - m_viewTop)));
    });
    if (QScreen *sc = QGuiApplication::primaryScreen())
        m_dpr = sc->devicePixelRatio();
    m_widthSettle.setSingleShot(true);
    m_widthSettle.setInterval(kResizeSettleMs);
    connect(&m_widthSettle, &QTimer::timeout, this, &VirtualLayout::reestimate);
    m_mediaSettle.setSingleShot(true);
    connect(&m_mediaSettle, &QTimer::timeout, this, [this] {
        m_mediaDirty = true;
        updateMediaWants();
    });
    if (MediaService *media = m_ctx->media) {
        // Only what lines here show: images and PDF thumbnails, never a
        // viewer's pages or tiles.
        connect(media, &MediaService::arrived, this, [this](const MediaKey &k) { mediaArrived(k); });
        connect(media, &MediaService::refused, this, [this](const MediaKey &k) { mediaArrived(k); });
        connect(media, &MediaService::budgetChanged, this, [this] {
            m_mediaDirty = true; // request sizes follow the budget
            emit update(QRectF(0, m_viewTop, 1e9, qMax<qreal>(1, m_viewBottom - m_viewTop)));
        });
    }
    if (AttachmentStore *as = m_ctx->attachments) {
        connect(as, &AttachmentStore::infoArrived, this, [this](const QString &hash) {
            // Sizes are known now: lay out the lines showing it again.
            QTextDocument *d = document();
            for (QTextBlock b = d->firstBlock(); b.isValid(); b = b.next())
                if (roleIsAttachment(roleOf(b)) && imageHashOf(b) == hash) {
                    b.layout()->clearLayout();
                    relayoutBlock(b);
                }
            m_mediaDirty = true;
        });
    }
}

VirtualLayout::~VirtualLayout()
{
    if (m_ctx->media)
        m_ctx->media->removeOwner(this);
}

// ----------------------------------------------------------- geometry -----
qreal VirtualLayout::columnWidth() const
{
    const EditorTheme &t = m_ctx->theme();
    qreal avail = m_width - 2 * t.sideMargin;
    if (t.maxColumnWidth > 0)
        avail = qMin(avail, t.maxColumnWidth);
    return qMax<qreal>(avail, 160);
}

qreal VirtualLayout::columnLeft() const
{
    return qMax(m_ctx->theme().sideMargin, std::floor((m_width - columnWidth()) / 2));
}

qreal VirtualLayout::indentFor(LineRole role) const
{
    switch (role) {
    case LineRole::CodeOpen:
    case LineRole::CodeLine:
    case LineRole::CodeClose:
    case LineRole::MathOpen:
    case LineRole::MathLine:
    case LineRole::MathClose:
    case LineRole::MathSingle:
        return kCodeInset;
    default:
        return 0;
    }
}

qreal VirtualLayout::topPadFor(const QTextBlock &block, LineRole role) const
{
    switch (role) {
    case LineRole::CodeOpen:
    case LineRole::MathOpen:
    case LineRole::MathSingle:
        return 10;
    case LineRole::Image:
    case LineRole::Pdf:
        return kImageTop;
    case LineRole::Para: {
        const int h = headingLevel(block.text());
        return h == 1 ? 16 : h == 2 ? 12 : h == 3 ? 8 : 0;
    }
    default:
        return 0;
    }
}

qreal VirtualLayout::bottomPadFor(LineRole role) const
{
    const qreal ps = m_ctx->theme().paragraphSpacing;
    switch (role) {
    case LineRole::CodeOpen:
    case LineRole::CodeLine:
    case LineRole::MathOpen:
    case LineRole::MathLine:
        return 0;
    case LineRole::CodeClose:
    case LineRole::MathClose:
    case LineRole::MathSingle:
        return 10 + ps;
    case LineRole::Image:
    case LineRole::Pdf:
        return kImageBelow + ps;
    default:
        return ps;
    }
}

qreal VirtualLayout::extraBelow(const QTextBlock &block) const
{
    const BlockData *d = dataOf(block);
    return d && d->pendingImports > 0 ? kImportH : 0;
}

qreal VirtualLayout::objectHeight(const QTextBlock &block, LineRole role) const
{
    return role == LineRole::Pdf ? pdfCardSize().height() : imageDisplaySize(imageHashOf(block)).height();
}

QSizeF VirtualLayout::imageDisplaySize(const QString &hash) const
{
    const qreal col = columnWidth();
    const AttachmentInfo *info = m_ctx->attachments ? m_ctx->attachments->info(hash) : nullptr;
    if (!info || info->width <= 0 || info->height <= 0) {
        // Not known yet (or missing): a modest box; relaid when it arrives.
        if (m_ctx->attachments)
            m_ctx->attachments->fetchInfo(hash);
        return QSizeF(qMin<qreal>(col, 320), 120);
    }
    const qreal w = qMin<qreal>(col, info->width);
    return QSizeF(w, qMax<qreal>(1, std::round(w * info->height / info->width)));
}

QRectF VirtualLayout::imageRect(const QTextBlock &block) const
{
    const LineRole role = roleOf(block);
    if (!roleIsAttachment(role))
        return QRectF();
    const QRectF br = blockBoundingRect(block);
    const QSizeF sz = role == LineRole::Pdf ? pdfCardSize() : imageDisplaySize(imageHashOf(block));
    return QRectF(QPointF(columnLeft(), br.top() + kImageTop), sz);
}

QSizeF VirtualLayout::pdfCardSize() const
{
    return QSizeF(qMin<qreal>(columnWidth(), kPdfCardMaxW), kPdfCardH);
}

QRectF VirtualLayout::pdfThumbRect(const QRectF &card, const QString &hash) const
{
    // Width from the stored first-page geometry (rotation applied): layout
    // never waits for a render.
    const AttachmentInfo *info = m_ctx->attachments ? m_ctx->attachments->info(hash) : nullptr;
    const qreal aspect = info && info->width > 0 && info->height > 0 ? qreal(info->width) / info->height : 612.0 / 792.0;
    const qreal th = card.height() - 2 * kPdfPad;
    const qreal maxW = qMax<qreal>(th * 0.4, qMin<qreal>(th * 1.6, card.width() * 0.45));
    const qreal tw = qBound<qreal>(th * 0.4, std::round(th * aspect), maxW);
    return QRectF(card.left() + kPdfPad, card.top() + kPdfPad, tw, th);
}

MediaService::Request VirtualLayout::pdfThumbRequest(const QString &hash, const QRectF &thumb) const
{
    const AttachmentStore *as = m_ctx->attachments;
    const AttachmentInfo *info = as ? as->info(hash) : nullptr;
    const int w = qMax(1, int(std::round(thumb.width() * m_dpr)));
    const int h = qMax(1, int(std::round(thumb.height() * m_dpr)));
    const PdfPageGeom first = info ? attachments::firstPageGeom(info->pageGeom) : PdfPageGeom();
    return MediaService::pdfRequest(hash, as ? as->pathFor(hash) : QString(), MediaKey::PdfThumb, 0, first, w, h);
}

QFont VirtualLayout::roleFont(const QTextBlock &block, LineRole role) const
{
    const EditorTheme &t = m_ctx->theme();
    switch (role) {
    case LineRole::CodeOpen:
    case LineRole::CodeLine:
    case LineRole::CodeClose:
    case LineRole::MathOpen:
    case LineRole::MathLine:
    case LineRole::MathClose:
    case LineRole::MathSingle:
        return t.mono;
    case LineRole::Para: {
        const int h = headingLevel(block.text());
        if (h) {
            QFont f = t.body;
            f.setPointSizeF(t.body.pointSizeF() * (h == 1 ? t.h1Scale : h == 2 ? t.h2Scale : t.h3Scale));
            f.setWeight(h == 3 ? QFont::DemiBold : QFont::Bold);
            return f;
        }
        return t.body;
    }
    default:
        return t.body;
    }
}

float VirtualLayout::estimateHeight(const QTextBlock &block) const
{
    instr::add(instr::BlockHeightEstimates);
    const LineRole role = roleOf(block);
    if (roleIsAttachment(role)) // exact: the same arithmetic as layoutBlock()
        return float(std::ceil(topPadFor(block, role) + objectHeight(block, role) + extraBelow(block) + bottomPadFor(role)));
    const QFont f = roleFont(block, role);
    const QFontMetricsF fm(f);
    const qreal lw = qMax<qreal>(40, columnWidth() - 2 * indentFor(role));
    const qreal chars = qMax(0, block.length() - 1);
    const qreal lines = qMax<qreal>(1, std::ceil(chars * fm.averageCharWidth() / lw));
    // A fence line is a sliver unless the caret is on it (and then it is laid out).
    qreal h = topPadFor(block, role)
        + (collapsibleFence(role) ? kFenceSliver : lines * fm.lineSpacing() * m_ctx->theme().lineSpacing)
        + bottomPadFor(role);
    if (role == LineRole::MathClose || role == LineRole::MathSingle) {
        BlockData *d = dataOf(block);
        h += (d && d->previewHeight > 0) ? d->previewHeight : 2 * fm.lineSpacing();
    }
    return float(h + extraBelow(block));
}

void VirtualLayout::setHeight(int number, float h) const
{
    float &cur = m_h[size_t(number)];
    if (cur == h)
        return;
    m_sumH += double(h) - double(cur);
    cur = h;
    if (number + 1 < m_topValid)
        m_topValid = number + 1;
    if (m_inPaint)
        m_heightChangedInPaint = true;
}

void VirtualLayout::ensureTops(int upTo) const
{
    const int n = int(m_h.size());
    if (n == 0)
        return;
    upTo = qMin(upTo, n - 1);
    if (int(m_top.size()) != n)
        m_top.resize(size_t(n));
    if (m_topValid == 0) {
        m_top[0] = m_ctx->theme().topMargin;
        m_topValid = 1;
    }
    for (int i = m_topValid; i <= upTo; ++i)
        m_top[size_t(i)] = m_top[size_t(i - 1)] + double(m_h[size_t(i - 1)]);
    m_topValid = qMax(m_topValid, upTo + 1);
}

qreal VirtualLayout::topOf(int number) const
{
    ensureTops(number);
    return m_top[size_t(number)];
}

int VirtualLayout::blockAtY(qreal y) const
{
    const int n = int(m_h.size());
    if (n == 0)
        return -1;
    ensureTops(0);
    // Extend the prefix only as far as needed.
    while (m_topValid < n && m_top[size_t(m_topValid - 1)] + m_h[size_t(m_topValid - 1)] <= y)
        ensureTops(m_topValid);
    auto begin = m_top.begin();
    auto end = m_top.begin() + m_topValid;
    auto it = std::upper_bound(begin, end, double(y));
    int i = int(it - begin) - 1;
    return qBound(0, i, n - 1);
}

QSizeF VirtualLayout::documentSize() const
{
    return QSizeF(m_width, m_sumH + m_ctx->theme().topMargin + kBottomSlack);
}

QRectF VirtualLayout::frameBoundingRect(QTextFrame *) const
{
    return QRectF(QPointF(0, 0), documentSize());
}

QRectF VirtualLayout::blockBoundingRect(const QTextBlock &block) const
{
    if (!block.isValid())
        return QRectF();
    const int n = block.blockNumber();
    if (n < 0 || n >= int(m_h.size()))
        return QRectF();
    if (!(m_st[size_t(n)] & StLaidOut) || block.layout()->lineCount() == 0)
        layoutBlock(block, n);
    return QRectF(0, topOf(n), m_width, m_h[size_t(n)]);
}

qint64 VirtualLayout::estimatedLayoutBytes() const
{
    // Measured order of magnitude for shaped Latin text in Qt 6: ~40 bytes
    // per laid-out character plus the per-block vectors kept here.
    return qint64(m_h.size()) * (sizeof(float) + sizeof(uint8_t) + sizeof(double))
        + qint64(m_laidOut) * 4096;
}

// --------------------------------------------------------- invalidation ---
void VirtualLayout::fullInvalidate()
{
    m_mediaDirty = true;
    m_estimatesStale = false; // every height is estimated now
    m_widthSettle.stop();
    QTextDocument *doc = document();
    const int n = doc->blockCount();
    // QTextBlock::layout() allocates a QTextLayout on first use, so asking
    // every block for one costs ~280 bytes per line that is never drawn.
    // Only layoutBlock() creates lines and it always sets StLaidOut, so the
    // old flags say which blocks hold lines — when they still line up with
    // the document. If the block count moved under us, check every block.
    const std::vector<uint8_t> was = std::move(m_st);
    const bool trustFlags = int(was.size()) == n;
    m_h.assign(size_t(n), 0.f);
    m_st.assign(size_t(n), 0);
    m_top.assign(size_t(n), 0.0);
    m_topValid = 0;
    m_sumH = 0;
    m_laidOut = 0;
    int i = 0;
    for (QTextBlock b = doc->begin(); b.isValid() && i < n; b = b.next(), ++i) {
        if ((!trustFlags || (was[size_t(i)] & StLaidOut)) && b.layout()->lineCount() > 0)
            b.layout()->clearLayout();
        const float h = estimateHeight(b);
        m_h[size_t(i)] = h;
        m_sumH += h;
    }
    emit documentSizeChanged(documentSize());
    emit update(QRectF(0, 0, 1e9, 1e9));
}

void VirtualLayout::resizeInvalidate(bool columnChanged)
{
    m_mediaDirty = true;
    QTextDocument *doc = document();
    const size_t n = m_st.size();
    for (size_t i = 0; i < n && m_laidOut > 0; ++i) {
        if (!(m_st[i] & StLaidOut))
            continue;
        QTextBlock b = doc->findBlockByNumber(int(i));
        if (b.isValid() && b.layout()->lineCount() > 0)
            b.layout()->clearLayout(); // lines (and x positions) for the old width
        m_st[i] &= uint8_t(~StLaidOut);
        --m_laidOut;
    }
    if (columnChanged) {
        m_estimatesStale = true;
        m_widthSettle.start(); // restarted by every step of a live resize
    }
    emit documentSizeChanged(documentSize());
    emit update(QRectF(0, 0, 1e9, 1e9));
}

void VirtualLayout::reestimate()
{
    if (!m_estimatesStale)
        return;
    m_estimatesStale = false;
    QTextDocument *doc = document();
    const int n = doc->blockCount();
    if (n != int(m_h.size())) {
        fullInvalidate();
        return;
    }
    double sum = 0;
    int i = 0;
    for (QTextBlock b = doc->begin(); b.isValid() && i < n; b = b.next(), ++i) {
        if (!(m_st[size_t(i)] & StLaidOut))
            m_h[size_t(i)] = estimateHeight(b);
        sum += m_h[size_t(i)];
    }
    m_sumH = sum;
    m_topValid = 0;
    emit documentSizeChanged(documentSize());
    emit update(QRectF(0, 0, 1e9, 1e9));
}

void VirtualLayout::relayoutAll()
{
    fullInvalidate();
}

void VirtualLayout::relayoutBlock(const QTextBlock &block)
{
    if (!block.isValid())
        return;
    const int n = block.blockNumber();
    if (n < 0 || n >= int(m_h.size()))
        return;
    const float before = m_h[size_t(n)];
    layoutBlock(block, n);
    const qreal top = topOf(n);
    if (m_h[size_t(n)] != before) {
        emit documentSizeChanged(documentSize());
        emit update(QRectF(0, top, 1e9, 1e9));
    } else {
        emit updateBlock(block);
        emit update(QRectF(0, top, 1e9, m_h[size_t(n)]));
    }
}

void VirtualLayout::documentChanged(int from, int charsRemoved, int charsAdded)
{
    QTextDocument *doc = document();
    qreal w = doc->pageSize().width();
    if (w <= 0)
        w = 800;
    const int n = doc->blockCount();
    m_mediaDirty = true;
    if (w != m_width || m_h.empty()) {
        if (!m_h.empty() && int(m_h.size()) == n) {
            m_widthChanged.start(); // a resize: coalesce image decodes
            const qreal column = columnWidth();
            m_width = w;
            resizeInvalidate(columnWidth() != column);
            return;
        }
        if (!m_h.empty())
            m_widthChanged.start();
        m_width = w;
        fullInvalidate();
        return;
    }
    // QTextDocument::setPageSize() (sent by QTextEdit on every setDocument)
    // reports the whole document as (0, 0, length). With an unchanged width
    // and block count nothing needs re-layout: this keeps warm page switches
    // from discarding every cached block layout.
    if (from == 0 && charsRemoved == 0 && charsAdded == doc->characterCount() && n == int(m_h.size()))
        return;
    QTextBlock sb = doc->findBlock(from);
    QTextBlock eb = doc->findBlock(from + charsAdded);
    if (!sb.isValid())
        sb = doc->lastBlock();
    if (!eb.isValid())
        eb = doc->lastBlock();
    const int first = sb.blockNumber();
    const int lastNew = qMax(first, eb.blockNumber());
    const int delta = n - int(m_h.size());
    const int lastOld = lastNew - delta;
    if (lastOld < first - 1 || lastOld >= int(m_h.size())) {
        fullInvalidate(); // inconsistent notification; be safe
        return;
    }
    // Splice per-block vectors: [first, lastOld] -> [first, lastNew].
    // Lines can only survive in blocks that were laid out before (newly
    // inserted blocks start without a QTextLayout), so if none were, the
    // estimate path below need not touch — and allocate — any layout.
    bool anyWasLaidOut = false;
    for (int i = first; i <= lastOld; ++i) {
        m_sumH -= m_h[size_t(i)];
        if (m_st[size_t(i)] & StLaidOut) {
            --m_laidOut;
            anyWasLaidOut = true;
        }
    }
    const int oldCount = lastOld - first + 1;
    const int newCount = lastNew - first + 1;
    if (newCount != oldCount) {
        if (newCount > oldCount) {
            m_h.insert(m_h.begin() + first, size_t(newCount - oldCount), 0.f);
            m_st.insert(m_st.begin() + first, size_t(newCount - oldCount), 0);
        } else {
            m_h.erase(m_h.begin() + first, m_h.begin() + first + (oldCount - newCount));
            m_st.erase(m_st.begin() + first, m_st.begin() + first + (oldCount - newCount));
        }
        m_top.resize(m_h.size());
    }
    m_topValid = qMin(m_topValid, first);
    const bool layoutNow = newCount <= 64;
    QTextBlock b = sb;
    for (int i = first; i <= lastNew && b.isValid(); ++i, b = b.next()) {
        m_h[size_t(i)] = 0.f;
        m_st[size_t(i)] = 0;
        if (layoutNow) {
            layoutBlock(b, i); // setHeight() accounts for the new height
        } else {
            const float h = estimateHeight(b);
            m_h[size_t(i)] = h;
            m_sumH += h;
            if (anyWasLaidOut && b.layout()->lineCount() > 0)
                b.layout()->clearLayout();
        }
    }
    emit documentSizeChanged(documentSize());
    emit update(QRectF(0, topOf(first), 1e9, 1e9));
    Q_UNUSED(charsRemoved);
}

// ------------------------------------------------------------- formats ----
bool VirtualLayout::applyFormats(const QTextBlock &block, LineRole role, QTextLayout *tl) const
{
    const EditorTheme &t = m_ctx->theme();
    const QString text = block.text();
    BlockData *bd = dataOf(block);
    if (role != LineRole::Para) {
        if (bd && bd->inlineMath)
            bd->inlineMath.reset(); // no longer a paragraph: no inline formulas
    }
    const int len = int(text.size());
    QList<QTextLayout::FormatRange> fmts;
    auto add = [&](int s, int l, const QTextCharFormat &f) {
        if (l <= 0 || s >= len)
            return;
        QTextLayout::FormatRange r;
        r.start = s;
        r.length = qMin(l, len - s);
        r.format = marked(f);
        fmts.push_back(r);
    };
    const QFont roleF = roleFont(block, role);
    QTextCharFormat base;
    base.setFont(roleF, QTextCharFormat::FontPropertiesAll);
    base.setForeground(t.text);
    add(0, len, base);
    QTextCharFormat dimF;
    dimF.setForeground(t.dim);
    QTextCharFormat mathF;
    mathF.setForeground(t.mathSource);
    const int caretRel = (m_caret >= block.position() && m_caret < block.position() + block.length())
        ? m_caret - block.position() : -2;

    // Markup delimiters are hidden except in the span the caret touches (the
    // first one, if it touches two). Hidden delimiters stay in the line, so
    // positions, selection, undo and copy are unchanged: they are drawn
    // transparent with letter spacing that cancels their advance.
    MarkupState *ms = nullptr;
    auto markupState = [&] {
        if (!ms) {
            if (!bd->markup)
                bd->markup = std::make_unique<MarkupState>();
            ms = bd->markup.get();
            ms->spans.clear(); // keeps its capacity across relayouts
            ms->revealed = -1;
            ms->prefix = 0;
            ms->prefixShown = false;
        }
        return ms;
    };
    auto revealSpan = [&](int start, int end, int open, int close) {
        if (!bd)
            return true; // no per-line state to follow the caret with: show the source
        markupState();
        const bool touched = ms->revealed < 0 && caretRel >= start && caretRel <= end;
        if (touched)
            ms->revealed = int(ms->spans.size());
        ms->spans.push_back({start, end, open, close});
        return touched;
    };
    QFont markF = roleF; // the delimiters' own font (italic on quote lines)
    std::optional<QFontMetricsF> markFm, monoFm;
    // `pad`: advance each hidden run keeps (inline code keeps a little background).
    auto hide = [&](int s, int n, bool mono, qreal pad) {
        if (n <= 0 || s >= len)
            return;
        n = qMin(n, len - s);
        std::optional<QFontMetricsF> &fm = mono ? monoFm : markFm;
        if (!fm)
            fm.emplace(mono ? t.mono : markF);
        QTextCharFormat f;
        f.setForeground(QBrush(Qt::transparent));
        f.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        f.setFontLetterSpacing((pad - fm->horizontalAdvance(text.mid(s, n))) / n);
        add(s, n, f);
    };
    // Delimiters of the span [start, end): dim while the caret touches it.
    auto delimit = [&](int start, int end, int open, int close, bool mono, qreal pad) {
        if (revealSpan(start, end, open, close)) {
            if (!mono) {
                add(start, open, dimF);
                add(end - close, close, dimF);
            }
            return;
        }
        hide(start, open, mono, pad);
        hide(end - close, close, mono, pad);
    };
    bool collapsed = false;

    switch (role) {
    case LineRole::CodeOpen:
        // The info string after the fence stays, as the block's label.
        add(0, len, dimF);
        delimit(0, len, 3, 0, false, 0);
        break;
    case LineRole::CodeClose:
    case LineRole::MathOpen:
    case LineRole::MathClose:
        add(0, len, dimF);
        if (!revealSpan(0, len, len, 0)) {
            hide(0, len, false, 0);
            collapsed = true;
        }
        break;
    case LineRole::MathLine:
        add(0, len, mathF);
        break;
    case LineRole::MathSingle:
        add(2, len - 4, mathF);
        delimit(0, len, 2, 2, false, 0);
        break;
    case LineRole::CodeLine: {
        BlockData *d = dataOf(block);
        if (!d || !m_ctx->highlightEnabled)
            break;
        // Carry state from the previous line, computing backwards only as far
        // as the last line whose state is known.
        QTextBlock prev = block.previous();
        const code::Language *lang = nullptr;
        int stateIn = 0;
        if (roleOf(prev) == LineRole::CodeOpen) {
            const QString info = prev.text().mid(3).trimmed();
            const int sp = info.indexOf(QLatin1Char(' '));
            lang = code::languageFor(sp < 0 ? QStringView(info) : QStringView(info).left(sp));
        } else {
            std::vector<QTextBlock> chain;
            QTextBlock p = prev;
            while (p.isValid() && roleOf(p) == LineRole::CodeLine) {
                BlockData *pd = dataOf(p);
                if (pd && pd->hlValid)
                    break;
                chain.push_back(p);
                p = p.previous();
            }
            const code::Language *l = nullptr;
            int st = 0;
            if (p.isValid() && roleOf(p) == LineRole::CodeLine && dataOf(p)) {
                l = dataOf(p)->lang;
                st = dataOf(p)->hlOut;
            } else if (p.isValid() && roleOf(p) == LineRole::CodeOpen) {
                const QString info = p.text().mid(3).trimmed();
                const int sp = info.indexOf(QLatin1Char(' '));
                l = code::languageFor(sp < 0 ? QStringView(info) : QStringView(info).left(sp));
            }
            QVector<code::Span> scratch;
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                BlockData *cd = dataOf(*it);
                scratch.clear();
                const int out = code::highlightLine(l, it->text(), st, scratch);
                instr::add(instr::HighlightStateSteps);
                if (cd) {
                    cd->lang = l;
                    cd->hlIn = st;
                    cd->hlOut = out;
                    cd->hlValid = true;
                }
                st = out;
            }
            lang = l;
            stateIn = st;
        }
        QVector<code::Span> spans;
        const int oldOut = d->hlOut;
        const bool hadValid = d->hlValid;
        const int out = code::highlightLine(lang, text, stateIn, spans);
        instr::add(instr::HighlightLines);
        d->lang = lang;
        d->hlIn = stateIn;
        d->hlOut = out;
        d->hlValid = true;
        for (const code::Span &s : std::as_const(spans)) {
            if (s.token == code::Token::Plain)
                continue;
            QTextCharFormat f;
            f.setForeground(t.tokens[int(s.token)]);
            if (s.token == code::Token::Comment)
                f.setFontItalic(true);
            add(s.start, s.length, f);
        }
        // Propagate the carry state forward only until it stabilizes.
        if (hadValid && oldOut != out) {
            int budget = 4096;
            int st = out;
            QVector<code::Span> scratch;
            for (QTextBlock nb = block.next(); nb.isValid() && roleOf(nb) == LineRole::CodeLine; nb = nb.next()) {
                BlockData *nd = dataOf(nb);
                if (!nd)
                    break;
                if (budget-- <= 0) {
                    // Too far to settle synchronously: mark the rest unknown;
                    // they are recomputed lazily when they become visible.
                    for (QTextBlock r = nb; r.isValid() && roleOf(r) == LineRole::CodeLine; r = r.next())
                        if (BlockData *rd = dataOf(r))
                            rd->hlValid = false;
                    break;
                }
                const bool wasValid = nd->hlValid;
                const int prevIn = nd->hlIn;
                const int prevOut = nd->hlOut;
                if (wasValid && prevIn == st)
                    break; // stabilized
                scratch.clear();
                const int o = code::highlightLine(lang, nb.text(), st, scratch);
                instr::add(instr::HighlightStateSteps);
                nd->hlIn = st;
                nd->hlOut = o;
                nd->hlValid = true;
                nd->lang = lang;
                const int num = nb.blockNumber();
                if (num >= 0 && num < int(m_st.size()) && (m_st[size_t(num)] & StLaidOut)) {
                    m_st[size_t(num)] &= uint8_t(~StLaidOut);
                    --m_laidOut;
                    nb.layout()->clearLayout(); // recolor on next paint
                }
                st = o;
                if (wasValid && prevOut == o)
                    break;
            }
            emit const_cast<VirtualLayout *>(this)->update(QRectF(0, 0, 1e9, 1e9));
        }
        break;
    }
    case LineRole::Para: {
        int i = 0;
        // Inline formulas are drawn rendered in place, except the one the
        // caret touches (and none while an IME composition is open here:
        // its text would shift the spans).
        InlineMathState *im = nullptr;
        const bool renderInline = m_ctx->previews && tl->preeditAreaText().isEmpty();
        qreal inlinePt = 0;
        std::optional<QFontMetricsF> inlineFm;
        // The previous layout's formulas: a span whose source is unchanged
        // (typing elsewhere on the line) reuses its result and measurement.
        std::vector<InlineMathState::Span> previous;
        auto spanState = [&]() {
            if (!im) {
                if (!bd->inlineMath)
                    bd->inlineMath = std::make_unique<InlineMathState>();
                im = bd->inlineMath.get();
                previous.swap(im->spans);
                im->spans.clear();
                im->revealed = -1;
                im->pending = false;
                im->gen = m_inlineGen;
            }
            return im;
        };
        const int h = headingLevel(text);
        if (h) {
            // The #s and their space show only while the caret is on the line.
            if (!bd || caretRel >= 0) {
                add(0, h + 1, dimF);
            } else {
                hide(0, h + 1, false, 0);
            }
            if (bd) {
                markupState()->prefix = h + 1;
                ms->prefixShown = caretRel >= 0;
            }
            i = h + 1;
        } else if (const int lm = listMarkerLength(text)) {
            QTextCharFormat mk;
            mk.setForeground(t.accent);
            add(0, lm, mk);
            i = lm;
        } else if (len >= 2 && text.startsWith(QLatin1String("> "))) {
            QTextCharFormat q;
            q.setForeground(t.dim);
            q.setFontItalic(true);
            add(0, len, q);
            markF.setItalic(true);
            i = 2;
        }
        // Inline markup: `code`, $math$, **bold**, *italic*, [[link]], bare URLs.
        QTextCharFormat linkF;
        linkF.setForeground(t.accent);
        linkF.setFontUnderline(true);
        linkF.setUnderlineColor(t.accent);
        while (i < len) {
            const QChar c = text[i];
            if (c == QLatin1Char('\\')) {
                i += 2;
                continue;
            }
            if (const char16_t lc = c.unicode() | 0x20; // ASCII lower-case, no table lookup
                (lc == u'h' || lc == u'w' || lc == u'm' || lc == u'f') && (i == 0 || !text[i - 1].isLetterOrNumber())) {
                if (const int ul = links::bareUrlLength(text, i)) {
                    QTextCharFormat f = linkF;
                    f.setProperty(kLinkHrefProperty, links::bareUrlHref(QStringView(text).mid(i, ul)));
                    add(i, ul, f);
                    i += ul;
                    continue;
                }
            }
            if (c == QLatin1Char('`')) {
                // Markdown rule: a run of N backticks is closed only by a run
                // of exactly N; an unmatched run is plain text.
                int run = 1;
                while (i + run < len && text[i + run] == QLatin1Char('`'))
                    ++run;
                const QString fence(run, QLatin1Char('`'));
                int e = text.indexOf(fence, i + run);
                while (e >= 0 && e + run < len && text[e + run] == QLatin1Char('`'))
                    e = text.indexOf(fence, e + run + 1);
                if (e > i + run - 1 && e >= 0) {
                    QTextCharFormat f;
                    f.setFont(t.mono, QTextCharFormat::FontPropertiesAll);
                    f.setBackground(t.inlineCodeBackground);
                    add(i, e - i + run, f);
                    delimit(i, e + run, run, run, true, kCodePad);
                    i = e + run;
                } else {
                    i += run;
                }
                continue;
            } else if (c == QLatin1Char('$') && (i + 1 < len && text[i + 1] != QLatin1Char('$'))) {
                int e = i + 1;
                while (e < len && !(text[e] == QLatin1Char('$') && text[e - 1] != QLatin1Char('\\')))
                    ++e;
                if (e < len && e > i + 1) {
                    const int end = e + 1;
                    std::shared_ptr<const MathDisplayList> list;
                    qreal natural = 0;
                    const bool touched = caretRel >= i && caretRel <= end;
                    if (bd) {
                        InlineMathState *st = spanState();
                        if (touched)
                            st->revealed = int(st->spans.size());
                        if (inlinePt <= 0)
                            inlinePt = roleFont(block, role).pointSizeF();
                        const size_t key = qHash(QStringView(text).mid(i, end - i), size_t(inlinePt * 64));
                        if (renderInline && !touched) {
                            for (const InlineMathState::Span &p : previous)
                                if (p.key == key && p.list) {
                                    list = p.list;
                                    natural = p.natural;
                                    break;
                                }
                            if (!list)
                                list = inlineFormula(text.mid(i + 1, e - i - 1), inlinePt, &st->pending);
                        }
                        st->spans.push_back({i, end, list, key, natural});
                    }
#ifdef FOLIA_WITH_MATH
                    if (list) {
                        // The source stays in the line (positions, selection and
                        // copy are unchanged) but is invisible and spaced to the
                        // rendered width; draw() paints the formula over it.
                        const int n = end - i;
                        if (natural <= 0) {
                            if (!inlineFm)
                                inlineFm.emplace(roleFont(block, role));
                            natural = inlineFm->horizontalAdvance(text.mid(i, n));
                            im->spans.back().natural = natural;
                        }
                        QTextCharFormat cf;
                        cf.setForeground(QBrush(Qt::transparent));
                        cf.setFontLetterSpacingType(QFont::AbsoluteSpacing);
                        cf.setFontLetterSpacing((list->width + 2 * kInlinePad - natural) / n);
                        add(i, n, cf);
                        i = end;
                        continue;
                    }
#endif
                    // Shown as source (not rendered, or not yet): the `$`s
                    // show only while the caret touches it.
                    add(i + 1, e - i - 1, mathF);
                    if (!bd || touched) {
                        add(i, 1, dimF);
                        add(e, 1, dimF);
                    } else {
                        hide(i, 1, false, 0);
                        hide(e, 1, false, 0);
                    }
                    i = end;
                    continue;
                }
            } else if (c == QLatin1Char('*') && i + 1 < len && text[i + 1] == QLatin1Char('*')) {
                const int e = text.indexOf(QLatin1String("**"), i + 2);
                if (e > i + 2) {
                    QTextCharFormat f;
                    f.setFontWeight(QFont::Bold);
                    add(i + 2, e - i - 2, f);
                    delimit(i, e + 2, 2, 2, false, 0);
                    i = e + 2;
                    continue;
                }
            } else if ((c == QLatin1Char('*') || c == QLatin1Char('_'))
                       && (i == 0 || !text[i - 1].isLetterOrNumber())) {
                const int e = text.indexOf(c, i + 1);
                if (e > i + 1 && (e + 1 >= len || !text[e + 1].isLetterOrNumber())) {
                    QTextCharFormat f;
                    f.setFontItalic(true);
                    add(i + 1, e - i - 1, f);
                    delimit(i, e + 1, 1, 1, false, 0);
                    i = e + 1;
                    continue;
                }
            } else if (c == QLatin1Char('[') && i + 1 < len && text[i + 1] == QLatin1Char('[')) {
                const int e = text.indexOf(QLatin1String("]]"), i + 2);
                if (e > i + 2) {
                    QTextCharFormat f;
                    f.setForeground(t.accent);
                    add(i, e - i + 2, f);
                    i = e + 2;
                    continue;
                }
            }
            ++i;
        }
        // Named links: the label carries an anchor format in the document.
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (f.isValid() && links::isLink(f.charFormat()))
                add(f.position() - block.position(), f.length(), linkF);
        }
        if (bd && !im && bd->inlineMath)
            bd->inlineMath.reset(); // no formulas left on this line
        break;
    }
    default:
        break;
    }
    if (bd && !ms && bd->markup)
        bd->markup.reset(); // no delimiters left on this line

    // Preserve IME preedit formats set by the text control (unmarked) and
    // shift ours around the preedit insertion point.
    const QString preedit = tl->preeditAreaText();
    if (!preedit.isEmpty()) {
        const int pp = tl->preeditAreaPosition();
        const int pl = int(preedit.size());
        QList<QTextLayout::FormatRange> merged;
        for (const QTextLayout::FormatRange &r : tl->formats())
            if (!r.format.hasProperty(kLayoutFormatMarker))
                merged.push_back(r);
        for (QTextLayout::FormatRange r : std::as_const(fmts)) {
            if (r.start >= pp) {
                r.start += pl;
                merged.push_back(r);
            } else if (r.start + r.length <= pp) {
                merged.push_back(r);
            } else {
                QTextLayout::FormatRange a = r, b = r;
                a.length = pp - r.start;
                b.start = pp + pl;
                b.length = r.start + r.length - pp;
                merged.push_back(a);
                merged.push_back(b);
            }
        }
        // Unmarked (preedit) formats must win: put them last.
        std::stable_partition(merged.begin(), merged.end(), [](const QTextLayout::FormatRange &r) {
            return r.format.hasProperty(kLayoutFormatMarker);
        });
        tl->setFormats(merged);
    } else {
        tl->setFormats(fmts);
    }
    return collapsed;
}

// -------------------------------------------------------------- layout ----
void VirtualLayout::layoutBlock(const QTextBlock &block, int number) const
{
    instr::add(instr::BlocksLaidOut);
    const EditorTheme &t = m_ctx->theme();
    QTextLayout *tl = block.layout();
    const LineRole role = roleOf(block);
    const bool collapsed = applyFormats(block, role, tl);
    QTextOption opt = document()->defaultTextOption();
    opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    tl->setTextOption(opt);

    const qreal indent = indentFor(role);
    const qreal x = columnLeft() + indent;
    const qreal lw = qMax<qreal>(40, columnWidth() - 2 * indent);
    qreal y = topPadFor(block, role);
    // Lines with formulas drawn in place take the slower path below.
    const InlineMathState *im = nullptr;
#ifdef FOLIA_WITH_MATH
    if (BlockData *d = dataOf(block); d && d->inlineMath)
        for (const InlineMathState::Span &sp : d->inlineMath->spans)
            if (sp.list) {
                im = d->inlineMath.get();
                break;
            }
#endif
    if (roleIsAttachment(role)) {
        // One line holding the image or PDF object; its height comes from the
        // stored dimensions (the same arithmetic as estimateHeight), never a
        // decode or a render.
        const qreal ih = objectHeight(block, role);
        tl->beginLayout();
        QTextLine line = tl->createLine();
        if (line.isValid()) {
            line.setLineWidth(lw);
            line.setPosition(QPointF(x, y + ih - line.ascent()));
        }
        tl->endLayout();
        y += ih + extraBelow(block) + bottomPadFor(role);
        if (!(m_st[size_t(number)] & StLaidOut)) {
            m_st[size_t(number)] |= StLaidOut;
            ++m_laidOut;
        }
        setHeight(number, float(std::ceil(y)));
        return;
    }
    tl->beginLayout();
    for (;;) {
        QTextLine line = tl->createLine();
        if (!line.isValid())
            break;
        line.setLeadingIncluded(true);
        line.setLineWidth(lw);
        if (collapsed) {
            // Hidden fence: a sliver. The (invisible) line ends at the
            // sliver's bottom, so what is placed below the last line (the
            // math preview) starts right after it.
            line.setPosition(QPointF(x, y + kFenceSliver - line.height()));
            y += kFenceSliver;
            continue;
        }
        qreal above = 0, below = 0;
#ifdef FOLIA_WITH_MATH
        if (im) {
            // A rendered formula is one unit: if it would be split across
            // lines, end this line before it.
            const int ls = line.textStart(), le = ls + line.textLength();
            for (const InlineMathState::Span &sp : im->spans) {
                if (sp.list && sp.start > ls && sp.start < le && sp.end > le) {
                    const qreal w = line.cursorToX(sp.start) - line.x();
                    if (w > 1)
                        line.setLineWidth(w);
                    break;
                }
            }
            // Tall formulas (fractions, roots…) get room above and below.
            const int s2 = line.textStart(), e2 = s2 + line.textLength();
            for (const InlineMathState::Span &sp : im->spans) {
                if (sp.list && sp.start >= s2 && sp.start < e2) {
                    above = qMax(above, sp.list->ascent - line.ascent());
                    below = qMax(below, sp.list->descent - line.descent());
                }
            }
        }
#endif
        line.setPosition(QPointF(x, y + above));
        y += line.height() * t.lineSpacing + above + below;
    }
    tl->endLayout();
    if (role == LineRole::MathClose || role == LineRole::MathSingle) {
        BlockData *d = dataOf(block);
        if (d) {
            requestPreview(block, d, role);
            y += d->previewHeight;
        }
    }
    y += extraBelow(block);
    y += bottomPadFor(role);
    if (!(m_st[size_t(number)] & StLaidOut)) {
        m_st[size_t(number)] |= StLaidOut;
        ++m_laidOut;
    }
    setHeight(number, float(std::ceil(y)));
}

QString VirtualLayout::mathSourceFor(const QTextBlock &b, LineRole role) const
{
    if (role == LineRole::MathSingle) {
        const QString t = b.text();
        return t.mid(2, t.size() - 4);
    }
    QStringList lines;
    for (QTextBlock p = b.previous(); p.isValid(); p = p.previous()) {
        const LineRole r = roleOf(p);
        if (r == LineRole::MathLine)
            lines.prepend(p.text());
        else
            break;
    }
    return lines.join(QLatin1Char('\n'));
}

void VirtualLayout::requestPreview(const QTextBlock &block, BlockData *d, LineRole role) const
{
#ifdef FOLIA_WITH_MATH
    const EditorTheme &t = m_ctx->theme();
    const QString src = mathSourceFor(block, role);
    const qreal pt = previewPointSize();
    const quint64 hash = previewHash(src, pt);
    const QFontMetricsF fm(t.body);
    if (src.trimmed().isEmpty()) {
        d->previewHash = hash;
        d->preview.reset();
        d->previewError.clear();
        d->previewPending = false;
        d->previewHeight = 0;
        return;
    }
    if (d->previewHash == hash && (d->preview || !d->previewError.isEmpty() || d->previewPending)) {
        // Up to date (or waiting): keep current height.
        return;
    }
    d->previewHash = hash;
    d->preview.reset();
    d->previewError.clear();
    IPreviewService *svc = m_ctx->previews;
    if (!svc) {
        d->previewHeight = 0;
        return;
    }
    PreviewRequest req;
    req.block = d->id;
    req.blockRev = hash;
    req.kind = PreviewKind::DisplayMath;
    req.source = src;
    req.pointSize = pt;
    req.color = t.text;
    d->previewPending = true;
    // Keep the previously reserved height while rendering to avoid jumps.
    if (d->previewHeight <= 0)
        d->previewHeight = float(fm.lineSpacing() * 2 + 2 * kPreviewPad);
    QPointer<VirtualLayout> self(const_cast<VirtualLayout *>(this));
    svc->renderPreview(req, const_cast<VirtualLayout *>(this), [self, hash](PreviewResult r) {
        if (!self)
            return;
        // Find visible math blocks still waiting for exactly this source.
        QTextDocument *doc = self->document();
        const int first = self->blockAtY(self->m_viewTop - 400);
        const int last = self->blockAtY(self->m_viewBottom + 400);
        if (first < 0)
            return;
        bool applied = false;
        QTextBlock b = doc->findBlockByNumber(first);
        for (int i = first; i <= last && b.isValid(); ++i, b = b.next()) {
            BlockData *bd = dataOf(b);
            if (!bd || !bd->previewPending || bd->previewHash != hash)
                continue;
            bd->previewPending = false;
            const QFontMetricsF efm(self->m_ctx->theme().body);
            if (r.ok && r.list) {
                bd->preview = r.list;
                bd->previewError.clear();
                bd->previewHeight = float(std::ceil(r.list->height() + 2 * kPreviewPad));
            } else {
                bd->preview.reset();
                bd->previewError = r.error.isEmpty() ? QStringLiteral("Cannot render") : r.error;
                bd->previewHeight = float(std::ceil(efm.lineSpacing() + 2 * kPreviewPad));
            }
            self->relayoutBlock(b);
            applied = true;
        }
        if (!applied)
            instr::add(instr::PreviewStaleDrops);
    });
#else
    Q_UNUSED(block);
    Q_UNUSED(role);
    d->previewHeight = 0;
#endif
}

void VirtualLayout::previewArrived(quint64) {}

std::shared_ptr<const MathDisplayList> VirtualLayout::inlineFormula(const QString &source, qreal pointSize,
                                                                   bool *pending) const
{
#ifdef FOLIA_WITH_MATH
    IPreviewService *svc = m_ctx->previews;
    if (!svc || source.trimmed().isEmpty())
        return nullptr;
    // Same request as the caret's preview bubble, so a formula just edited
    // is already cached when the caret leaves it.
    PreviewRequest req;
    req.kind = PreviewKind::InlineMath;
    req.source = source;
    req.pointSize = pointSize;
    req.color = m_ctx->theme().text;
    PreviewResult cached;
    if (svc->lookup(req, &cached))
        return cached.ok ? cached.list : nullptr; // an error keeps the source visible
    if (m_inlineBudgetUs > 0) {
        // Warm renderer: render here, within this paint's budget.
        QElapsedTimer t;
        t.start();
        const bool done = svc->renderNow(req, &cached, int(qMax<qint64>(1, m_inlineBudgetUs / 1000)));
        m_inlineBudgetUs -= t.nsecsElapsed() / 1000;
        if (done)
            return cached.ok ? cached.list : nullptr;
    }
    const quint64 key = previewHash(source, pointSize) ^ 0x1f83d9abfb41bd6bull;
    *pending = true;
    if (m_inlineRequested.contains(key))
        return nullptr; // already in flight
    m_inlineRequested.insert(key);
    // Cold renderer or over budget: render on the preview worker. The request
    // id is unique to this layout and formula, so nothing supersedes it.
    req.block = BlockId(((key ^ (quint64(quintptr(this)) * 0x9e3779b97f4a7c15ull)) >> 3) | (quint64(1) << 61));
    req.blockRev = key;
    QPointer<VirtualLayout> self(const_cast<VirtualLayout *>(this));
    svc->renderPreview(req, const_cast<VirtualLayout *>(this), [self, key](PreviewResult) {
        if (self)
            self->inlineFormulaArrived(key);
    });
    return nullptr;
#else
    Q_UNUSED(source);
    Q_UNUSED(pointSize);
    Q_UNUSED(pending);
    return nullptr;
#endif
}

void VirtualLayout::inlineFormulaArrived(quint64 key)
{
    m_inlineRequested.remove(key);
    ++m_inlineGen;
    // Lines waiting for a formula re-probe the cache when next painted.
    if (!m_inlineRepaint.isActive())
        m_inlineRepaint.start();
}

void VirtualLayout::setCaret(int position)
{
    if (position == m_caret)
        return;
    const int old = m_caret;
    m_caret = position;
    m_inlineBudgetUs = 2000; // the formula the caret leaves renders right away
    QTextDocument *doc = document();
    // Only lines with formulas or markup delimiters care, and only when the
    // caret starts or stops touching one of them: then that line is laid out
    // again.
    auto refresh = [&](int pos) {
        if (pos < 0 || pos >= doc->characterCount())
            return QTextBlock();
        const QTextBlock b = doc->findBlock(pos);
        const BlockData *d = b.isValid() ? dataOf(b) : nullptr;
        if (!d || (!d->inlineMath && !d->markup))
            return b;
        const int rel = (m_caret >= b.position() && m_caret < b.position() + b.length())
            ? m_caret - b.position() : -2;
        auto touching = [rel](const auto &spans) {
            for (int k = 0; k < int(spans.size()); ++k)
                if (rel >= spans[size_t(k)].start && rel <= spans[size_t(k)].end)
                    return k;
            return -1;
        };
        if ((d->inlineMath && touching(d->inlineMath->spans) != d->inlineMath->revealed)
            || (d->markup && touching(d->markup->spans) != d->markup->revealed)
            || (d->markup && d->markup->prefix > 0 && (rel >= 0) != d->markup->prefixShown))
            relayoutBlock(b);
        return b;
    };
    const QTextBlock ob = refresh(old);
    if (!ob.isValid() || !(ob.position() <= position && position < ob.position() + ob.length()))
        refresh(position);
}

quint64 VirtualLayout::previewHash(const QString &source, qreal pointSize)
{
    return quint64(qHash(source)) ^ (quint64(qRound(pointSize * 16)) << 48) ^ 0x9e3779b97f4a7c15ull;
}

QString VirtualLayout::mathSource(const QTextBlock &b) const
{
    return mathSourceFor(b, roleOf(b));
}

qreal VirtualLayout::previewPointSize() const
{
    return m_ctx->theme().body.pointSizeF() * 1.12;
}

// --------------------------------------------------------------- paint ----
void VirtualLayout::setViewport(qreal top, qreal bottom)
{
    if (top > m_viewTop)
        m_scrollDir = 1;
    else if (top < m_viewTop)
        m_scrollDir = -1;
    if (top != m_viewTop || bottom != m_viewBottom)
        m_mediaDirty = true;
    m_viewTop = top;
    m_viewBottom = bottom;
}

QRectF VirtualLayout::codeCopyRect(const QTextBlock &block) const
{
    if (roleOf(block) != LineRole::CodeOpen)
        return QRectF();
    const QRectF br = blockBoundingRect(block);
    QTextLayout *tl = block.layout();
    if (!tl || tl->lineCount() == 0)
        return QRectF();
    const QTextLine l = tl->lineAt(0);
    QFont f = m_ctx->theme().body;
    f.setPointSizeF(f.pointSizeF() * 0.82);
    const qreal w = QFontMetricsF(f).horizontalAdvance(QStringLiteral("Copy")) + 16 + kCopyIcon + 5;
    const qreal right = columnLeft() + columnWidth() - 6;
    return QRectF(right - w, br.top() + l.y() + 2, w, l.height() - 4);
}

QRectF VirtualLayout::previewRect(const QTextBlock &block) const
{
    const LineRole role = roleOf(block);
    if (role != LineRole::MathClose && role != LineRole::MathSingle)
        return QRectF();
    BlockData *d = dataOf(block);
    const QRectF br = blockBoundingRect(block);
    QTextLayout *tl = block.layout();
    if (!d || !tl || tl->lineCount() == 0)
        return QRectF();
    const QTextLine l = tl->lineAt(tl->lineCount() - 1);
    const qreal top = br.top() + l.y() + l.height();
    return QRectF(columnLeft(), top, columnWidth(), d->previewHeight);
}

void VirtualLayout::paintBlockDecorations(QPainter *p, const QTextBlock &block, LineRole role,
                                          const QRectF &r, const PaintContext &) const
{
    const EditorTheme &t = m_ctx->theme();
    const qreal left = columnLeft();
    const qreal width = columnWidth();
    if (roleIsCodeSegment(role) || roleIsMathSegment(role) || role == LineRole::MathSingle) {
        const bool math = !roleIsCodeSegment(role);
        qreal top = r.top();
        qreal bottom = r.bottom();
        if (role == LineRole::CodeOpen || role == LineRole::MathOpen || role == LineRole::MathSingle)
            top += topPadFor(block, role) - 4;
        if (role == LineRole::CodeClose || role == LineRole::MathClose || role == LineRole::MathSingle)
            bottom -= bottomPadFor(role) - 4;
        const QRectF bg(left, top, width, bottom - top);
        if (math) {
            QColor c = t.mathSource;
            c.setAlpha(18);
            p->fillRect(bg, c);
            QColor bar = t.mathSource;
            bar.setAlpha(110);
            p->fillRect(QRectF(left, top, 2.5, bg.height()), bar);
        } else {
            p->fillRect(bg, t.codeBackground);
        }
    } else if (role == LineRole::Para && block.text().startsWith(QLatin1String("> "))) {
        QColor bar = t.dim;
        bar.setAlpha(120);
        p->fillRect(QRectF(left - 12, r.top() + 2, 3, r.height() - 4 - t.paragraphSpacing), bar);
    }
}

void VirtualLayout::draw(QPainter *painter, const PaintContext &ctx)
{
    instr::add(instr::PaintEvents);
    // Paint without the caller's clip when it is redundant (setRedundantClip);
    // the paint engine's system clip still bounds every pixel.
    struct ClipOff {
        QPainter *p;
        bool off;
        ~ClipOff()
        {
            if (off)
                p->setClipping(true);
        }
    } clipOff{painter, m_redundantClip && painter->hasClipping() && painter->paintEngine()
                           && !painter->paintEngine()->systemClip().isEmpty()};
    if (clipOff.off)
        painter->setClipping(false);
    QTextDocument *doc = document();
    if (m_h.empty() || int(m_h.size()) != doc->blockCount())
        fullInvalidate();
    QRectF clip = ctx.clip;
    if (!clip.isValid())
        clip = QRectF(0, m_viewTop, m_width, qMax<qreal>(1, m_viewBottom - m_viewTop));
    m_inPaint = true;
    m_heightChangedInPaint = false;
    m_inlineBudgetUs = 3000;
    const int first = blockAtY(clip.top());
    if (first < 0) {
        m_inPaint = false;
        return;
    }
    const EditorTheme &t = m_ctx->theme();
    QTextBlock block = doc->findBlockByNumber(first);
    int i = first;
    int last = first;
    QTextCharFormat selFmt;
    for (; block.isValid(); block = block.next(), ++i) {
        if (!(m_st[size_t(i)] & StLaidOut) || block.layout()->lineCount() == 0)
            layoutBlock(block, i);
        else if (const BlockData *wd = dataOf(block);
                 wd && wd->inlineMath && wd->inlineMath->pending && wd->inlineMath->gen != m_inlineGen)
            layoutBlock(block, i); // a formula it waits for may have arrived
        const qreal top = topOf(i);
        if (top > clip.bottom())
            break;
        last = i;
        const QRectF r(0, top, m_width, m_h[size_t(i)]);
        if (r.bottom() < clip.top())
            continue;
        const LineRole role = roleOf(block);
        paintBlockDecorations(painter, block, role, r, ctx);

        QTextLayout *tl = block.layout();
        const int bpos = block.position();
        const int blen = block.length();
        QList<QTextLayout::FormatRange> sels;
        bool selectionCoversSeparator = false;
        const BlockData *bdata = dataOf(block);
        const InlineMathState *ims = bdata && bdata->inlineMath ? bdata->inlineMath.get() : nullptr;
        // Over a formula drawn in place, a selection paints its background
        // but not the (invisible) source glyphs.
        auto addSelection = [&](const QTextLayout::FormatRange &fr) {
            if (!ims) {
                sels.push_back(fr);
                return;
            }
            int at = fr.start;
            const int stop = fr.start + fr.length;
            for (const InlineMathState::Span &sp : ims->spans) {
                if (!sp.list || sp.end <= at || sp.start >= stop)
                    continue;
                if (sp.start > at) {
                    QTextLayout::FormatRange a = fr;
                    a.start = at;
                    a.length = sp.start - at;
                    sels.push_back(a);
                }
                QTextLayout::FormatRange m = fr;
                m.start = qMax(at, sp.start);
                m.length = qMin(stop, sp.end) - m.start;
                m.format.clearForeground();
                sels.push_back(m);
                at = qMin(stop, sp.end);
            }
            if (at < stop) {
                QTextLayout::FormatRange c = fr;
                c.start = at;
                c.length = stop - at;
                sels.push_back(c);
            }
        };
        for (const Selection &s : ctx.selections) {
            const int ss = s.cursor.selectionStart();
            const int se = s.cursor.selectionEnd();
            if (se <= bpos || ss >= bpos + blen)
                continue;
            QTextLayout::FormatRange fr;
            fr.start = qMax(ss, bpos) - bpos;
            fr.length = qMin(se, bpos + blen - 1) - bpos - fr.start;
            fr.format = s.format;
            if (fr.length > 0)
                addSelection(fr);
            if (se > bpos + blen - 1 && ss < bpos + blen) {
                selectionCoversSeparator = true;
                selFmt = s.format;
            }
        }
        const QPointF origin(0, top);
        tl->draw(painter, origin, sels, clip);
#ifdef FOLIA_WITH_MATH
        if (ims) {
            for (const InlineMathState::Span &sp : ims->spans) {
                if (!sp.list)
                    continue;
                const QTextLine l = tl->lineForTextPosition(sp.start);
                if (!l.isValid())
                    continue;
                const qreal x0 = l.cursorToX(sp.start);
                const bool sameLine = sp.end <= l.textStart() + l.textLength();
                const qreal slot = sameLine ? l.cursorToX(sp.end) - x0 : sp.list->width + 2 * kInlinePad;
                sp.list->paint(painter, QPointF(x0 + qMax<qreal>(0, (slot - sp.list->width) / 2), top + l.y() + l.ascent()),
                               t.text);
            }
        }
#endif
        if (selectionCoversSeparator && tl->lineCount() > 0) {
            const QTextLine ll = tl->lineAt(tl->lineCount() - 1);
            const QRectF sep(ll.x() + ll.naturalTextWidth(), top + ll.y(), 6, ll.height());
            painter->fillRect(sep.intersected(r), selFmt.background()); // a fence sliver is shorter than its line
        }

        // Rendered math preview below the source.
        if ((role == LineRole::MathClose || role == LineRole::MathSingle) && tl->lineCount() > 0) {
            if (BlockData *d = dataOf(block)) {
                const QTextLine ll = tl->lineAt(tl->lineCount() - 1);
                const qreal pTop = top + ll.y() + ll.height();
#ifdef FOLIA_WITH_MATH
                if (d->preview) {
                    const MathDisplayList &dl = *d->preview;
                    const qreal x = columnLeft() + qMax<qreal>(kCodeInset, (columnWidth() - dl.width) / 2);
                    dl.paint(painter, QPointF(x, pTop + kPreviewPad + dl.ascent), t.text);
                } else
#endif
                if (!d->previewError.isEmpty()) {
                    QFont ef = t.body;
                    ef.setPointSizeF(t.body.pointSizeF() * 0.86);
                    painter->setFont(ef);
                    painter->setPen(t.mathError);
                    const QFontMetricsF fm(ef);
                    painter->drawText(QPointF(columnLeft() + kCodeInset, pTop + kPreviewPad + fm.ascent()),
                                      QStringLiteral("⚠ ") + d->previewError);
                }
            }
        }
        // Selected image or PDF: tinted, since the object hides the selection background.
        if (roleIsAttachment(role)) {
            for (const Selection &s : ctx.selections)
                if (s.cursor.selectionStart() <= bpos && s.cursor.selectionEnd() > bpos) {
                    QColor c = s.format.background().color();
                    c.setAlpha(110);
                    painter->fillRect(imageRect(block), c);
                    break;
                }
        }
        // Images being imported after this line: a placeholder, never text.
        if (const BlockData *pd = dataOf(block); pd && pd->pendingImports > 0) {
            const QRectF pr(columnLeft(), r.bottom() - bottomPadFor(role) - kImportH + 6, qMin<qreal>(columnWidth(), 360),
                            kImportH - 12);
            paintImagePlaceholder(painter, pr,
                                  pd->pendingImports > 1 ? tr("Importing %1 files…").arg(pd->pendingImports)
                                                         : tr("Importing…"));
        }
        // Copy affordance on code openers.
        if (role == LineRole::CodeOpen && tl->lineCount() > 0) {
            const QRectF cr = codeCopyRect(block);
            QFont f = t.body;
            f.setPointSizeF(t.body.pointSizeF() * 0.82);
            painter->setFont(f);
            painter->setPen(t.dim);
            const QRectF ir(cr.left() + 8, cr.center().y() - kCopyIcon / 2, kCopyIcon, kCopyIcon);
            icons::paint(painter, QStringLiteral("copy"), ir, t.dim);
            painter->drawText(cr.adjusted(8 + kCopyIcon + 5, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft,
                              QStringLiteral("Copy"));
        }

        // Caret (also inside an IME preedit area).
        int cpos = -1;
        if (ctx.cursorPosition >= bpos && ctx.cursorPosition < bpos + blen)
            cpos = ctx.cursorPosition - bpos;
        else if (ctx.cursorPosition < -1 && !tl->preeditAreaText().isEmpty())
            cpos = tl->preeditAreaPosition() - (ctx.cursorPosition + 2);
        if (cpos >= 0) {
            painter->setPen(ctx.palette.color(QPalette::Text));
            tl->drawCursor(painter, origin, cpos, 2);
        }
    }
    m_inPaint = false;
    if (m_heightChangedInPaint) {
        emit documentSizeChanged(documentSize());
        emit update(QRectF(0, clip.top(), 1e9, 1e9));
    }
    if (m_laidOut > m_cap)
        evictFarBlocks(first, last);
    if (m_mediaDirty)
        updateMediaWants();
}

void VirtualLayout::evictFarBlocks(int firstVisible, int lastVisible)
{
    QTextDocument *doc = document();
    const int keepFrom = qMax(0, firstVisible - 150);
    const int keepTo = lastVisible + 150;
    const int n = int(m_st.size());
    for (int i = 0; i < n && m_laidOut > m_cap / 2; ++i) {
        if (i >= keepFrom && i <= keepTo)
            continue;
        if (!(m_st[size_t(i)] & StLaidOut))
            continue;
        QTextBlock b = doc->findBlockByNumber(i);
        if (!b.isValid())
            continue;
        QTextLayout *tl = b.layout();
        if (!tl->preeditAreaText().isEmpty())
            continue; // pinned: active composition
        tl->clearLayout();
        tl->clearFormats(); // frees shaped glyph data
        if (BlockData *d = dataOf(b)) {
            d->inlineMath.reset(); // rebuilt (from the cache) if it comes back
            d->mediaKey = MediaKey(); // the key only: the bitmap stays cached
            d->markup.reset();
        }
        m_st[size_t(i)] &= uint8_t(~StLaidOut);
        --m_laidOut;
    }
}

int VirtualLayout::hitTest(const QPointF &point, Qt::HitTestAccuracy accuracy) const
{
    QTextDocument *doc = document();
    const int i = blockAtY(point.y());
    if (i < 0)
        return -1;
    QTextBlock b = doc->findBlockByNumber(i);
    if (!b.isValid())
        return -1;
    if (!(m_st[size_t(i)] & StLaidOut) || b.layout()->lineCount() == 0)
        layoutBlock(b, i);
    const QPointF local = point - QPointF(0, topOf(i));
    QTextLayout *tl = b.layout();
    if (tl->lineCount() == 0)
        return b.position();
    QTextLine line = tl->lineAt(0);
    for (int k = 0; k < tl->lineCount(); ++k) {
        QTextLine l = tl->lineAt(k);
        line = l;
        if (local.y() < l.y() + l.height())
            break;
    }
    if (accuracy == Qt::ExactHit) {
        const QRectF lr(line.x(), line.y(), line.naturalTextWidth(), line.height());
        if (!lr.contains(local))
            return -1;
    }
    const int off = line.xToCursor(local.x(), QTextLine::CursorBetweenCharacters);
    int rel = qBound(0, off, b.length() - 1);
    // A click on a formula drawn in place opens its source, caret after '$'.
    if (const BlockData *d = dataOf(b); d && d->inlineMath)
        for (const InlineMathState::Span &sp : d->inlineMath->spans)
            if (sp.list && rel > sp.start && rel < sp.end) {
                rel = sp.start + 1;
                break;
            }
    // Positions inside a hidden delimiter (`**`, a fence) share one x: take
    // the one next to the text rather than one between two marker characters.
    if (const BlockData *d = dataOf(b); d && d->markup) {
        if (!d->markup->prefixShown && rel < d->markup->prefix)
            rel = d->markup->prefix; // a hidden heading marker: start of the heading text
        const auto &spans = d->markup->spans;
        for (int k = 0; k < int(spans.size()); ++k) {
            const MarkupState::Span &sp = spans[size_t(k)];
            if (k == d->markup->revealed)
                continue;
            if (rel > sp.start && rel < sp.start + sp.open) {
                rel = sp.start + sp.open;
                break;
            }
            if (rel > sp.end - sp.close && rel < sp.end) {
                rel = sp.end - sp.close;
                break;
            }
        }
    }
    return b.position() + rel;
}

// ----------------------------------------------------- inline objects -----
void VirtualLayout::resizeInlineObject(QTextInlineObject item, int pos, const QTextFormat &format)
{
    if (format.objectType() == kImageObjectType || format.objectType() == kPdfObjectType) {
        const QSizeF sz = format.objectType() == kPdfObjectType
            ? pdfCardSize() : imageDisplaySize(format.property(kAttachmentHashProperty).toString());
        item.setWidth(sz.width());
        item.setAscent(sz.height());
        item.setDescent(0);
        return;
    }
    if (format.objectType() != kPageLinkObjectType) {
        QAbstractTextDocumentLayout::resizeInlineObject(item, pos, format);
        return;
    }
    const PageId target = format.property(kPageLinkTargetProperty).toLongLong();
    const QString title = m_ctx->titleOf ? m_ctx->titleOf(target) : QString();
    const QFontMetricsF fm(m_ctx->theme().body);
    const qreal w = qMin(columnWidth(), fm.horizontalAdvance(title.isEmpty() ? QStringLiteral("Untitled") : title) + 38);
    item.setWidth(w);
    item.setAscent(fm.ascent() + 5);
    item.setDescent(fm.descent() + 5);
}

void VirtualLayout::positionInlineObject(QTextInlineObject item, int pos, const QTextFormat &format)
{
    const int ot = format.objectType();
    if (ot != kPageLinkObjectType && ot != kImageObjectType && ot != kPdfObjectType)
        QAbstractTextDocumentLayout::positionInlineObject(item, pos, format);
}

void VirtualLayout::drawInlineObject(QPainter *p, const QRectF &rect, QTextInlineObject object,
                                     int pos, const QTextFormat &format)
{
    if (format.objectType() == kImageObjectType) {
        drawImageObject(p, rect, pos, format);
        return;
    }
    if (format.objectType() == kPdfObjectType) {
        drawPdfObject(p, rect, pos, format);
        return;
    }
    if (format.objectType() != kPageLinkObjectType) {
        QAbstractTextDocumentLayout::drawInlineObject(p, rect, object, pos, format);
        return;
    }
    const EditorTheme &t = m_ctx->theme();
    const PageId target = format.property(kPageLinkTargetProperty).toLongLong();
    QString title = m_ctx->titleOf ? m_ctx->titleOf(target) : QString();
    if (title.isEmpty())
        title = QStringLiteral("Untitled");
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    const QRectF chip = rect.adjusted(0, 1, 0, -1);
    p->setPen(Qt::NoPen);
    p->setBrush(t.linkBackground);
    p->drawRoundedRect(chip, 5, 5);
    // Child page glyph: Lucide "file-text".
    const qreal gs = 16;
    icons::paint(p, QStringLiteral("file-text"), QRectF(chip.left() + 7, chip.center().y() - gs / 2, gs, gs), t.accent);
    p->setPen(t.accent);
    p->setFont(t.body);
    const QFontMetricsF fm(t.body);
    const QRectF tr = chip.adjusted(29, 0, -9, 0);
    p->drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(title, Qt::ElideRight, tr.width()));
    p->restore();
}

// -------------------------------------------------------------- images ----
void VirtualLayout::paintImagePlaceholder(QPainter *p, const QRectF &rect, const QString &label) const
{
    const EditorTheme &t = m_ctx->theme();
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    QColor border = t.dim;
    border.setAlpha(90);
    p->setPen(QPen(border, 1));
    p->setBrush(t.codeBackground);
    p->drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
    if (!label.isEmpty()) {
        QFont f = t.body;
        f.setPointSizeF(t.body.pointSizeF() * 0.86);
        p->setFont(f);
        p->setPen(t.dim);
        p->drawText(rect, Qt::AlignCenter, label);
    }
    p->restore();
}

void VirtualLayout::drawImageObject(QPainter *p, const QRectF &rect, int pos, const QTextFormat &format)
{
    // No file I/O, decoding or conversion here: look the key up, draw, keep
    // nothing but the key.
    const QString hash = format.property(kAttachmentHashProperty).toString();
    if (const QPaintDevice *dev = p->device()) {
        const qreal dpr = dev->devicePixelRatioF();
        if (dpr > 0 && dpr != m_dpr) {
            m_dpr = dpr;
            m_mediaDirty = true;
        }
    }
    MediaService *media = m_ctx->media;
    AttachmentStore *as = m_ctx->attachments;
    const AttachmentInfo *info = as ? as->info(hash) : nullptr;
    BlockData *d = dataOf(document()->findBlock(pos));
    const QImage *img = nullptr;
    MediaKey used;
    bool smooth = false;
    QString label;
    if (media && info) {
        const MediaService::Request req = media->imageRequest(*info, QString(), rect.width(), m_dpr);
        used = req.key;
        img = media->lookup(req.key);
        if (!img && (img = media->nearest(hash, req.key.width, 0.10, &used)))
            smooth = true; // within ±10 %: reused, drawn smooth-scaled
        if (!img)
            img = media->nearest(hash, req.key.width, -1, &used); // while resizing: fast scaling
        if (!img) {
            used = req.key;
            if (media->tooLarge(req.key))
                label = tr("Too large to preview");
            else if (media->failed(hash))
                label = tr("Cannot show this image");
        }
    } else if (as && as->isMissing(hash)) {
        label = tr("Missing attachment");
    }
    if (d)
        d->mediaKey = used;
    if (!img) {
        paintImagePlaceholder(p, rect, label);
        return;
    }
    p->save();
    p->setRenderHint(QPainter::SmoothPixmapTransform, smooth);
    p->drawImage(rect, *img);
    p->restore();
}

void VirtualLayout::drawPdfObject(QPainter *p, const QRectF &rect, int pos, const QTextFormat &format)
{
    // The card: first-page thumbnail (a cache key, never a copy), file name,
    // page count and size. No file I/O or rendering here.
    const EditorTheme &t = m_ctx->theme();
    const QString hash = format.property(kAttachmentHashProperty).toString();
    const QString name = format.property(kImageAltProperty).toString();
    if (const QPaintDevice *dev = p->device()) {
        const qreal dpr = dev->devicePixelRatioF();
        if (dpr > 0 && dpr != m_dpr) {
            m_dpr = dpr;
            m_mediaDirty = true;
        }
    }
    AttachmentStore *as = m_ctx->attachments;
    MediaService *media = m_ctx->media;
    const AttachmentInfo *info = as ? as->info(hash) : nullptr;
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    QColor border = t.dim;
    border.setAlpha(90);
    p->setPen(QPen(border, 1));
    p->setBrush(t.codeBackground);
    p->drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    const QRectF thumb = pdfThumbRect(rect, hash);
    const MediaService::Request req = pdfThumbRequest(hash, thumb);
    const QImage *img = media ? media->lookup(req.key) : nullptr;
    if (BlockData *d = dataOf(document()->findBlock(pos)))
        d->mediaKey = req.key;
    p->setRenderHint(QPainter::Antialiasing, false);
    if (img) {
        p->drawImage(thumb, *img);
    } else {
        p->fillRect(thumb, QColor(255, 255, 255)); // a blank page until it renders
    }
    p->setPen(QPen(border, 1));
    p->setBrush(Qt::NoBrush);
    p->drawRect(thumb.adjusted(-0.5, -0.5, 0.5, 0.5));
    // Text column.
    const qreal tx = thumb.right() + 14;
    const QRectF text(tx, rect.top() + kPdfPad + 4, qMax<qreal>(10, rect.right() - kPdfPad - tx), rect.height() - 2 * kPdfPad);
    QFont nf = t.body;
    nf.setWeight(QFont::DemiBold);
    p->setFont(nf);
    p->setPen(t.text);
    const QFontMetricsF nfm(nf);
    p->drawText(QPointF(text.left(), text.top() + nfm.ascent()),
                nfm.elidedText(name.isEmpty() ? tr("Document.pdf") : name, Qt::ElideMiddle, text.width()));
    QFont df = t.body;
    df.setPointSizeF(t.body.pointSizeF() * 0.86);
    p->setFont(df);
    const QFontMetricsF dfm(df);
    QString detail;
    if (info && info->pages > 0)
        detail = (info->pages == 1 ? tr("1 page") : tr("%1 pages").arg(info->pages)) + QStringLiteral(" · ")
            + formatSize(info->bytes);
    else if (as && as->isMissing(hash))
        detail = tr("Missing attachment");
    if (media && media->failed(hash))
        detail = tr("Cannot show this PDF");
    p->setPen(t.dim);
    const qreal dy = text.top() + nfm.lineSpacing() + 4 + dfm.ascent();
    p->drawText(QPointF(text.left(), dy), dfm.elidedText(detail, Qt::ElideRight, text.width()));
    p->drawText(QPointF(text.left(), text.bottom() - dfm.descent() - 4), dfm.elidedText(tr("Click to open"), Qt::ElideRight, text.width()));
    p->restore();
}

void VirtualLayout::mediaArrived(const MediaKey &key)
{
    LineRole want;
    if (key.kind == MediaKey::Image)
        want = LineRole::Image;
    else if (key.kind == MediaKey::PdfThumb)
        want = LineRole::Pdf;
    else
        return; // a viewer's page or tile: nothing here shows it
    // Repaint only the visible lines showing it.
    const int first = blockAtY(m_viewTop);
    const int last = blockAtY(m_viewBottom);
    if (first < 0)
        return;
    QTextBlock b = document()->findBlockByNumber(first);
    for (int i = first; i <= last && b.isValid(); ++i, b = b.next())
        if (roleOf(b) == want && imageHashOf(b) == key.hash)
            emit update(QRectF(0, topOf(i), 1e9, m_h[size_t(i)]));
}

void VirtualLayout::releaseMedia()
{
    if (m_ctx->media)
        m_ctx->media->removeOwner(this);
    m_mediaDirty = true;
}

void VirtualLayout::updateMediaWants()
{
    m_mediaDirty = false;
    MediaService *media = m_ctx->media;
    AttachmentStore *as = m_ctx->attachments;
    if (!media || !as || m_h.empty() || m_viewBottom <= m_viewTop)
        return;
    // Keep window: the viewport plus one viewport ahead in the scroll
    // direction. Requests outside it are dropped before they start.
    const qreal vh = m_viewBottom - m_viewTop;
    const qreal keepTop = m_scrollDir < 0 ? m_viewTop - vh : m_viewTop;
    const qreal keepBottom = m_scrollDir > 0 ? m_viewBottom + vh : m_viewBottom;
    const bool resizing = m_widthChanged.isValid() && m_widthChanged.elapsed() < kResizeSettleMs;
    bool settle = false;
    QVector<MediaService::Request> visible, ahead;
    const int first = blockAtY(keepTop);
    const int last = blockAtY(keepBottom);
    QTextBlock b = document()->findBlockByNumber(first);
    for (int i = first; i <= last && b.isValid(); ++i, b = b.next()) {
        const LineRole role = roleOf(b);
        if (!roleIsAttachment(role))
            continue;
        const QString hash = imageHashOf(b);
        const AttachmentInfo *info = as->info(hash);
        if (!info) {
            as->fetchInfo(hash);
            continue;
        }
        if (role == LineRole::Pdf) {
            // The first-page thumbnail, through the PDF lane and the budget.
            const QRectF card(QPointF(columnLeft(), topOf(i) + kImageTop), pdfCardSize());
            MediaService::Request r = pdfThumbRequest(hash, pdfThumbRect(card, hash));
            const qreal top = topOf(i);
            const bool isVisible = top + m_h[size_t(i)] > m_viewTop && top < m_viewBottom;
            r.priority = isVisible ? 0 : 1;
            (isVisible ? visible : ahead).push_back(r);
            continue;
        }
        MediaService::Request r = media->imageRequest(*info, as->pathFor(hash), imageDisplaySize(hash).width(), m_dpr);
        if (!media->contains(r.key)) {
            MediaKey near;
            if (media->nearest(hash, r.key.width, 0.10, &near)) {
                r.key = near; // within ±10 % of a cached resolution: no decode
            } else if (resizing) {
                // The divider is moving: keep drawing what is cached (scaled)
                // and decode once the width has settled.
                settle = true;
                if (!media->nearest(hash, r.key.width, -1, &near))
                    continue;
                r.key = near;
            }
        }
        const qreal top = topOf(i);
        const bool isVisible = top + m_h[size_t(i)] > m_viewTop && top < m_viewBottom;
        r.priority = isVisible ? 0 : 1;
        (isVisible ? visible : ahead).push_back(r);
    }
    if (m_scrollDir < 0)
        std::reverse(ahead.begin(), ahead.end()); // nearest first
    visible += ahead;
    media->setWanted(this, visible);
    if (settle)
        m_mediaSettle.start(int(qMax<qint64>(1, kResizeSettleMs - m_widthChanged.elapsed())));
}

} // namespace folia

#include "VirtualLayout.moc"
