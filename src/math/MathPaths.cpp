// Synthetic, resolution-independent outlines for stretchy delimiters, radical
// signs and horizontal braces. Used when the math font has no OpenType MATH
// construction for a character (or no MATH table at all), so delimiters stay
// correct at any height instead of being stretched bitmaps or clipped glyphs.
#include "math/MathPaths.h"

#include <QPainterPathStroker>
#include <QTransform>

namespace folia {
namespace math {
namespace detail {

namespace {

QPainterPath stroked(const QPainterPath &center, qreal width, Qt::PenCapStyle cap = Qt::RoundCap)
{
    QPainterPathStroker s;
    s.setWidth(width);
    s.setCapStyle(cap);
    s.setJoinStyle(Qt::RoundJoin);
    QPainterPath p = s.createStroke(center);
    p.setFillRule(Qt::WindingFill);
    return p;
}

QPainterPath mirrored(const QPainterPath &p, qreal width)
{
    QPainterPath m = QTransform(-1, 0, 0, 1, width, 0).map(p);
    m.setFillRule(Qt::WindingFill);
    return m;
}

} // namespace

QPainterPath syntheticVertical(char32_t cp, qreal H, qreal em, qreal *advance)
{
    const qreal t = qBound(0.05 * em, 0.03 * H, 0.09 * em);
    QPainterPath p;
    p.setFillRule(Qt::WindingFill);
    qreal W = 0;
    switch (cp) {
    case '(':
    case ')': {
        W = qMin(0.34 * em + 0.05 * H, 0.9 * em);
        const qreal xl = 0.1 * em, te = 0.45 * t, thick = 1.3 * t;
        const qreal xr = W - te - 0.06 * em;
        p.moveTo(xr, -H);
        p.quadTo(2 * xl - xr, -H / 2, xr, 0);
        p.lineTo(xr + te, 0);
        p.quadTo(2 * (xl + thick) - (xr + te), -H / 2, xr + te, -H);
        p.closeSubpath();
        if (cp == ')')
            p = mirrored(p, W);
        break;
    }
    case '[': case ']': case 0x230A: case 0x230B: case 0x2308: case 0x2309:
    case 0x27E6: case 0x27E7: {
        const bool dbl = cp == 0x27E6 || cp == 0x27E7;
        const qreal x0 = 0.1 * em, bar = 1.1 * t, ts = 0.75 * t;
        const qreal serif = (dbl ? 0.36 : 0.24) * em;
        W = x0 + serif + 0.08 * em;
        p.addRect(QRectF(x0, -H, bar, H));
        if (dbl)
            p.addRect(QRectF(x0 + 0.16 * em, -H, 0.6 * bar, H));
        const bool top = cp != 0x230A && cp != 0x230B;
        const bool bottom = cp != 0x2308 && cp != 0x2309;
        if (top)
            p.addRect(QRectF(x0, -H, serif, ts));
        if (bottom)
            p.addRect(QRectF(x0, -ts, serif, ts));
        if (cp == ']' || cp == 0x230B || cp == 0x2309 || cp == 0x27E7)
            p = mirrored(p, W);
        break;
    }
    case '{':
    case '}': {
        W = qMin(0.46 * em + 0.04 * H, 0.9 * em);
        const qreal sw = 0.9 * t;
        const qreal xm = W / 2, x0 = 0.08 * em + sw / 2, x1 = W - 0.08 * em - sw / 2;
        const qreal r = qMin(0.2 * em, H / 6);
        const qreal top = -H + sw / 2, bot = -sw / 2, mid = -H / 2;
        QPainterPath c;
        c.moveTo(x1, top);
        c.quadTo(xm, top, xm, top + r);
        c.lineTo(xm, mid - r);
        c.quadTo(xm, mid, x0, mid);
        c.quadTo(xm, mid, xm, mid + r);
        c.lineTo(xm, bot - r);
        c.quadTo(xm, bot, x1, bot);
        p = stroked(c, sw);
        if (cp == '}')
            p = mirrored(p, W);
        break;
    }
    case '|':
        W = 0.28 * em;
        p.addRect(QRectF(W / 2 - t / 2, -H, t, H));
        break;
    case 0x2016:
        W = 0.46 * em;
        p.addRect(QRectF(W / 2 - 0.09 * em - t / 2, -H, t, H));
        p.addRect(QRectF(W / 2 + 0.09 * em - t / 2, -H, t, H));
        break;
    case 0x27E8:
    case 0x27E9: {
        W = qMin(0.36 * em + 0.08 * H, 1.0 * em);
        const qreal sw = 0.85 * t;
        QPainterPath c;
        c.moveTo(W - 0.08 * em, -H + sw / 2);
        c.lineTo(0.08 * em + sw / 2, -H / 2);
        c.lineTo(W - 0.08 * em, -sw / 2);
        p = stroked(c, sw, Qt::FlatCap);
        if (cp == 0x27E9)
            p = mirrored(p, W);
        break;
    }
    case '/':
    case '\\': {
        W = qMin(0.12 * em + 0.4 * H, 2.0 * em);
        QPainterPath c;
        c.moveTo(W - 0.06 * em, -H);
        c.lineTo(0.06 * em, 0);
        p = stroked(c, t, Qt::FlatCap);
        if (cp == '\\')
            p = mirrored(p, W);
        break;
    }
    case 0x221A: {
        W = qMin(0.62 * em + 0.02 * H, 1.1 * em);
        QPainterPath a, b, c;
        a.moveTo(0.04 * em, -0.46 * em);
        a.lineTo(0.17 * em, -0.53 * em);
        b.moveTo(0.17 * em, -0.53 * em);
        b.lineTo(0.42 * em, -t * 0.4);
        c.moveTo(0.42 * em, -t * 0.4);
        c.lineTo(W - 0.02 * em, -H + 0.35 * t);
        p = stroked(a, 0.8 * t);
        p.addPath(stroked(b, 1.9 * t));
        p.addPath(stroked(c, 0.7 * t));
        p.setFillRule(Qt::WindingFill);
        break;
    }
    default:
        break;
    }
    if (advance)
        *advance = W;
    return p;
}

QPainterPath syntheticHorizontal(char32_t cp, qreal W, qreal em)
{
    // Build the vertical counterpart (height W) and rotate it.
    char32_t vert = 0;
    bool over = true;
    switch (cp) {
    case 0x23DE: vert = '{'; over = true; break;
    case 0x23DF: vert = '{'; over = false; break;
    case 0x23DC: vert = '('; over = true; break;
    case 0x23DD: vert = '('; over = false; break;
    case 0x23B4: vert = '['; over = true; break;
    case 0x23B5: vert = '['; over = false; break;
    default: return QPainterPath();
    }
    qreal depth = 0;
    const QPainterPath v = syntheticVertical(vert, W, em, &depth);
    // vertical: x in [0, depth], y in [-W, 0], opening towards +x.
    QTransform t = over ? QTransform(0, 1, -1, 0, 0, -depth) : QTransform(0, -1, -1, 0, 0, 0);
    QPainterPath h = t.map(v);
    h.setFillRule(Qt::WindingFill);
    return h;
}

QPainterPath notSlash(qreal cx, qreal cy, qreal halfHeight, qreal em)
{
    QPainterPath c;
    c.moveTo(cx + 0.42 * halfHeight, cy - halfHeight);
    c.lineTo(cx - 0.42 * halfHeight, cy + halfHeight);
    return stroked(c, 0.055 * em, Qt::FlatCap);
}

} // namespace detail
} // namespace math
} // namespace folia
