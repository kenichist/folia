#include "core/LucideIcons.h"

#include <QCache>
#include <QHashFunctions>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QtMath>

#include <cmath>

namespace folia::icons {

namespace {

constexpr qreal kPi = 3.14159265358979323846;

struct Source {
    const char *name;
    const char *svg;
};

// Element markup copied verbatim from lucide-static 0.469.0.
// Lucide is ISC licensed: Copyright (c) for portions of Lucide are held by
// Cole Bemis 2013-2022 as part of Feather (MIT); all other copyright (c) for
// Lucide are held by Lucide Contributors 2022.
const Source kSources[] = {
    {"arrow-left-right",
     R"svg(<path d="M8 3 4 7l4 4" /><path d="M4 7h16" /><path d="m16 21 4-4-4-4" /><path d="M20 17H4" />)svg"},
    {"check",
     R"svg(<path d="M20 6 9 17l-5-5" />)svg"},
    {"chevron-down",
     R"svg(<path d="m6 9 6 6 6-6" />)svg"},
    {"chevron-left",
     R"svg(<path d="m15 18-6-6 6-6" />)svg"},
    {"chevron-right",
     R"svg(<path d="m9 18 6-6-6-6" />)svg"},
    {"copy",
     R"svg(<rect width="14" height="14" x="8" y="8" rx="2" ry="2" /><path d="M4 16c-1.1 0-2-.9-2-2V4c0-1.1.9-2 2-2h10c1.1 0 2 .9 2 2" />)svg"},
    {"external-link",
     R"svg(<path d="M15 3h6v6" /><path d="M10 14 21 3" /><path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6" />)svg"},
    {"file-plus",
     R"svg(<path d="M15 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V7Z" /><path d="M14 2v4a2 2 0 0 0 2 2h4" /><path d="M9 15h6" /><path d="M12 18v-6" />)svg"},
    {"file-text",
     R"svg(<path d="M15 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V7Z" /><path d="M14 2v4a2 2 0 0 0 2 2h4" /><path d="M10 9H8" /><path d="M16 13H8" /><path d="M16 17H8" />)svg"},
    {"globe",
     R"svg(<circle cx="12" cy="12" r="10" /><path d="M12 2a14.5 14.5 0 0 0 0 20 14.5 14.5 0 0 0 0-20" /><path d="M2 12h20" />)svg"},
    {"link",
     R"svg(<path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71" /><path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71" />)svg"},
    {"panel-left-close",
     R"svg(<rect width="18" height="18" x="3" y="3" rx="2" /><path d="M9 3v18" /><path d="m16 15-3-3 3-3" />)svg"},
    {"panel-left-open",
     R"svg(<rect width="18" height="18" x="3" y="3" rx="2" /><path d="M9 3v18" /><path d="m14 9 3 3-3 3" />)svg"},
    {"pencil",
     R"svg(<path d="M21.174 6.812a1 1 0 0 0-3.986-3.987L3.842 16.174a2 2 0 0 0-.5.83l-1.321 4.352a.5.5 0 0 0 .623.622l4.353-1.32a2 2 0 0 0 .83-.497z" /><path d="m15 5 4 4" />)svg"},
    {"plus",
     R"svg(<path d="M5 12h14" /><path d="M12 5v14" />)svg"},
    {"rotate-ccw",
     R"svg(<path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8" /><path d="M3 3v5h5" />)svg"},
    {"settings",
     R"svg(<path d="M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z" /><circle cx="12" cy="12" r="3" />)svg"},
    {"trash-2",
     R"svg(<path d="M3 6h18" /><path d="M19 6v14c0 1-1 2-2 2H7c-1 0-2-1-2-2V6" /><path d="M8 6V4c0-1 1-2 2-2h4c1 0 2 1 2 2v2" /><line x1="10" x2="10" y1="11" y2="17" /><line x1="14" x2="14" y1="11" y2="17" />)svg"},
    {"unlink",
     R"svg(<path d="m18.84 12.25 1.72-1.71h-.02a5.004 5.004 0 0 0-.12-7.07 5.006 5.006 0 0 0-6.95 0l-1.72 1.71" /><path d="m5.17 11.75-1.71 1.71a5.004 5.004 0 0 0 .12 7.07 5.006 5.006 0 0 0 6.95 0l1.71-1.71" /><line x1="8" x2="8" y1="2" y2="5" /><line x1="2" x2="5" y1="8" y2="8" /><line x1="16" x2="16" y1="19" y2="22" /><line x1="19" x2="22" y1="16" y2="16" />)svg"},
    {"upload",
     R"svg(<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" /><polyline points="17 8 12 3 7 8" /><line x1="12" x2="12" y1="3" y2="15" />)svg"},
    {"x",
     R"svg(<path d="M18 6 6 18" /><path d="m6 6 12 12" />)svg"},
};

// ------------------------------------------------------- path data -----
// SVG path data ("d"): M L H V C S Q T A Z, absolute and relative, with
// implicit command repetition and compact number syntax ("1.1.9", "2-2").
class PathData {
public:
    PathData(const QString &d, QPainterPath &out) : s(d), n(int(d.size())), path(out) {}

    void run()
    {
        QChar cmd;
        QPointF cur, start, lastCubic, lastQuad;
        QChar prev;
        while (true) {
            skip();
            if (i >= n)
                break;
            const int before = i;
            const QChar c = s[i];
            if (c.isLetter()) {
                cmd = c;
                ++i;
            } else if (cmd.isNull()) {
                return; // malformed: numbers before any command
            }
            const bool rel = cmd.isLower();
            const QPointF base = rel ? cur : QPointF();
            const char up = char(cmd.toUpper().unicode());
            switch (up) {
            case 'M': {
                const QPointF p = base + point();
                path.moveTo(p);
                cur = start = p;
                cmd = rel ? QLatin1Char('l') : QLatin1Char('L'); // further pairs are line-tos
                break;
            }
            case 'L': {
                cur = base + point();
                path.lineTo(cur);
                break;
            }
            case 'H': {
                const qreal x = number();
                cur.setX(rel ? cur.x() + x : x);
                path.lineTo(cur);
                break;
            }
            case 'V': {
                const qreal y = number();
                cur.setY(rel ? cur.y() + y : y);
                path.lineTo(cur);
                break;
            }
            case 'C': {
                const QPointF c1 = base + point();
                const QPointF c2 = base + point();
                const QPointF p = base + point();
                path.cubicTo(c1, c2, p);
                lastCubic = c2;
                cur = p;
                break;
            }
            case 'S': {
                const QPointF c1 = (prev == QLatin1Char('C') || prev == QLatin1Char('S')) ? 2 * cur - lastCubic : cur;
                const QPointF c2 = base + point();
                const QPointF p = base + point();
                path.cubicTo(c1, c2, p);
                lastCubic = c2;
                cur = p;
                break;
            }
            case 'Q': {
                const QPointF c1 = base + point();
                const QPointF p = base + point();
                path.quadTo(c1, p);
                lastQuad = c1;
                cur = p;
                break;
            }
            case 'T': {
                const QPointF c1 = (prev == QLatin1Char('Q') || prev == QLatin1Char('T')) ? 2 * cur - lastQuad : cur;
                const QPointF p = base + point();
                path.quadTo(c1, p);
                lastQuad = c1;
                cur = p;
                break;
            }
            case 'A': {
                const qreal rx = number(), ry = number(), rot = number();
                const bool large = flag(), sweep = flag();
                const QPointF p = base + point();
                arcTo(cur, rx, ry, rot, large, sweep, p);
                cur = p;
                break;
            }
            case 'Z':
                path.closeSubpath();
                cur = start;
                break;
            default:
                return; // unsupported command: stop rather than guess
            }
            prev = QChar(QLatin1Char(up));
            if (bad || i == before)
                return; // malformed data: stop rather than loop

        }
    }

private:
    void skip()
    {
        while (i < n && (s[i].isSpace() || s[i] == QLatin1Char(',')))
            ++i;
    }

    qreal number()
    {
        skip();
        const int b = i;
        if (i < n && (s[i] == QLatin1Char('-') || s[i] == QLatin1Char('+')))
            ++i;
        while (i < n && s[i].isDigit())
            ++i;
        if (i < n && s[i] == QLatin1Char('.')) {
            ++i;
            while (i < n && s[i].isDigit())
                ++i;
        }
        if (i < n && (s[i] == QLatin1Char('e') || s[i] == QLatin1Char('E'))) {
            int j = i + 1;
            if (j < n && (s[j] == QLatin1Char('-') || s[j] == QLatin1Char('+')))
                ++j;
            if (j < n && s[j].isDigit()) {
                i = j;
                while (i < n && s[i].isDigit())
                    ++i;
            }
        }
        bool ok = false;
        const qreal v = QStringView(s).mid(b, i - b).toDouble(&ok);
        if (!ok)
            bad = true;
        return v;
    }

    bool flag()
    {
        skip();
        if (i < n && (s[i] == QLatin1Char('0') || s[i] == QLatin1Char('1')))
            return s[i++] == QLatin1Char('1');
        bad = true;
        return false;
    }

    QPointF point()
    {
        const qreal x = number();
        const qreal y = number();
        return QPointF(x, y);
    }

    // SVG elliptical arc (endpoint parameterization, SVG 1.1 appendix F.6)
    // approximated by cubic Béziers of at most 90° each.
    void arcTo(QPointF p0, qreal rx, qreal ry, qreal phiDeg, bool large, bool sweep, QPointF p1)
    {
        if (p0 == p1)
            return;
        rx = std::fabs(rx);
        ry = std::fabs(ry);
        if (rx == 0 || ry == 0) {
            path.lineTo(p1);
            return;
        }
        const qreal phi = qDegreesToRadians(phiDeg);
        const qreal cp = std::cos(phi), sp = std::sin(phi);
        const qreal dx2 = (p0.x() - p1.x()) / 2, dy2 = (p0.y() - p1.y()) / 2;
        const qreal x1 = cp * dx2 + sp * dy2;
        const qreal y1 = -sp * dx2 + cp * dy2;
        const qreal lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
        if (lambda > 1) {
            const qreal k = std::sqrt(lambda);
            rx *= k;
            ry *= k;
        }
        const qreal rx2 = rx * rx, ry2 = ry * ry;
        const qreal num = rx2 * ry2 - rx2 * y1 * y1 - ry2 * x1 * x1;
        const qreal den = rx2 * y1 * y1 + ry2 * x1 * x1;
        qreal coef = den == 0 ? 0 : std::sqrt(qMax<qreal>(0, num / den));
        if (large == sweep)
            coef = -coef;
        const qreal cxp = coef * (rx * y1 / ry);
        const qreal cyp = coef * -(ry * x1 / rx);
        const qreal cx = cp * cxp - sp * cyp + (p0.x() + p1.x()) / 2;
        const qreal cy = sp * cxp + cp * cyp + (p0.y() + p1.y()) / 2;
        auto angle = [](qreal ux, qreal uy, qreal vx, qreal vy) {
            return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        };
        const qreal ux = (x1 - cxp) / rx, uy = (y1 - cyp) / ry;
        const qreal vx = (-x1 - cxp) / rx, vy = (-y1 - cyp) / ry;
        const qreal theta = angle(1, 0, ux, uy);
        qreal delta = angle(ux, uy, vx, vy);
        if (!sweep && delta > 0)
            delta -= 2 * kPi;
        else if (sweep && delta < 0)
            delta += 2 * kPi;
        const int segs = qMax(1, int(std::ceil(std::fabs(delta) / (kPi / 2) - 1e-9)));
        const qreal step = delta / segs;
        const qreal t = 4.0 / 3.0 * std::tan(step / 4);
        auto map = [&](qreal ex, qreal ey) {
            const qreal x = rx * ex, y = ry * ey;
            return QPointF(cp * x - sp * y + cx, sp * x + cp * y + cy);
        };
        qreal a = theta;
        for (int k = 0; k < segs; ++k) {
            const qreal b = a + step;
            const qreal ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
            path.cubicTo(map(ca - t * sa, sa + t * ca), map(cb + t * sb, sb - t * cb),
                         k == segs - 1 ? p1 : map(cb, sb));
            a = b;
        }
    }

    const QString &s;
    const int n;
    int i = 0;
    bool bad = false;
    QPainterPath &path;
};

// ---------------------------------------------------------- elements -----
// A tiny scanner for the embedded markup (`<tag a="v" b="w" />` only), so
// no regular-expression engine is compiled at startup.
QPainterPath parseIcon(QLatin1StringView m)
{
    QPainterPath path;
    const qsizetype n = m.size();
    qsizetype i = 0;
    struct Attr { QLatin1StringView name, value; };
    while ((i = m.indexOf(QLatin1Char('<'), i)) >= 0) {
        const qsizetype t = ++i;
        while (i < n && QChar(m[i]).isLetter())
            ++i;
        const QLatin1StringView tag = m.sliced(t, i - t);
        Attr attrs[8];
        int count = 0;
        while (i < n && m[i] != QLatin1Char('>')) {
            while (i < n && (m[i] == QLatin1Char(' ') || m[i] == QLatin1Char('/')))
                ++i;
            if (i >= n || m[i] == QLatin1Char('>'))
                break;
            const qsizetype a = i;
            while (i < n && m[i] != QLatin1Char('=') && m[i] != QLatin1Char(' ') && m[i] != QLatin1Char('>'))
                ++i;
            const QLatin1StringView name = m.sliced(a, i - a);
            QLatin1StringView value;
            if (i + 1 < n && m[i] == QLatin1Char('=') && m[i + 1] == QLatin1Char('"')) {
                const qsizetype v = i += 2;
                while (i < n && m[i] != QLatin1Char('"'))
                    ++i;
                value = m.sliced(v, i - v);
                ++i;
            }
            if (count < 8)
                attrs[count++] = {name, value};
        }
        auto str = [&](const char *k) {
            for (int j = 0; j < count; ++j)
                if (attrs[j].name == QLatin1StringView(k))
                    return attrs[j].value;
            return QLatin1StringView();
        };
        auto num = [&](const char *k, qreal def = 0) {
            bool ok = false;
            const qreal v = QString(str(k)).toDouble(&ok);
            return ok ? v : def;
        };
        if (tag == QLatin1StringView("path")) {
            PathData(QString(str("d")), path).run();
        } else if (tag == QLatin1StringView("rect")) {
            const QRectF r(num("x"), num("y"), num("width"), num("height"));
            qreal rx = num("rx", -1), ry = num("ry", -1);
            if (rx < 0)
                rx = ry;
            if (ry < 0)
                ry = rx;
            rx = qBound<qreal>(0, rx, r.width() / 2);
            ry = qBound<qreal>(0, ry, r.height() / 2);
            if (rx > 0 && ry > 0)
                path.addRoundedRect(r, rx, ry);
            else
                path.addRect(r);
        } else if (tag == QLatin1StringView("circle")) {
            path.addEllipse(QPointF(num("cx"), num("cy")), num("r"), num("r"));
        } else if (tag == QLatin1StringView("ellipse")) {
            path.addEllipse(QPointF(num("cx"), num("cy")), num("rx"), num("ry"));
        } else if (tag == QLatin1StringView("line")) {
            path.moveTo(num("x1"), num("y1"));
            path.lineTo(num("x2"), num("y2"));
        } else if (tag == QLatin1StringView("polyline") || tag == QLatin1StringView("polygon")) {
            // "x,y x,y …" is path data once prefixed with a move-to.
            QString d = QStringLiteral("M") + QString(str("points"));
            if (tag == QLatin1StringView("polygon"))
                d += QLatin1Char('Z');
            PathData(d, path).run();
        }
    }
    return path;
}

const Source *findSource(const QString &name)
{
    for (const Source &s : kSources)
        if (name == QLatin1StringView(s.name))
            return &s;
    return nullptr;
}

// Icons are parsed on first use only (most sessions draw a handful).
bool pathFor(const QString &name, QPainterPath *out)
{
    static QHash<QString, QPainterPath> parsed;
    if (const auto it = parsed.constFind(name); it != parsed.cend()) {
        *out = *it;
        return true;
    }
    const Source *s = findSource(name);
    if (!s)
        return false;
    *out = parseIcon(QLatin1StringView(s->svg));
    parsed.insert(name, *out);
    return true;
}

void strokeInto(QPainter *p, const QPainterPath &path, const QRectF &rect, const QColor &color, qreal strokeWidth)
{
    const qreal s = qMin(rect.width(), rect.height()) / 24.0;
    if (s <= 0)
        return;
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    p->translate(rect.center().x() - 12 * s, rect.center().y() - 12 * s);
    p->scale(s, s);
    QPen pen(color, strokeWidth);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p->setPen(pen);
    p->setBrush(Qt::NoBrush);
    p->drawPath(path);
    p->restore();
}

// Rendered icons, keyed by name/color/size/stroke. Rows, chips and buttons
// repaint often; blitting a cached pixmap is far cheaper than stroking the
// paths again. Capped in bytes (LRU), dropped on memory pressure. The key is
// a plain value (the name is shared, not copied), so a lookup allocates nothing.
struct PixKey {
    QString name;
    QRgb rgba;
    int w, h, stroke, dpr;
    bool operator==(const PixKey &o) const
    {
        return rgba == o.rgba && w == o.w && h == o.h && stroke == o.stroke && dpr == o.dpr && name == o.name;
    }
};
size_t qHash(const PixKey &k, size_t seed = 0)
{
    return qHashMulti(seed, k.name, k.rgba, k.w, k.h, k.stroke, k.dpr);
}

QCache<PixKey, QPixmap> &pixmapCache()
{
    static QCache<PixKey, QPixmap> cache(192 * 1024);
    return cache;
}

QPixmap cachedPixmap(const QString &name, QSize devSize, qreal dpr, const QColor &color, qreal strokeWidth)
{
    const PixKey key{name, color.rgba(), devSize.width(), devSize.height(), qRound(strokeWidth * 100), qRound(dpr * 100)};
    if (const QPixmap *hit = pixmapCache().object(key))
        return *hit;
    QPainterPath path;
    if (!pathFor(name, &path) || devSize.isEmpty())
        return QPixmap();
    QPixmap pm(devSize);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    {
        QPainter q(&pm);
        strokeInto(&q, path, QRectF(QPointF(0, 0), QSizeF(devSize) / dpr), color, strokeWidth);
    }
    pixmapCache().insert(key, new QPixmap(pm), qMax(1, devSize.width() * devSize.height() * 4));
    return pm;
}

class LucideEngine : public QIconEngine {
public:
    LucideEngine(const QString &name, const QColor &color) : m_name(name), m_color(color) {}

    void paint(QPainter *p, const QRect &rect, QIcon::Mode mode, QIcon::State) override
    {
        icons::paint(p, m_name, rect, colorFor(mode));
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State, qreal scale) override
    {
        const QSize dev(qCeil(size.width() * scale), qCeil(size.height() * scale));
        return cachedPixmap(m_name, dev, scale, colorFor(mode), 2.0);
    }
    QIconEngine *clone() const override { return new LucideEngine(m_name, m_color); }
    QString key() const override { return QStringLiteral("folia-lucide"); }

private:
    QColor colorFor(QIcon::Mode mode) const
    {
        QColor c = m_color;
        if (mode == QIcon::Disabled)
            c.setAlphaF(c.alphaF() * 0.35);
        return c;
    }
    QString m_name;
    QColor m_color;
};

} // namespace

bool has(const QString &name) { return findSource(name) != nullptr; }

QStringList names()
{
    QStringList out;
    for (const Source &s : kSources)
        out << QString::fromLatin1(s.name);
    out.sort();
    return out;
}

bool paint(QPainter *p, const QString &name, const QRectF &rect, const QColor &color, qreal strokeWidth)
{
    const QTransform &tf = p->transform();
    const qreal dpr = p->device() ? p->device()->devicePixelRatio() : 1.0;
    if (tf.type() <= QTransform::TxTranslate) {
        const QSize dev(qRound(rect.width() * dpr), qRound(rect.height() * dpr));
        const QPixmap pm = cachedPixmap(name, dev, dpr, color, strokeWidth);
        if (pm.isNull())
            return has(name);
        // Snap to device pixels so the cached bitmap is not resampled.
        const qreal dx = tf.dx(), dy = tf.dy();
        const QPointF at(std::round((rect.x() + dx) * dpr) / dpr - dx, std::round((rect.y() + dy) * dpr) / dpr - dy);
        p->drawPixmap(at, pm);
        return true;
    }
    QPainterPath path; // scaled/rotated painter: draw vectors
    if (!pathFor(name, &path))
        return false;
    strokeInto(p, path, rect, color, strokeWidth);
    return true;
}

void clearCache() { pixmapCache().clear(); }

QIcon icon(const QString &name, const QColor &color)
{
    if (!has(name))
        return QIcon();
    return QIcon(new LucideEngine(name, color));
}

} // namespace folia::icons
