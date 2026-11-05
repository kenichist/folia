#include "editor/VirtualLayout.h"

#include "core/Instrument.h"
#include "core/LucideIcons.h"
#include "editor/Links.h"
#ifdef FOLIA_WITH_MATH
#include "math/MathRenderer.h"
#endif

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextLayout>
#include <QTextObjectInterface>

#include <algorithm>
#include <cmath>

namespace folia {

namespace {

constexpr qreal kBottomSlack = 160; // scroll past the end
constexpr qreal kCodeInset = 14;
constexpr qreal kPreviewPad = 8;
constexpr qreal kCopyIcon = 13;

LineRole roleOf(const QTextBlock &b)
{
    const int s = b.userState();
    return s < 0 ? LineRole::Para : LineRole(s);
}

BlockData *dataOf(const QTextBlock &b) { return static_cast<BlockData *>(b.userData()); }

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
    default:
        return ps;
    }
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
    const QFont f = roleFont(block, role);
    const QFontMetricsF fm(f);
    const qreal lw = qMax<qreal>(40, columnWidth() - 2 * indentFor(role));
    const qreal chars = qMax(0, block.length() - 1);
    const qreal lines = qMax<qreal>(1, std::ceil(chars * fm.averageCharWidth() / lw));
    qreal h = topPadFor(block, role) + lines * fm.lineSpacing() * m_ctx->theme().lineSpacing
        + bottomPadFor(role);
    if (role == LineRole::MathClose || role == LineRole::MathSingle) {
        BlockData *d = dataOf(block);
        h += (d && d->previewHeight > 0) ? d->previewHeight : 2 * fm.lineSpacing();
    }
    return float(h);
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
    QTextDocument *doc = document();
    const int n = doc->blockCount();
    m_h.assign(size_t(n), 0.f);
    m_st.assign(size_t(n), 0);
    m_top.assign(size_t(n), 0.0);
    m_topValid = 0;
    m_sumH = 0;
    m_laidOut = 0;
    int i = 0;
    for (QTextBlock b = doc->begin(); b.isValid() && i < n; b = b.next(), ++i) {
        if (b.layout()->lineCount() > 0)
            b.layout()->clearLayout();
        const float h = estimateHeight(b);
        m_h[size_t(i)] = h;
        m_sumH += h;
    }
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
    if (w != m_width || m_h.empty()) {
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
    for (int i = first; i <= lastOld; ++i) {
        m_sumH -= m_h[size_t(i)];
        if (m_st[size_t(i)] & StLaidOut)
            --m_laidOut;
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
            if (b.layout()->lineCount() > 0)
                b.layout()->clearLayout();
        }
    }
    emit documentSizeChanged(documentSize());
    emit update(QRectF(0, topOf(first), 1e9, 1e9));
    Q_UNUSED(charsRemoved);
}

// ------------------------------------------------------------- formats ----
void VirtualLayout::applyFormats(const QTextBlock &block, LineRole role, QTextLayout *tl) const
{
    const EditorTheme &t = m_ctx->theme();
    const QString text = block.text();
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
    QTextCharFormat base;
    base.setFont(roleFont(block, role), QTextCharFormat::FontPropertiesAll);
    base.setForeground(t.text);
    add(0, len, base);
    QTextCharFormat dimF;
    dimF.setForeground(t.dim);
    QTextCharFormat mathF;
    mathF.setForeground(t.mathSource);

    switch (role) {
    case LineRole::CodeOpen:
    case LineRole::CodeClose:
    case LineRole::MathOpen:
    case LineRole::MathClose:
        add(0, len, dimF);
        break;
    case LineRole::MathLine:
        add(0, len, mathF);
        break;
    case LineRole::MathSingle:
        add(0, 2, dimF);
        add(2, len - 4, mathF);
        add(len - 2, 2, dimF);
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
        const int h = headingLevel(text);
        if (h) {
            add(0, h + 1, dimF);
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
                    add(i, 1, dimF);
                    add(i + 1, e - i - 1, mathF);
                    add(e, 1, dimF);
                    i = e + 1;
                    continue;
                }
            } else if (c == QLatin1Char('*') && i + 1 < len && text[i + 1] == QLatin1Char('*')) {
                const int e = text.indexOf(QLatin1String("**"), i + 2);
                if (e > i + 2) {
                    QTextCharFormat f;
                    f.setFontWeight(QFont::Bold);
                    add(i, 2, dimF);
                    add(i + 2, e - i - 2, f);
                    add(e, 2, dimF);
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
                    add(i, 1, dimF);
                    add(e, 1, dimF);
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
        break;
    }
    default:
        break;
    }

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
}

// -------------------------------------------------------------- layout ----
void VirtualLayout::layoutBlock(const QTextBlock &block, int number) const
{
    instr::add(instr::BlocksLaidOut);
    const EditorTheme &t = m_ctx->theme();
    QTextLayout *tl = block.layout();
    const LineRole role = roleOf(block);
    applyFormats(block, role, tl);
    QTextOption opt = document()->defaultTextOption();
    opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    tl->setTextOption(opt);

    const qreal indent = indentFor(role);
    const qreal x = columnLeft() + indent;
    const qreal lw = qMax<qreal>(40, columnWidth() - 2 * indent);
    qreal y = topPadFor(block, role);
    tl->beginLayout();
    for (;;) {
        QTextLine line = tl->createLine();
        if (!line.isValid())
            break;
        line.setLeadingIncluded(true);
        line.setLineWidth(lw);
        line.setPosition(QPointF(x, y));
        y += line.height() * t.lineSpacing;
    }
    tl->endLayout();
    if (role == LineRole::MathClose || role == LineRole::MathSingle) {
        BlockData *d = dataOf(block);
        if (d) {
            requestPreview(block, d, role);
            y += d->previewHeight;
        }
    }
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
    QTextDocument *doc = document();
    if (m_h.empty() || int(m_h.size()) != doc->blockCount())
        fullInvalidate();
    QRectF clip = ctx.clip;
    if (!clip.isValid())
        clip = QRectF(0, m_viewTop, m_width, qMax<qreal>(1, m_viewBottom - m_viewTop));
    m_inPaint = true;
    m_heightChangedInPaint = false;
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
                sels.push_back(fr);
            if (se > bpos + blen - 1 && ss < bpos + blen) {
                selectionCoversSeparator = true;
                selFmt = s.format;
            }
        }
        const QPointF origin(0, top);
        tl->draw(painter, origin, sels, clip);
        if (selectionCoversSeparator && tl->lineCount() > 0) {
            const QTextLine ll = tl->lineAt(tl->lineCount() - 1);
            const QRectF sep(ll.x() + ll.naturalTextWidth(), top + ll.y(), 6, ll.height());
            painter->fillRect(sep, selFmt.background());
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
    return b.position() + qBound(0, off, b.length() - 1);
}

// ----------------------------------------------------- inline objects -----
void VirtualLayout::resizeInlineObject(QTextInlineObject item, int pos, const QTextFormat &format)
{
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
    if (format.objectType() != kPageLinkObjectType)
        QAbstractTextDocumentLayout::positionInlineObject(item, pos, format);
}

void VirtualLayout::drawInlineObject(QPainter *p, const QRectF &rect, QTextInlineObject object,
                                     int pos, const QTextFormat &format)
{
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

} // namespace folia

#include "VirtualLayout.moc"
