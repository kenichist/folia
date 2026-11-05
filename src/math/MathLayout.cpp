// TeX-style math layout (TeXbook Appendix G, using OpenType MATH constants
// the way MathML Core does). Boxes are laid out bottom-up into one flat item
// vector; a Box is a contiguous item range positioned relative to its own
// baseline origin, so moving a box is a loop over its range. Recursion depth
// follows the AST, which the parser bounds by Ctx::maxDepth.
#include "math/MathLayout.h"
#include "math/MathPaths.h"

#include <QTransform>
#include <QVarLengthArray>
#include <cmath>

namespace folia {
namespace math {
namespace detail {

namespace {

struct MStyle {
    quint8 lvl = 0; // 0 display, 1 text, 2 script, 3 scriptscript
    bool cramped = false;
};
inline MStyle supStyle(MStyle s) { return {quint8(s.lvl <= 1 ? 2 : 3), s.cramped}; }
inline MStyle subStyle(MStyle s) { return {quint8(s.lvl <= 1 ? 2 : 3), true}; }
inline MStyle numStyle(MStyle s) { return {quint8(s.lvl == 0 ? 1 : s.lvl == 1 ? 2 : 3), s.cramped}; }
inline MStyle denStyle(MStyle s) { return {numStyle(s).lvl, true}; }
inline MStyle cramp(MStyle s) { return {s.lvl, true}; }

struct Box {
    qreal w = 0, a = 0, d = 0, ic = 0;
    qreal accentX = qQNaN();
    int b = 0, e = 0;
    bool isChar = false;
    bool largeOp = false;
};

struct Item {
    enum T : quint8 { Glyph, Rule, Path } t = Glyph;
    int font = -1, ref = -1;
    qreal x = 0, y = 0, w = 0, h = 0;
    QRectF ink; // relative to (x, y)
};

// TeX inter-atom spacing: 0 none, 1 thin, 2 medium, 3 thick; negative values
// apply only in display/text style.
const qint8 kSpacing[8][8] = {
    //        Ord Op  Bin Rel Open Close Punct Inner
    /*Ord*/   {0,  1, -2, -3,  0,   0,    0,   -1},
    /*Op*/    {1,  1,  0, -3,  0,   0,    0,   -1},
    /*Bin*/   {-2, -2, 0,  0, -2,   0,    0,   -2},
    /*Rel*/   {-3, -3, 0,  0, -3,   0,    0,   -3},
    /*Open*/  {0,  0,  0,  0,  0,   0,    0,    0},
    /*Close*/ {0,  1, -2, -3,  0,   0,    0,   -1},
    /*Punct*/ {-1, -1, 0, -1, -1,  -1,   -1,   -1},
    /*Inner*/ {-1, 1, -2, -3, -1,   0,   -1,   -1},
};

inline QString str(char32_t c) { return QString::fromUcs4(&c, 1); }

inline bool isLatin(char32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline bool isGreekLower(char32_t c)
{
    return (c >= 0x3B1 && c <= 0x3C9) || c == 0x3D1 || c == 0x3D5 || c == 0x3D6 || c == 0x3F0
           || c == 0x3F1 || c == 0x3F5 || c == 0x2202;
}
inline bool isCombiningAccent(char32_t c)
{
    return (c >= 0x300 && c <= 0x36F) || (c >= 0x20D0 && c <= 0x20FF);
}
char32_t spacingAccent(char32_t c)
{
    switch (c) {
    case 0x0300: return 0x60;
    case 0x0301: return 0xB4;
    case 0x0302: return 0x2C6;
    case 0x0303: return 0x2DC;
    case 0x0304: case 0x0305: return 0xAF;
    case 0x0306: return 0x2D8;
    case 0x0307: return 0x2D9;
    case 0x0308: return 0xA8;
    case 0x030A: return 0x2DA;
    case 0x030C: return 0x2C7;
    case 0x20D7: return 0x2192;
    case 0x20D6: return 0x2190;
    case 0x20E1: return 0x2194;
    default: return c;
    }
}

constexpr int kMaxItems = 400000;
constexpr qreal kMaxExtentEm = 4000; // largest width/height of a formula

class Layouter {
public:
    Layouter(const FontData &fd, const Ast &ast, Ctx &ctx, qreal base)
        : fd_(fd), ast_(ast), ctx_(ctx), base_(base)
    {
        items_.reserve(64);
    }

    Box root(bool display)
    {
        const MStyle st{quint8(display ? 0 : 1), false};
        return node(ast_.root, st);
    }

    void flatten(const Box &root, MathDisplayList &out);

private:
    const FontData &fd_;
    const Ast &ast_;
    Ctx &ctx_;
    qreal base_;
    std::vector<Item> items_;
    std::vector<QString> strs_;
    std::vector<QPainterPath> paths_;
    struct FontKey {
        int variant;
        qint64 size;
    };
    std::vector<FontKey> fontKeys_;
    std::vector<QFont> fonts_;

    // ---------------------------------------------------------- helpers --
    // Qt font engines only exist at whole pixel sizes (QFont rounds the
    // pixel size), so lay out with exactly the size that will be drawn.
    qreal size(MStyle s) const
    {
        const qreal f = s.lvl <= 1 ? 1.0 : s.lvl == 2 ? fd_.scriptScale : fd_.scriptScriptScale;
        return qreal(qMax(1, qRound(base_ * f)));
    }
    qreal C(MC k, MStyle s) const { return fd_.c[k] * size(s); }
    qreal axis(MStyle s) const { return C(AxisHeight, s); }

    Box start() const
    {
        Box b;
        b.b = b.e = int(items_.size());
        return b;
    }
    void finish(Box &b) const { b.e = int(items_.size()); }
    void move(const Box &b, qreal dx, qreal dy)
    {
        for (int i = b.b; i < b.e; ++i) {
            items_[size_t(i)].x += dx;
            items_[size_t(i)].y += dy;
        }
    }
    bool pushItem(const Item &it)
    {
        if (items_.size() >= size_t(kMaxItems))
            return ctx_.fail(QStringLiteral("Formula too complex to lay out"), -1);
        items_.push_back(it);
        return true;
    }

    int fontIndex(int variant, qreal px)
    {
        const qint64 key = qRound64(px * 256.0);
        for (size_t i = 0; i < fontKeys_.size(); ++i) {
            if (fontKeys_[i].variant == variant && fontKeys_[i].size == key)
                return int(i);
        }
        fontKeys_.push_back({variant, key});
        fonts_.push_back(makeFont(fd_, variant, px));
        return int(fonts_.size()) - 1;
    }

    Box glyphBox(int variant, const QString &s, qreal px)
    {
        const GlyphMetrics m = measure(fd_, variant, s, &ctx_.coldNanos);
        Box b = start();
        Item it;
        it.t = Item::Glyph;
        it.font = fontIndex(variant, px);
        it.ref = int(strs_.size());
        strs_.push_back(s);
        it.w = m.adv * px;
        it.ink = QRectF(QPointF(m.x0 * px, m.y0 * px), QPointF(m.x1 * px, m.y1 * px));
        pushItem(it);
        finish(b);
        b.w = m.adv * px;
        b.a = qMax<qreal>(0, -m.y0 * px);
        b.d = qMax<qreal>(0, m.y1 * px);
        return b;
    }

    Box pathBox(const QPainterPath &emPath, const QRectF &emInk, qreal adv, qreal sx, qreal sy,
                qreal dx = 0, qreal dy = 0)
    {
        Box b = start();
        const QTransform t(sx, 0, 0, sy, dx, dy);
        Item it;
        it.t = Item::Path;
        it.ref = int(paths_.size());
        QPainterPath p = t.map(emPath);
        p.setFillRule(Qt::WindingFill);
        paths_.push_back(std::move(p));
        it.ink = t.mapRect(emInk);
        pushItem(it);
        finish(b);
        b.w = adv;
        b.a = qMax<qreal>(0, -it.ink.top());
        b.d = qMax<qreal>(0, it.ink.bottom());
        return b;
    }
    Box shapeBox(const GlyphShape &g, qreal px, qreal sy = -1, qreal dy = 0)
    {
        return pathBox(g.path, g.ink, g.adv * px, px, sy < 0 ? px : sy, 0, dy);
    }
    Box rawPathBox(const QPainterPath &p, qreal adv)
    {
        return pathBox(p, p.boundingRect(), adv, 1, 1);
    }

    void addRule(qreal x, qreal y, qreal w, qreal h)
    {
        if (w <= 0 || h <= 0)
            return;
        Item it;
        it.t = Item::Rule;
        it.x = x;
        it.y = y;
        it.w = w;
        it.h = h;
        it.ink = QRectF(0, 0, w, h);
        pushItem(it);
    }

    // Real vertical ink extent of a box (Box::a/d are clamped at the baseline).
    bool inkY(const Box &b, qreal &top, qreal &bottom) const
    {
        bool any = false;
        for (int i = b.b; i < b.e; ++i) {
            const Item &it = items_[size_t(i)];
            if (it.ink.isEmpty())
                continue;
            const qreal t = it.ink.top() + it.y, bt = it.ink.bottom() + it.y;
            top = any ? qMin(top, t) : t;
            bottom = any ? qMax(bottom, bt) : bt;
            any = true;
        }
        if (!any)
            top = bottom = 0;
        return any;
    }

    void centerOnAxis(Box &b, MStyle st)
    {
        const qreal mid = (b.d - b.a) / 2;
        const qreal dy = -axis(st) - mid;
        move(b, 0, dy);
        b.a -= dy;
        b.d += dy;
    }

    qreal spacing(Cls l, Cls r, MStyle st) const
    {
        if (l == Cls::None || r == Cls::None)
            return 0;
        const int v = kSpacing[int(l)][int(r)];
        if (v == 0 || (v < 0 && st.lvl >= 2))
            return 0;
        const qreal mu = size(st) / 18.0;
        switch (v < 0 ? -v : v) {
        case 1: return 3 * mu;
        case 2: return 4 * mu;
        case 3: return 5 * mu;
        default: return 0;
        }
    }

    qreal spaceWidth(const Node &n, MStyle st) const
    {
        const qreal px = size(st);
        if (n.flags & kSpaceMu)
            return n.value * px / 18.0;
        if (n.flags & kSpaceEx)
            return n.value * fd_.xHeight * px;
        return n.value * px;
    }

    // ------------------------------------------------------------ nodes --
    Box node(int idx, MStyle st)
    {
        if (idx < 0 || !ctx_.check())
            return start();
        const Node &n = ast_.nodes[size_t(idx)];
        Box nuc;
        switch (n.kind) {
        case NK::Char: nuc = charBox(n, st); break;
        case NK::Text: nuc = glyphBox(n.variant, n.text, size(st)); break;
        case NK::List: nuc = hlist(ast_.lists[size_t(n.list)], st, Cls::None); break;
        case NK::Frac: nuc = frac(n, st); break;
        case NK::Sqrt: nuc = radical(n, st); break;
        case NK::LeftRight: nuc = leftRight(n, st); break;
        case NK::BigDelim: {
            nuc = delimiter(n.cp, n.value * size(st), st, true);
            break;
        }
        case NK::Accent: nuc = accent(n, st); break;
        case NK::Overline: nuc = overline(n, st); break;
        case NK::Underline: nuc = underline(n, st); break;
        case NK::Brace: nuc = brace(n, st); break;
        case NK::Stack: nuc = stack(n, st); break;
        case NK::Env: nuc = env(n, st); break;
        case NK::Phantom: nuc = phantom(n, st); break;
        case NK::Op: nuc = op(n, st); break;
        case NK::Not: nuc = notBox(n, st); break;
        case NK::Boxed: nuc = boxed(n, st); break;
        case NK::Space: {
            nuc = start();
            nuc.w = spaceWidth(n, st);
            break;
        }
        case NK::Style:
        case NK::Empty:
            nuc = start();
            break;
        }
        if (ctx_.failed)
            return nuc;
        if (n.sup >= 0 || n.sub >= 0 || n.primes) {
            const bool limits = !n.primes
                                && (n.limits == 1 || (n.limits == -1 && n.limitsDefault && st.lvl == 0));
            if (limits)
                return limitsBox(nuc, n, st);
            return scripts(nuc, n, st);
        }
        return nuc;
    }

    int fallbackVariant(char32_t cp, Alpha a) const
    {
        const bool letter = isLatin(cp) || isGreekLower(cp);
        switch (a) {
        case Alpha::Default: return fontVariant(RoleMath, letter, false);
        case Alpha::It: return fontVariant(RoleMath, true, false);
        case Alpha::Rm: return fontVariant(RoleMath);
        case Alpha::Bf: case Alpha::Bb: return fontVariant(RoleMath, false, true);
        case Alpha::BfIt: return fontVariant(RoleMath, true, true);
        case Alpha::Sf: return fontVariant(RoleSans);
        case Alpha::Tt: return fontVariant(RoleMono);
        case Alpha::Cal: case Alpha::Frak: return fontVariant(RoleMath, true, false);
        }
        return fontVariant(RoleMath);
    }

    Box charBox(const Node &n, MStyle st)
    {
        const qreal px = size(st);
        char32_t cp = n.cp;
        int variant = fontVariant(RoleMath);
        const char32_t mapped = mapAlpha(cp, n.alpha);
        bool native = true;
        if (mapped != cp) {
            if (fd_.hasMathAlphanumerics && measure(fd_, variant, str(mapped), &ctx_.coldNanos).inFont) {
                cp = mapped;
            } else {
                variant = fallbackVariant(cp, n.alpha);
                native = false;
            }
        } else if (!fd_.hasMathAlphanumerics && (isLatin(cp) || isGreekLower(cp))) {
            variant = fallbackVariant(cp, n.alpha);
            native = false;
        }
        Box b = glyphBox(variant, str(cp), px);
        b.isChar = true;
        const Item &it = items_[size_t(b.b)];
        if (native && fd_.hasMathTable) {
            b.ic = fd_.italicCorrection(cp) * px;
            const qreal ta = fd_.topAccent(cp);
            if (!std::isnan(ta))
                b.accentX = ta * px;
        } else if (variantItalic(variant)) {
            b.ic = qMax<qreal>(0, it.ink.right() - it.w);
        }
        return b;
    }

    Box hlist(const std::vector<int> &nodes, MStyle st, Cls startCls)
    {
        const int n = int(nodes.size());
        QVarLengthArray<Cls, 64> cls(n);
        int prevIdx = -1;
        Cls prev = startCls;
        for (int i = 0; i < n; ++i) {
            const Node &x = ast_.nodes[size_t(nodes[size_t(i)])];
            if (x.kind == NK::Space || x.kind == NK::Style) {
                cls[i] = Cls::None;
                continue;
            }
            Cls c = x.cls == Cls::None ? Cls::Ord : x.cls;
            if (c == Cls::Bin
                && (prev == Cls::None || prev == Cls::Bin || prev == Cls::Op || prev == Cls::Rel
                    || prev == Cls::Open || prev == Cls::Punct))
                c = Cls::Ord;
            if ((c == Cls::Rel || c == Cls::Close || c == Cls::Punct) && prevIdx >= 0
                && cls[prevIdx] == Cls::Bin)
                cls[prevIdx] = Cls::Ord;
            cls[i] = c;
            prev = c;
            prevIdx = i;
        }
        if (prevIdx >= 0 && cls[prevIdx] == Cls::Bin)
            cls[prevIdx] = Cls::Ord;

        Box out = start();
        qreal x = 0;
        MStyle cur = st;
        Cls last = startCls;
        int atoms = 0;
        Box single;
        for (int i = 0; i < n; ++i) {
            const Node &nd = ast_.nodes[size_t(nodes[size_t(i)])];
            if (nd.kind == NK::Style) {
                cur = MStyle{quint8(nd.style), false};
                continue;
            }
            if (nd.kind == NK::Space && nd.sub < 0 && nd.sup < 0) {
                x += spaceWidth(nd, cur);
                continue;
            }
            Box b = node(nodes[size_t(i)], cur);
            if (ctx_.failed)
                return out;
            if (cls[i] != Cls::None) {
                x += spacing(last, cls[i], cur);
                last = cls[i];
            }
            move(b, x, 0);
            x += b.w;
            out.a = qMax(out.a, b.a);
            out.d = qMax(out.d, b.d);
            ++atoms;
            single = b;
            if (x > kMaxExtentEm * base_ || out.a + out.d > kMaxExtentEm * base_) {
                ctx_.fail(QStringLiteral("Formula too large to display"), -1);
                return out;
            }
        }
        finish(out);
        out.w = x;
        if (atoms == 1 && n == 1) {
            out.ic = single.ic;
            out.isChar = single.isChar;
            out.accentX = single.accentX;
            out.largeOp = single.largeOp;
        }
        return out;
    }

    QString primeString(int count) const
    {
        static const char32_t primes[] = {0x2032, 0x2033, 0x2034, 0x2057};
        QString s;
        while (count > 0) {
            const int k = qMin(count, 4);
            s += str(primes[k - 1]);
            count -= k;
        }
        return s;
    }

    Box scripts(Box nuc, const Node &n, MStyle st)
    {
        const qreal px = size(st);
        const bool hasSup = n.sup >= 0, hasSub = n.sub >= 0;
        qreal supX = nuc.w + (nuc.largeOp ? 0 : nuc.ic);
        const qreal subX = nuc.w - (nuc.largeOp ? nuc.ic : 0);
        Box out = nuc;
        out.b = nuc.b;
        qreal w = nuc.w;
        if (n.primes) {
            Box P = glyphBox(fontVariant(RoleMath), primeString(n.primes), px);
            move(P, supX, 0);
            supX += P.w;
            w = qMax(w, supX);
            out.a = qMax(out.a, P.a);
        }
        Box sp, sb;
        if (hasSup)
            sp = node(n.sup, supStyle(st));
        if (hasSub)
            sb = node(n.sub, subStyle(st));
        if (ctx_.failed)
            return out;
        qreal u = 0, v = 0;
        if (hasSup) {
            u = st.cramped ? C(SuperscriptShiftUpCramped, st) : C(SuperscriptShiftUp, st);
            if (!nuc.isChar)
                u = qMax(u, nuc.a - C(SuperscriptBaselineDropMax, st));
            u = qMax(u, sp.d + C(SuperscriptBottomMin, st));
        }
        if (hasSub) {
            v = C(SubscriptShiftDown, st);
            if (!nuc.isChar)
                v = qMax(v, nuc.d + C(SubscriptBaselineDropMin, st));
            if (!hasSup)
                v = qMax(v, sb.a - C(SubscriptTopMax, st));
        }
        if (hasSup && hasSub) {
            const qreal gap = (u - sp.d) - (sb.a - v);
            const qreal gmin = C(SubSuperscriptGapMin, st);
            if (gap < gmin) {
                qreal delta = gmin - gap;
                const qreal supBottomMax = C(SuperscriptBottomMaxWithSubscript, st) - (u - sp.d);
                if (supBottomMax > 0) {
                    const qreal up = qMin(delta, supBottomMax);
                    u += up;
                    delta -= up;
                }
                v += delta;
            }
        }
        if (hasSup) {
            move(sp, supX, -u);
            w = qMax(w, supX + sp.w);
            out.a = qMax(out.a, u + sp.a);
            out.d = qMax(out.d, sp.d - u);
        }
        if (hasSub) {
            move(sb, subX, v);
            w = qMax(w, subX + sb.w);
            out.d = qMax(out.d, v + sb.d);
            out.a = qMax(out.a, sb.a - v);
        }
        if (hasSup || hasSub)
            w += C(SpaceAfterScript, st);
        finish(out);
        out.w = w;
        out.ic = 0;
        out.isChar = false;
        out.largeOp = false;
        out.accentX = qQNaN();
        return out;
    }

    // Stack over/under boxes centred on `nuc` (limits, \overset, braces).
    Box stackLimits(Box nuc, Box *over, Box *under, MStyle st)
    {
        const qreal ic = nuc.largeOp ? nuc.ic : 0;
        qreal minX = -nuc.w / 2, maxX = nuc.w / 2;
        qreal overX = 0, underX = 0;
        if (over) {
            overX = -over->w / 2 + ic / 2;
            minX = qMin(minX, overX);
            maxX = qMax(maxX, overX + over->w);
        }
        if (under) {
            underX = -under->w / 2 - ic / 2;
            minX = qMin(minX, underX);
            maxX = qMax(maxX, underX + under->w);
        }
        Box out = nuc;
        const qreal shift = -minX;
        move(nuc, shift - nuc.w / 2, 0);
        if (over) {
            const qreal rise = qMax(C(UpperLimitBaselineRiseMin, st), C(UpperLimitGapMin, st) + over->d);
            move(*over, shift + overX, -(nuc.a + rise));
            out.a = nuc.a + rise + over->a;
        }
        if (under) {
            const qreal drop = qMax(C(LowerLimitBaselineDropMin, st), C(LowerLimitGapMin, st) + under->a);
            move(*under, shift + underX, nuc.d + drop);
            out.d = nuc.d + drop + under->d;
        }
        finish(out);
        out.w = maxX - minX;
        out.ic = 0;
        out.isChar = false;
        out.largeOp = false;
        out.accentX = qQNaN();
        return out;
    }

    Box limitsBox(Box nuc, const Node &n, MStyle st)
    {
        Box over, under;
        if (n.sup >= 0)
            over = node(n.sup, supStyle(st));
        if (n.sub >= 0)
            under = node(n.sub, subStyle(st));
        if (ctx_.failed)
            return nuc;
        return stackLimits(nuc, n.sup >= 0 ? &over : nullptr, n.sub >= 0 ? &under : nullptr, st);
    }

    Box op(const Node &n, MStyle st)
    {
        const qreal px = size(st);
        if (n.cp) {
            Box b;
            const bool display = st.lvl == 0;
            const GlyphConstruction *gc = fd_.hasMathTable ? fd_.verticalFor(n.cp) : nullptr;
            if (display && gc && !gc->variants.empty()) {
                const GlyphVariant *pick = &gc->variants.back();
                const qreal minH = fd_.displayOperatorMinHeight * px;
                for (const GlyphVariant &v : gc->variants) {
                    const GlyphShape *sh = fd_.shape(v.gid);
                    if (sh && sh->ink.height() * px >= minH) {
                        pick = &v;
                        break;
                    }
                }
                const GlyphShape *sh = fd_.shape(pick->gid);
                if (sh) {
                    b = shapeBox(*sh, px);
                    b.ic = sh->italic * px;
                } else {
                    b = glyphBox(fontVariant(RoleMath), str(n.cp), px);
                }
            } else {
                // No display-size variant available (no MATH table, or the
                // table has no construction for this operator): scale up.
                const qreal opPx = display ? qreal(qRound(px * 1.4)) : px;
                b = glyphBox(fontVariant(RoleMath), str(n.cp), opPx);
                if (fd_.hasMathTable)
                    b.ic = fd_.italicCorrection(n.cp) * px;
                else
                    b.ic = qMax<qreal>(0, items_[size_t(b.b)].ink.right() - b.w);
            }
            // Normalise to the OpenType convention (advance covers the ink,
            // the italic correction tucks the subscript in). Fonts following
            // the TeX convention with no italic data (STIX 1.1 "STIX Math")
            // have ink overhanging a narrow advance: widen the box and use the
            // overhang as italic correction so the superscript clears the top.
            qreal it = 0, ib = 0;
            inkY(b, it, ib);
            qreal inkRight = b.w;
            for (int i = b.b; i < b.e; ++i)
                inkRight = qMax(inkRight, items_[size_t(i)].x + items_[size_t(i)].ink.right());
            if (inkRight > b.w + 0.02 * px) {
                b.ic = qMax(b.ic, inkRight - b.w);
                b.w = inkRight;
            }
            centerOnAxis(b, st);
            b.isChar = false;
            b.largeOp = true;
            return b;
        }
        Box b = n.a >= 0 ? node(n.a, st) : glyphBox(n.variant, n.text, px);
        b.isChar = false;
        b.largeOp = false;
        b.ic = 0;
        b.accentX = qQNaN();
        return b;
    }

    // --------------------------------------------------------- stretchy --
    Box nullDelimiter(MStyle st)
    {
        Box b = start();
        b.w = 0.12 * size(st);
        return b;
    }

    Box assemblyV(const GlyphConstruction &gc, qreal target, qreal px)
    {
        const qreal ov = fd_.minConnectorOverlap * px;
        qreal fixed = 0;
        int nExt = 0;
        for (const GlyphPart &p : gc.parts) {
            if (p.extender)
                ++nExt;
            else
                fixed += p.fullAdvance * px;
        }
        qreal L = 0;
        if (nExt > 0)
            L = qMax((target - fixed) / nExt + 2 * ov, 2 * ov);
        Box b = start();
        qreal pos = 0, width = 0;
        for (const GlyphPart &p : gc.parts) {
            const GlyphShape *sh = fd_.shape(p.gid);
            if (!sh)
                continue;
            if (!p.extender) {
                const qreal dy = -pos - sh->ink.bottom() * px;
                shapeBox(*sh, px, px, dy);
                pos += p.fullAdvance * px;
            } else {
                const qreal s = pos - ov, e = s + L;
                const qreal ih = sh->ink.height();
                if (ih > 0) {
                    const qreal sy = (e - s) / ih;
                    shapeBox(*sh, px, sy, -e - sh->ink.top() * sy);
                }
                pos = e - ov;
            }
            width = qMax(width, sh->adv * px);
        }
        finish(b);
        b.w = width;
        b.a = pos;
        b.d = 0;
        // Use real ink extents (parts may overshoot their advances).
        for (int i = b.b; i < b.e; ++i) {
            const QRectF r = items_[size_t(i)].ink.translated(items_[size_t(i)].x, items_[size_t(i)].y);
            b.a = qMax(b.a, -r.top());
            b.d = qMax(b.d, r.bottom());
        }
        return b;
    }

    Box assemblyH(const GlyphConstruction &gc, qreal target, qreal px)
    {
        const qreal ov = fd_.minConnectorOverlap * px;
        qreal fixed = 0;
        int nExt = 0;
        for (const GlyphPart &p : gc.parts) {
            if (p.extender)
                ++nExt;
            else
                fixed += p.fullAdvance * px;
        }
        qreal L = 0;
        if (nExt > 0)
            L = qMax((target - fixed) / nExt + 2 * ov, 2 * ov);
        Box b = start();
        qreal pos = 0;
        for (const GlyphPart &p : gc.parts) {
            const GlyphShape *sh = fd_.shape(p.gid);
            if (!sh)
                continue;
            if (!p.extender) {
                pathBox(sh->path, sh->ink, 0, px, px, pos - sh->ink.left() * px, 0);
                pos += p.fullAdvance * px;
            } else {
                const qreal s = pos - ov, e = s + L;
                const qreal iw = sh->ink.width();
                if (iw > 0) {
                    const qreal sx = (e - s) / iw;
                    pathBox(sh->path, sh->ink, 0, sx, px, s - sh->ink.left() * sx, 0);
                }
                pos = e - ov;
            }
        }
        finish(b);
        b.w = pos;
        for (int i = b.b; i < b.e; ++i) {
            const QRectF r = items_[size_t(i)].ink.translated(items_[size_t(i)].x, items_[size_t(i)].y);
            b.a = qMax(b.a, -r.top());
            b.d = qMax(b.d, r.bottom());
        }
        return b;
    }

    // Vertical delimiter of at least `target` total height (ink).
    Box delimiter(char32_t cp, qreal target, MStyle st, bool center)
    {
        if (cp == 0)
            return nullDelimiter(st);
        const qreal px = size(st);
        const QString s = str(cp);
        const GlyphMetrics m = measure(fd_, fontVariant(RoleMath), s, &ctx_.coldNanos);
        const qreal h0 = (m.y1 - m.y0) * px;
        Box b;
        bool done = false;
        if (h0 >= target - 0.01 * px && m.inFont) {
            b = glyphBox(fontVariant(RoleMath), s, px);
            done = true;
        }
        const GlyphConstruction *gc = (!done && fd_.hasMathTable) ? fd_.verticalFor(cp) : nullptr;
        if (!done && gc) {
            for (const GlyphVariant &v : gc->variants) {
                const GlyphShape *sh = fd_.shape(v.gid);
                if (sh && sh->ink.height() * px >= target) {
                    b = shapeBox(*sh, px);
                    done = true;
                    break;
                }
            }
            if (!done && !gc->parts.empty()) {
                b = assemblyV(*gc, target, px);
                done = true;
            }
            if (!done && !gc->variants.empty()) {
                const GlyphShape *sh = fd_.shape(gc->variants.back().gid);
                if (sh) {
                    b = shapeBox(*sh, px);
                    done = true;
                }
            }
        }
        if (!done) {
            qreal adv = 0;
            QPainterPath p = syntheticVertical(cp, target, px, &adv);
            if (!p.isEmpty()) {
                b = rawPathBox(p, adv);
                done = true;
            } else {
                // Unknown shape: scale the glyph (bounded).
                const qreal scale = h0 > 0 ? qBound<qreal>(1.0, target / h0, 4.0) : 1.0;
                b = glyphBox(fontVariant(RoleMath), s, qreal(qMax(1, qRound(px * scale))));
                done = true;
            }
        }
        if (center)
            centerOnAxis(b, st);
        return b;
    }

    // Horizontal stretchy glyph (wide accents, braces, arrows) >= width W.
    Box hstretch(char32_t cp, qreal W, MStyle st)
    {
        const qreal px = size(st);
        const GlyphConstruction *gc = fd_.hasMathTable ? fd_.horizontalFor(cp) : nullptr;
        if (gc) {
            for (const GlyphVariant &v : gc->variants) {
                const GlyphShape *sh = fd_.shape(v.gid);
                if (sh && sh->ink.width() * px >= W) {
                    Box b = pathBox(sh->path, sh->ink, sh->ink.width() * px, px, px,
                                    -sh->ink.left() * px, 0);
                    return b;
                }
            }
            if (!gc->parts.empty())
                return assemblyH(*gc, W, px);
        }
        QPainterPath syn = syntheticHorizontal(cp, W, px);
        if (!syn.isEmpty())
            return rawPathBox(syn, W);
        const GlyphShape *sh = gc && !gc->variants.empty() ? fd_.shape(gc->variants.back().gid)
                                                           : fd_.shapeForChar(cp);
        if (sh && sh->ink.width() > 0) {
            const qreal sx = qMax(px, W / sh->ink.width());
            return pathBox(sh->path, sh->ink, sh->ink.width() * sx, sx, px, -sh->ink.left() * sx, 0);
        }
        return glyphBox(fontVariant(RoleMath), str(spacingAccent(cp)), px);
    }

    // \left ... \middle ... \right (also matrix delimiters).
    Box delimited(std::vector<Box> &segs, const std::vector<char32_t> &delims, MStyle st,
                  bool explicitNull)
    {
        qreal a = 0, d = 0;
        for (const Box &s : segs) {
            a = qMax(a, s.a);
            d = qMax(d, s.d);
        }
        const qreal ax = axis(st);
        const qreal delta = qMax(a - ax, d + ax);
        const qreal target = qMax(2 * delta * 0.901, 2 * delta - 0.5 * size(st));
        Box out = segs.empty() ? start() : segs.front();
        if (!segs.empty())
            out.b = segs.front().b;
        std::vector<Box> dl;
        for (char32_t c : delims) {
            if (c == 0 && !explicitNull) {
                dl.push_back(start());
                continue;
            }
            dl.push_back(delimiter(c, target, st, true));
        }
        qreal x = 0;
        out.a = a;
        out.d = d;
        for (size_t i = 0; i < dl.size(); ++i) {
            move(dl[i], x, 0);
            x += dl[i].w;
            out.a = qMax(out.a, dl[i].a);
            out.d = qMax(out.d, dl[i].d);
            if (i < segs.size()) {
                move(segs[i], x, 0);
                x += segs[i].w;
            }
        }
        finish(out);
        out.w = x;
        out.ic = 0;
        out.isChar = false;
        out.largeOp = false;
        out.accentX = qQNaN();
        return out;
    }

    Box leftRight(const Node &n, MStyle st)
    {
        const LeftRightData &lr = ast_.lrs[size_t(n.list)];
        std::vector<Box> segs;
        segs.reserve(lr.segments.size());
        const Box first = start();
        for (int s : lr.segments) {
            segs.push_back(node(s, st));
            if (ctx_.failed)
                return first;
        }
        return delimited(segs, lr.delims, st, true);
    }

    // ------------------------------------------------------ constructs --
    Box frac(const Node &n, MStyle st)
    {
        MStyle s = st;
        if (n.style >= 0)
            s = MStyle{quint8(n.style), st.cramped};
        const bool disp = s.lvl == 0;
        Box num = node(n.a, numStyle(s));
        Box den = node(n.b, denStyle(s));
        if (ctx_.failed)
            return num;
        const qreal px = size(s);
        const qreal ax = axis(s);
        const bool rule = !(n.flags & kFracNoRule);
        const qreal t = rule ? C(FractionRuleThickness, s) : 0;
        qreal u, v;
        if (rule) {
            u = disp ? C(FractionNumeratorDisplayStyleShiftUp, s) : C(FractionNumeratorShiftUp, s);
            v = disp ? C(FractionDenominatorDisplayStyleShiftDown, s) : C(FractionDenominatorShiftDown, s);
            const qreal gn = disp ? C(FractionNumDisplayStyleGapMin, s) : C(FractionNumeratorGapMin, s);
            const qreal gd = disp ? C(FractionDenomDisplayStyleGapMin, s) : C(FractionDenominatorGapMin, s);
            u = qMax(u, ax + t / 2 + gn + num.d);
            v = qMax(v, gd + t / 2 - ax + den.a);
        } else {
            u = disp ? C(StackTopDisplayStyleShiftUp, s) : C(StackTopShiftUp, s);
            v = disp ? C(StackBottomDisplayStyleShiftDown, s) : C(StackBottomShiftDown, s);
            const qreal gap = disp ? C(StackDisplayStyleGapMin, s) : C(StackGapMin, s);
            const qreal actual = (u - num.d) - (den.a - v);
            if (actual < gap) {
                u += (gap - actual) / 2;
                v += (gap - actual) / 2;
            }
        }
        const qreal pad = (n.flags & kFracParens) ? 0 : 0.12 * px;
        const qreal W = qMax(num.w, den.w) + 2 * pad;
        Box out = num;
        move(num, (W - num.w) / 2, -u);
        move(den, (W - den.w) / 2, v);
        if (rule)
            addRule(pad, -ax - t / 2, W - 2 * pad, t);
        finish(out);
        out.w = W;
        out.a = qMax(u + num.a, ax + t / 2);
        out.d = qMax(v + den.d, t / 2 - ax);
        out.ic = 0;
        out.isChar = false;
        out.largeOp = false;
        out.accentX = qQNaN();
        if (n.flags & kFracParens) {
            std::vector<Box> segs{out};
            const std::vector<char32_t> dl{'(', ')'};
            return delimited(segs, dl, s, false);
        }
        return out;
    }

    Box radical(const Node &n, MStyle st)
    {
        const qreal px = size(st);
        Box body = node(n.a, cramp(st));
        if (ctx_.failed)
            return body;
        const qreal t = C(RadicalRuleThickness, st);
        qreal gap = st.lvl == 0 ? C(RadicalDisplayStyleVerticalGap, st) : C(RadicalVerticalGap, st);
        const qreal target = body.a + body.d + gap + t;
        Box rad = delimiter(0x221A, target, st, false);
        const qreal rh = rad.a + rad.d;
        if (rh > target)
            gap += (rh - target) / 2;
        const qreal top = -(body.a + gap + t); // y of the rule's top edge
        const qreal dy = top + rad.a;           // move radical so its ink top == top
        move(rad, 0, dy);
        const qreal radW = rad.w;
        // Body after the radical sign, then the vinculum over it.
        move(body, radW, 0);
        Box out = body;
        out.b = body.b;
        addRule(radW - 0.02 * px, top, body.w + 0.02 * px, t);
        finish(out);
        out.w = radW + body.w;
        out.a = body.a + gap + t + C(RadicalExtraAscender, st);
        out.d = qMax(body.d, rad.d + dy);
        if (n.b >= 0) {
            Box idx = node(n.b, MStyle{3, true});
            if (ctx_.failed)
                return out;
            const qreal before = C(RadicalKernBeforeDegree, st);
            const qreal after = C(RadicalKernAfterDegree, st);
            const qreal bottom = rad.d + dy; // radical ink bottom (y)
            const qreal raise = fd_.radicalDegreeBottomRaise * (rad.a + rad.d);
            const qreal idxY = bottom - raise;
            const qreal shift = qMax<qreal>(0, before + idx.w + after);
            move(idx, before, idxY);
            // shift radical+body right to make room for the index
            for (int i = out.b; i < idx.b; ++i) {
                items_[size_t(i)].x += shift;
            }
            out.e = int(items_.size());
            out.w += shift;
            out.w = qMax(out.w, before + idx.w);
            out.a = qMax(out.a, idx.a - idxY);
            out.d = qMax(out.d, idx.d + idxY);
        }
        out.ic = 0;
        out.isChar = false;
        out.largeOp = false;
        out.accentX = qQNaN();
        return out;
    }

    Box accent(const Node &n, MStyle st)
    {
        const qreal px = size(st);
        Box body = node(n.a, cramp(st));
        if (ctx_.failed)
            return body;
        const bool wide = n.flags & kAccentWide;
        const qreal baseAttach = std::isnan(body.accentX) ? (body.w + body.ic) / 2 : body.accentX;
        const bool combining = isCombiningAccent(n.cp);
        Box acc;
        qreal attach = 0;
        bool spacingFallback = false;
        const GlyphShape *sh = fd_.shapeForChar(n.cp);
        if (wide) {
            // hstretch() boxes start their ink at x=0 and span acc.w
            acc = hstretch(n.cp, body.w, st);
            attach = acc.w / 2;
        } else if (sh && combining) {
            acc = shapeBox(*sh, px);
            attach = std::isnan(sh->topAccent) ? (sh->ink.left() + sh->ink.right()) / 2 * px
                                               : sh->topAccent * px;
        } else {
            // No outline for the combining mark: use the spacing form (arrows
            // at reduced size) and place it by its ink just above the body.
            const char32_t sp = spacingAccent(n.cp);
            const bool arrow = sp == 0x2192 || sp == 0x2190 || sp == 0x2194;
            acc = glyphBox(fontVariant(RoleMath), str(sp), arrow ? qreal(qMax(1, qRound(px * 0.7))) : px);
            const QRectF &ink = items_[size_t(acc.b)].ink;
            attach = (ink.left() + ink.right()) / 2;
            spacingFallback = true;
        }
        const qreal accentBase = fd_.hasMathTable ? C(AccentBaseHeight, st) : fd_.xHeight * px;
        qreal dy;
        if (combining && !spacingFallback && (fd_.hasMathTable || !wide)) {
            // OpenType convention: the mark is designed for a base whose
            // height is AccentBaseHeight; raise it by the excess.
            dy = -qMax<qreal>(0, body.a - accentBase);
        } else {
            // Arrows, spacing fallbacks and synthetic accents: sit on top of
            // the body with a small gap.
            const qreal gap = 0.08 * px;
            qreal it = 0, ib = 0;
            inkY(acc, it, ib);
            dy = -(qMax(body.a, accentBase * 0.95) + gap) - ib;
        }
        move(acc, baseAttach - attach, dy);
        Box out = body;
        finish(out);
        qreal at = 0, ab = 0;
        inkY(acc, at, ab);
        out.a = qMax(body.a, -at);
        out.isChar = false;
        out.largeOp = false;
        out.accentX = qQNaN();
        out.ic = body.ic;
        return out;
    }

    Box overline(const Node &n, MStyle st)
    {
        Box body = node(n.a, cramp(st));
        if (ctx_.failed)
            return body;
        const qreal t = C(OverbarRuleThickness, st);
        const qreal gap = C(OverbarVerticalGap, st);
        Box out = body;
        addRule(0, -(body.a + gap + t), body.w, t);
        finish(out);
        out.a = body.a + gap + t + C(OverbarExtraAscender, st);
        out.isChar = false;
        out.accentX = qQNaN();
        out.ic = 0;
        return out;
    }

    Box underline(const Node &n, MStyle st)
    {
        Box body = node(n.a, st);
        if (ctx_.failed)
            return body;
        const qreal t = C(UnderbarRuleThickness, st);
        const qreal gap = C(UnderbarVerticalGap, st);
        Box out = body;
        addRule(0, body.d + gap, body.w, t);
        finish(out);
        out.d = body.d + gap + t + C(UnderbarExtraDescender, st);
        out.isChar = false;
        out.accentX = qQNaN();
        out.ic = 0;
        return out;
    }

    Box brace(const Node &n, MStyle st)
    {
        const qreal px = size(st);
        Box body = node(n.a, st);
        if (ctx_.failed)
            return body;
        const bool under = n.flags & kUnder;
        Box br = hstretch(n.cp, body.w, st);
        const qreal gap = qMax(C(StretchStackGapBelowMin, st), 0.1 * px);
        const qreal W = qMax(body.w, br.w);
        Box out = body;
        move(body, (W - body.w) / 2, 0);
        qreal it = 0, ib = 0;
        inkY(br, it, ib);
        if (!under)
            move(br, (W - br.w) / 2, -(body.a + gap) - ib);
        else
            move(br, (W - br.w) / 2, body.d + gap - it);
        finish(out);
        out.w = W;
        if (!under)
            out.a = body.a + gap + (ib - it);
        else
            out.d = body.d + gap + (ib - it);
        out.isChar = false;
        out.largeOp = false;
        out.ic = 0;
        out.accentX = qQNaN();
        return out;
    }

    Box stack(const Node &n, MStyle st)
    {
        Box base = node(n.a, st);
        Box over, under;
        if (n.b >= 0)
            over = node(n.b, supStyle(st));
        if (n.c >= 0)
            under = node(n.c, subStyle(st));
        if (ctx_.failed)
            return base;
        base.largeOp = false;
        return stackLimits(base, n.b >= 0 ? &over : nullptr, n.c >= 0 ? &under : nullptr, st);
    }

    Box phantom(const Node &n, MStyle st)
    {
        Box b = node(n.a, st);
        if (ctx_.failed)
            return b;
        if (n.flags & kSmash) {
            b.a = b.d = 0;
            return b;
        }
        items_.resize(size_t(b.b));
        b.e = b.b;
        if (!(n.flags & kPhantomH))
            b.w = 0;
        if (!(n.flags & kPhantomV))
            b.a = b.d = 0;
        b.isChar = false;
        b.accentX = qQNaN();
        return b;
    }

    Box boxed(const Node &n, MStyle st)
    {
        const qreal px = size(st);
        Box b = node(n.a, st);
        if (ctx_.failed)
            return b;
        const qreal pad = 0.3 * px, t = 0.05 * px;
        move(b, pad + t, 0);
        Box out = b;
        const qreal W = b.w + 2 * (pad + t);
        const qreal top = -(b.a + pad + t), bottom = b.d + pad + t;
        addRule(0, top, W, t);
        addRule(0, bottom - t, W, t);
        addRule(0, top, t, bottom - top);
        addRule(W - t, top, t, bottom - top);
        finish(out);
        out.w = W;
        out.a = -top;
        out.d = bottom;
        out.isChar = false;
        out.accentX = qQNaN();
        out.ic = 0;
        return out;
    }

    Box notBox(const Node &n, MStyle st)
    {
        const qreal px = size(st);
        Box b = node(n.a, st);
        if (ctx_.failed)
            return b;
        Box out = b;
        const QPainterPath slash = notSlash(b.w / 2, -axis(st), 0.45 * px, px);
        rawPathBox(slash, 0);
        finish(out);
        out.a = qMax(out.a, axis(st) + 0.45 * px);
        out.d = qMax(out.d, 0.45 * px - axis(st));
        out.isChar = false;
        out.accentX = qQNaN();
        return out;
    }

    Box env(const Node &n, MStyle st)
    {
        const EnvData &e = ast_.envs[size_t(n.list)];
        MStyle cs{1, false};
        qreal stretch = 1.0, colGap = 1.0, outerPad = 0, jot = 0;
        switch (e.kind) {
        case EnvKind::Aligned:
        case EnvKind::Gathered:
            cs = MStyle{0, false};
            jot = 0.3;
            break;
        case EnvKind::SmallMatrix:
            cs = MStyle{2, false};
            colGap = 0.4;
            stretch = 0.75;
            break;
        case EnvKind::Cases:
        case EnvKind::RCases:
            stretch = 1.2;
            break;
        case EnvKind::DCases:
            cs = MStyle{0, false};
            stretch = 1.2;
            break;
        case EnvKind::Array:
            outerPad = 0.5;
            break;
        default:
            break;
        }
        const qreal em = size(cs);
        const size_t rows = e.rows.size();
        size_t cols = e.align.size();
        for (const auto &r : e.rows)
            cols = qMax(cols, r.size());
        if (cols == 0)
            cols = 1;
        std::vector<Box> cells(rows * cols, start());
        std::vector<bool> present(rows * cols, false);
        const Box first = start();
        for (size_t r = 0; r < rows; ++r) {
            for (size_t c = 0; c < e.rows[r].size(); ++c) {
                const Node &cell = ast_.nodes[size_t(e.rows[r][c])];
                const Cls startCls = (e.kind == EnvKind::Aligned && c % 2 == 1) ? Cls::Ord : Cls::None;
                Box b = cell.kind == NK::List ? hlist(ast_.lists[size_t(cell.list)], cs, startCls)
                                              : node(e.rows[r][c], cs);
                if (ctx_.failed)
                    return first;
                cells[r * cols + c] = b;
                present[r * cols + c] = true;
            }
        }
        auto alignOf = [&](size_t c) -> char {
            switch (e.kind) {
            case EnvKind::Cases: case EnvKind::DCases: case EnvKind::RCases: return 'l';
            case EnvKind::Aligned: return c % 2 == 0 ? 'r' : 'l';
            case EnvKind::Array: return c < e.align.size() ? e.align[c] : 'c';
            default: return 'c';
            }
        };
        auto gapAfter = [&](size_t c) -> qreal {
            if (e.kind == EnvKind::Aligned)
                return c % 2 == 0 ? 0 : 1.0;
            return colGap;
        };
        std::vector<qreal> colW(cols, 0), rowA(rows, 0), rowD(rows, 0), colX(cols, 0), baseY(rows, 0);
        const qreal strutA = 0.84 * stretch * em, strutD = 0.36 * stretch * em;
        for (size_t r = 0; r < rows; ++r) {
            rowA[r] = strutA;
            rowD[r] = strutD;
            for (size_t c = 0; c < cols; ++c) {
                const Box &b = cells[r * cols + c];
                colW[c] = qMax(colW[c], b.w);
                rowA[r] = qMax(rowA[r], b.a);
                rowD[r] = qMax(rowD[r], b.d);
            }
        }
        qreal x = outerPad * em;
        for (size_t c = 0; c < cols; ++c) {
            colX[c] = x;
            x += colW[c];
            if (c + 1 < cols)
                x += gapAfter(c) * em;
        }
        const qreal W = x + outerPad * em;
        qreal H = 0;
        for (size_t r = 0; r < rows; ++r) {
            H += rowA[r] + rowD[r];
            if (r + 1 < rows)
                H += jot * em + qMax<qreal>(-rowD[r], e.rowGapEm[r] * em);
        }
        const qreal ax = axis(st);
        const qreal top = -(H / 2 + ax);
        qreal y = top;
        for (size_t r = 0; r < rows; ++r) {
            y += rowA[r];
            baseY[r] = y;
            y += rowD[r];
            if (r + 1 < rows)
                y += jot * em + qMax<qreal>(-rowD[r], e.rowGapEm[r] * em);
        }
        for (size_t r = 0; r < rows; ++r) {
            for (size_t c = 0; c < cols; ++c) {
                if (!present[r * cols + c])
                    continue;
                const Box &b = cells[r * cols + c];
                const char al = alignOf(c);
                const qreal xo = colX[c] + (al == 'l' ? 0 : al == 'r' ? colW[c] - b.w : (colW[c] - b.w) / 2);
                move(b, xo, baseY[r]);
            }
        }
        if (e.kind == EnvKind::Array) {
            const qreal rt = 0.04 * em, sep = 0.2 * em;
            for (size_t j = 0; j < e.vlines.size() && j <= cols; ++j) {
                const int count = e.vlines[j];
                if (!count)
                    continue;
                qreal cx;
                if (j == 0)
                    cx = 0;
                else if (j >= cols)
                    cx = W;
                else
                    cx = colX[j] - gapAfter(j - 1) * em / 2;
                for (int k = 0; k < count; ++k) {
                    const qreal off = (k - (count - 1) / 2.0) * sep;
                    addRule(qBound<qreal>(0, cx + off - rt / 2, W - rt), top, rt, H);
                }
            }
            for (size_t r = 0; r < e.hlines.size() && r <= rows; ++r) {
                const int count = e.hlines[r];
                if (!count)
                    continue;
                qreal cy;
                if (r == 0)
                    cy = top;
                else if (r >= rows)
                    cy = top + H;
                else
                    cy = ((baseY[r - 1] + rowD[r - 1]) + (baseY[r] - rowA[r])) / 2;
                for (int k = 0; k < count; ++k) {
                    const qreal off = (k - (count - 1) / 2.0) * sep;
                    addRule(0, qBound(top, cy + off - rt / 2, top + H - rt), W, rt);
                }
            }
        }
        Box out = first;
        finish(out);
        out.w = W;
        out.a = H / 2 + ax;
        out.d = H / 2 - ax;
        char32_t L = 0, R = 0;
        switch (e.kind) {
        case EnvKind::PMatrix: L = '('; R = ')'; break;
        case EnvKind::BMatrix: L = '['; R = ']'; break;
        case EnvKind::BBMatrix: L = '{'; R = '}'; break;
        case EnvKind::VMatrix: L = '|'; R = '|'; break;
        case EnvKind::VVMatrix: L = 0x2016; R = 0x2016; break;
        case EnvKind::Cases: case EnvKind::DCases: L = '{'; break;
        case EnvKind::RCases: R = '}'; break;
        default: break;
        }
        if (L || R) {
            std::vector<Box> segs{out};
            const std::vector<char32_t> dl{L, R};
            return delimited(segs, dl, st, true);
        }
        return out;
    }
};

void Layouter::flatten(const Box &root, MathDisplayList &out)
{
    qreal minX = 0, maxX = root.w, minY = -root.a, maxY = root.d;
    for (int i = root.b; i < root.e; ++i) {
        const Item &it = items_[size_t(i)];
        const QRectF r = it.ink.translated(it.x, it.y);
        if (r.width() <= 0 && r.height() <= 0)
            continue;
        minX = qMin(minX, r.left());
        maxX = qMax(maxX, r.right());
        minY = qMin(minY, r.top());
        maxY = qMax(maxY, r.bottom());
    }
    const qreal sx = minX < 0 ? -minX : 0;
    out.width = maxX + sx;
    out.ascent = qMax<qreal>(0, -minY);
    out.descent = qMax<qreal>(0, maxY);

    std::vector<int> fontMap(fonts_.size(), -1);
    int lastRun = -1;
    qreal lastEnd = 0;
    int lastFont = -1;
    qreal lastY = 0;
    for (int i = root.b; i < root.e; ++i) {
        const Item &it = items_[size_t(i)];
        switch (it.t) {
        case Item::Glyph: {
            if (fontMap[size_t(it.font)] < 0) {
                fontMap[size_t(it.font)] = int(out.fonts.size());
                out.fonts.push_back(fonts_[size_t(it.font)]);
            }
            const int f = fontMap[size_t(it.font)];
            const QString &s = strs_[size_t(it.ref)];
            if (lastRun >= 0 && f == lastFont && std::fabs(it.y - lastY) < 1e-6
                && std::fabs(it.x + sx - lastEnd) < 1e-3) {
                out.runs[lastRun].text += s;
            } else {
                MathDisplayList::GlyphRun run;
                run.pos = QPointF(it.x + sx, it.y);
                run.font = f;
                run.text = s;
                out.runs.push_back(run);
                lastRun = int(out.runs.size()) - 1;
                lastFont = f;
                lastY = it.y;
            }
            lastEnd = it.x + sx + it.w;
            break;
        }
        case Item::Rule:
            out.rules.push_back({QRectF(it.x + sx, it.y, it.w, it.h)});
            lastRun = -1;
            break;
        case Item::Path: {
            QPainterPath p = paths_[size_t(it.ref)].translated(it.x + sx, it.y);
            p.setFillRule(Qt::WindingFill);
            out.paths.push_back(std::move(p));
            lastRun = -1;
            break;
        }
        }
    }
    out.runs.squeeze();
    out.rules.squeeze();
    out.paths.squeeze();
    out.fonts.squeeze();
}

} // namespace

bool layout(const FontData &fd, const Ast &ast, Ctx &ctx, qreal basePx, bool display,
            MathDisplayList &out)
{
    Layouter L(fd, ast, ctx, basePx);
    const Box root = L.root(display);
    if (ctx.failed || !ctx.check())
        return false;
    L.flatten(root, out);
    const qreal limit = kMaxExtentEm * basePx;
    if (!(out.width < limit && out.height() < limit))
        return ctx.fail(QStringLiteral("Formula too large to display"), -1);
    return true;
}

} // namespace detail
} // namespace math
} // namespace folia
