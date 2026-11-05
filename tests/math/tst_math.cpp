// math_tests: constructs, geometry, errors, pathological input, cancellation,
// thread-safety and a small perf report for folia::math::render().
#include "MathCorpus.h"
#include "math/MathFont.h" // internal: first-load breakdown only
#include "math/MathRenderer.h"

#include <QElapsedTimer>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QImage>
#include <QMutex>
#include <QPainter>
#include <QPainterPath>
#include <QThread>
#include <QThreadPool>
#include <QtTest>

#include <algorithm>
#include <functional>
#include <atomic>
#include <cstdio>
#include <vector>

using namespace folia;

namespace {

constexpr qreal kPt = 20; // em in px on macOS (72 dpi logical)

math::Result R(const QString &tex, bool display = true, qreal pt = kPt, const math::Limits &lim = {})
{
    math::Style st;
    st.pointSize = pt;
    st.display = display;
    return math::render(tex, st, lim);
}

QString ucs(char32_t c) { return QString::fromUcs4(&c, 1); }

bool hasMathAlnum()
{
    const math::detail::FontData *fd = math::detail::fontData();
    return fd && fd->hasMathAlphanumerics;
}

// What the renderer emits for a math letter: the Unicode math italic form
// when the math font has those glyphs, else the plain letter (drawn with an
// italic QFont).
QString mi(char c)
{
    if (!hasMathAlnum())
        return QString(QLatin1Char(c));
    if (c == 'h')
        return ucs(0x210E);
    if (c >= 'a' && c <= 'z')
        return ucs(0x1D44E + (c - 'a'));
    return ucs(0x1D434 + (c - 'A'));
}

const MathDisplayList::GlyphRun *findRun(const MathDisplayList &l, const QString &text, int nth = 0)
{
    for (const auto &r : l.runs) {
        if (r.text == text && nth-- == 0)
            return &r;
    }
    return nullptr;
}
std::vector<const MathDisplayList::GlyphRun *> runsContaining(const MathDisplayList &l, const QString &t)
{
    std::vector<const MathDisplayList::GlyphRun *> out;
    for (const auto &r : l.runs)
        if (r.text.contains(t))
            out.push_back(&r);
    return out;
}

qreal runWidth(const MathDisplayList &l, const MathDisplayList::GlyphRun &r)
{
    return QFontMetricsF(l.fonts[r.font]).horizontalAdvance(r.text);
}
// Exact ink of a run: the outline Qt draws for it (QPainterPath::addText).
// Engine glyph boxes are not exact: CoreText's tightBoundingRect() pads by
// up to ~1px and FreeType rounds boxes to whole pixels.
QRectF runInk(const MathDisplayList &l, const MathDisplayList::GlyphRun &r)
{
    QPainterPath p;
    p.addText(r.pos, l.fonts[r.font], r.text);
    return p.boundingRect();
}

// Paint the list (baseline at a fractional position on purpose) and return
// how many whole pixels with visible coverage (>= 12.5%, gray < 224) lie
// outside the allowed region around the reported box (<= 0 means none).
struct Overshoot {
    qreal left = 0, right = 0, top = 0, bottom = 0;
    qreal max() const { return qMax(qMax(left, right), qMax(top, bottom)); }
};
Overshoot paintedOvershoot(const MathDisplayList &l)
{
    const int pad = 8;
    const qreal ox = pad + 0.37, oy = pad + l.ascent + 0.29;
    const int w = qCeil(l.width) + 2 * pad, h = qCeil(l.height()) + 2 * pad;
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    QPainter p(&img);
    l.paint(&p, QPointF(ox, oy), Qt::black);
    p.end();
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            if (qGray(line[x]) < 224) {
                x0 = qMin(x0, x);
                x1 = qMax(x1, x);
                y0 = qMin(y0, y);
                y1 = qMax(y1, y);
            }
        }
    }
    // Allowed pixels: the exact box grown by 0.5px (Qt snaps text baselines
    // and glyph positions to the pixel grid, moving ink by <= 0.5px), then
    // rounded out to whole pixels (the antialiased pixel straddling an edge).
    // Overshoot > 0 means a visibly covered pixel outside that region.
    Overshoot o;
    if (x1 < 0)
        return o;
    o.left = qFloor(ox - 0.5) - x0;
    o.right = (x1 + 1) - qCeil(ox + l.width + 0.5);
    o.top = qFloor(oy - l.ascent - 0.5) - y0;
    o.bottom = (y1 + 1) - qCeil(oy + l.descent + 0.5);
    return o;
}

qreal runCenterX(const MathDisplayList &l, const MathDisplayList::GlyphRun &r)
{
    return r.pos.x() + runWidth(l, r) / 2;
}

QRectF inkBounds(const MathDisplayList &l)
{
    QRectF b;
    for (const auto &r : l.runs)
        b = b.united(runInk(l, r));
    for (const auto &r : l.rules)
        b = b.united(r.rect);
    for (const auto &p : l.paths)
        b = b.united(p.boundingRect());
    return b;
}

bool sameList(const MathDisplayList &a, const MathDisplayList &b)
{
    if (a.width != b.width || a.ascent != b.ascent || a.descent != b.descent)
        return false;
    if (a.fonts.size() != b.fonts.size() || a.runs.size() != b.runs.size()
        || a.rules.size() != b.rules.size() || a.paths.size() != b.paths.size())
        return false;
    for (int i = 0; i < a.fonts.size(); ++i) {
        const QFont &x = a.fonts[i], &y = b.fonts[i];
        if (x.families() != y.families() || x.pixelSize() != y.pixelSize() || x.pointSizeF() != y.pointSizeF() || x.italic() != y.italic()
            || x.weight() != y.weight())
            return false;
    }
    for (int i = 0; i < a.runs.size(); ++i) {
        if (a.runs[i].pos != b.runs[i].pos || a.runs[i].font != b.runs[i].font
            || a.runs[i].text != b.runs[i].text)
            return false;
    }
    for (int i = 0; i < a.rules.size(); ++i)
        if (a.rules[i].rect != b.rules[i].rect)
            return false;
    for (int i = 0; i < a.paths.size(); ++i) {
        const QPainterPath &x = a.paths[i], &y = b.paths[i];
        if (x.elementCount() != y.elementCount())
            return false;
        for (int k = 0; k < x.elementCount(); ++k) {
            const auto ex = x.elementAt(k), ey = y.elementAt(k);
            if (ex.x != ey.x || ex.y != ey.y || ex.type != ey.type)
                return false;
        }
    }
    return true;
}

qreal percentile(std::vector<double> v, double p)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    const double idx = p * double(v.size() - 1);
    const size_t lo = size_t(idx), hi = qMin(lo + 1, v.size() - 1);
    return v[lo] + (v[hi] - v[lo]) * (idx - double(lo));
}

qreal g_em = kPt; // renderer em in px for kPt (QtTest forces 96 dpi)

// Position of the nth occurrence of `needle` inside the (possibly merged) runs.
struct G {
    qreal x = 0, y = 0, w = 0;
    QRectF ink;
    int px = 0;
    bool ok = false;
    explicit operator bool() const { return ok; }
    qreal cx() const { return x + w / 2; }
};
G glyph(const MathDisplayList &l, const QString &needle, int nth = 0)
{
    for (const auto &r : l.runs) {
        int from = 0, idx;
        while ((idx = int(r.text.indexOf(needle, from))) >= 0) {
            if (nth-- == 0) {
                const QFontMetricsF fm(l.fonts[r.font]);
                G g;
                g.x = r.pos.x() + fm.horizontalAdvance(r.text.left(idx));
                g.y = r.pos.y();
                g.w = fm.horizontalAdvance(needle);
                MathDisplayList::GlyphRun one;
                one.pos = QPointF(g.x, g.y);
                one.font = r.font;
                one.text = needle;
                g.ink = runInk(l, one);
                g.px = l.fonts[r.font].pixelSize();
                g.ok = true;
                return g;
            }
            from = idx + int(needle.size());
        }
    }
    return G{};
}

qint64 g_fontDbMicros = -1;
qint64 g_resolveMicros = -1;
qint64 g_firstRenderMicros = -1;
qint64 g_secondRenderMicros = -1;

} // namespace

class TestMath : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void constructs_data();
    void constructs();
    void corpusRenders();
    void fractionGeometry();
    void scriptGeometry();
    void inlineVsDisplay();
    void inlineBaseline();
    void matrixColumns();
    void alignedColumns();
    void leftRightGrows();
    void radicalCovers();
    void bigOperatorLimits();
    void accentsAndFonts();
    void byteSizeEstimate();
    void errors_data();
    void errors();
    void pathological_data();
    void pathological();
    void fuzz();
    void cancellation();
    void concurrency();
    void perf();
};

void TestMath::initTestCase()
{
    // First-load cost: font database + family resolution + MATH table +
    // glyph outlines, measured before anything else touches the renderer.
    QElapsedTimer t;
    t.start();
    // Platform font database population: paid by any first text use in the
    // app, measured separately so the renderer's own cost is visible.
    const QStringList families = QFontDatabase::families();
    g_fontDbMicros = t.nsecsElapsed() / 1000;
    QVERIFY(!families.isEmpty());
    t.restart();
    const QString family = math::resolvedFontFamily();
    g_resolveMicros = t.nsecsElapsed() / 1000;
    QVERIFY2(!family.isEmpty(), "no math font resolved");
    t.restart();
    const auto r1 = R(QStringLiteral("x^2 + \\frac{a}{b}"));
    g_firstRenderMicros = t.nsecsElapsed() / 1000;
    QVERIFY2(r1.ok, qPrintable(r1.error));
    t.restart();
    const auto r2 = R(QStringLiteral("x^2 + \\frac{a}{b}"));
    g_secondRenderMicros = t.nsecsElapsed() / 1000;
    QFont probe;
    probe.setPointSizeF(kPt);
    g_em = QFontInfo(probe).pixelSize();
    QVERIFY(r2.ok);
    std::printf("MATH font: %s\n", qPrintable(family));
}

void TestMath::constructs_data()
{
    QTest::addColumn<QString>("tex");
    const char *cases[][2] = {
        {"letters", "abc xyz"}, {"digits", "0123456789"}, {"prime", "x'"},
        {"double prime", "f''(x)"}, {"prime+sup", "x'^2"}, {"sub", "x_i"}, {"sup", "x^2"},
        {"subsup", "x_i^2"}, {"supsub order", "x^2_i"}, {"nested scripts", "e^{-x^2}"},
        {"deep scripts", "a^{b^{c^{d}}}"}, {"empty base", "{}^{14}C"}, {"frac", "\\frac{a}{b}"},
        {"frac shorthand", "\\frac12"}, {"dfrac", "\\dfrac{a}{b}"}, {"tfrac", "\\tfrac{a}{b}"},
        {"cfrac", "\\cfrac{1}{1+x}"}, {"binom", "\\binom{n}{k}"}, {"dbinom", "\\dbinom{n}{k}"},
        {"tbinom", "\\tbinom{n}{k}"}, {"over", "{a \\over b}"}, {"choose", "{n \\choose k}"},
        {"sqrt", "\\sqrt{x}"}, {"sqrt index", "\\sqrt[3]{x}"}, {"sqrt tall", "\\sqrt{\\frac{a}{b}}"},
        {"greek", "\\alpha\\beta\\Gamma\\Delta\\varphi\\varepsilon\\vartheta\\varpi\\varrho\\varsigma"},
        {"symbols", "\\infty \\partial \\nabla \\cdot \\times \\pm \\mp"},
        {"relations", "\\leq \\geq \\neq \\approx \\equiv \\sim \\propto \\le \\ge \\ne"},
        {"arrows", "\\to \\rightarrow \\Rightarrow \\Leftrightarrow \\mapsto \\leftarrow \\iff"},
        {"sets", "\\in \\notin \\subset \\subseteq \\cup \\cap \\forall \\exists \\emptyset"},
        {"dots", "\\ldots \\cdots \\vdots \\ddots \\dots"},
        {"sum", "\\sum_{i=1}^n"}, {"prod", "\\prod_{k}"}, {"int", "\\int_a^b"},
        {"iint", "\\iint"}, {"oint", "\\oint_C"}, {"bigcup", "\\bigcup_i"}, {"bigcap", "\\bigcap_i"},
        {"limits forced", "\\int\\limits_0^1"}, {"nolimits", "\\sum\\nolimits_i"},
        {"lim", "\\lim_{x\\to 0}"}, {"max min sup inf", "\\max \\min \\sup \\inf"},
        {"functions", "\\sin \\cos \\tan \\log \\ln \\exp \\det \\arg \\liminf \\limsup"},
        {"operatorname", "\\operatorname{rank} A"}, {"operatorname*", "\\operatorname*{arg\\,max}_x"},
        {"parens", "( ) [ ] \\{ \\} | \\|"}, {"angles", "\\langle x \\rangle"},
        {"floor ceil", "\\lfloor x \\rfloor \\lceil x \\rceil"},
        {"left right", "\\left( \\frac{a}{b} \\right)"}, {"left dot", "\\left. \\frac{a}{b} \\right|"},
        {"right dot", "\\left\\{ x \\right."}, {"middle", "\\left\\{ x \\middle| x > 0 \\right\\}"},
        {"left angle", "\\left\\langle \\frac{a}{b} \\right\\rangle"},
        {"left floor", "\\left\\lfloor \\frac{a}{b} \\right\\rfloor"},
        {"left norm", "\\left\\| \\frac{a}{b} \\right\\|"}, {"left vert", "\\left| \\frac{a}{b} \\right|"},
        {"left brack", "\\left[ \\frac{a}{b} \\right]"},
        {"big", "\\big( \\Big[ \\bigg\\{ \\Bigg| x"}, {"bigl bigr", "\\bigl( x \\bigr)"},
        {"matrix", "\\begin{matrix} a & b \\\\ c & d \\end{matrix}"},
        {"pmatrix", "\\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}"},
        {"bmatrix", "\\begin{bmatrix} a \\\\ b \\end{bmatrix}"},
        {"Bmatrix", "\\begin{Bmatrix} a \\end{Bmatrix}"},
        {"vmatrix", "\\begin{vmatrix} a & b \\\\ c & d \\end{vmatrix}"},
        {"Vmatrix", "\\begin{Vmatrix} a \\end{Vmatrix}"},
        {"smallmatrix", "\\begin{smallmatrix} a & b \\\\ c & d \\end{smallmatrix}"},
        {"cases", "\\begin{cases} 1 & x > 0 \\\\ 0 & \\text{else} \\end{cases}"},
        {"aligned", "\\begin{aligned} a &= b \\\\ c &= d \\end{aligned}"},
        {"aligned pairs", "\\begin{aligned} a &= b & c &= d \\end{aligned}"},
        {"gathered", "\\begin{gathered} a \\\\ b \\end{gathered}"},
        {"array", "\\begin{array}{lcr} a & b & c \\end{array}"},
        {"array rules", "\\begin{array}{|c|c|} \\hline a & b \\\\ \\hline \\end{array}"},
        {"row spacing", "\\begin{matrix} a \\\\[1em] b \\end{matrix}"},
        {"top-level rows", "a = b \\\\ c = d"}, {"top-level align", "a &= b \\\\ c &= d"},
        {"accents", "\\hat a \\bar b \\vec c \\tilde d \\dot e \\ddot f \\check g \\breve h \\acute i \\grave j"},
        {"wide accents", "\\widehat{abc} \\widetilde{abc}"},
        {"overline", "\\overline{a+b}"}, {"underline", "\\underline{a+b}"},
        {"overbrace", "\\overbrace{a+b}^{2}"}, {"underbrace", "\\underbrace{a+b}_{2}"},
        {"overrightarrow", "\\overrightarrow{AB}"},
        {"mathrm", "\\mathrm{d}x"}, {"mathbf", "\\mathbf{v}"}, {"mathit", "\\mathit{x}"},
        {"mathsf", "\\mathsf{A}"}, {"mathtt", "\\mathtt{code}"}, {"mathbb", "\\mathbb{RZNQC}"},
        {"mathcal", "\\mathcal{LH}"}, {"mathfrak", "\\mathfrak{g}"}, {"boldsymbol", "\\boldsymbol{\\alpha x}"},
        {"text", "\\text{if } x"}, {"textbf", "\\textbf{bold}"}, {"textit", "\\textit{it}"},
        {"spacing", "a\\,b\\:c\\;d\\!e\\quad f\\qquad g~h\\ i"}, {"hspace", "a\\hspace{1em}b\\kern3mu c"},
        {"displaystyle", "\\displaystyle\\sum_i x_i"}, {"textstyle", "\\textstyle\\sum_i x_i"},
        {"scriptstyle", "\\scriptstyle x"}, {"phantom", "\\phantom{x}y\\hphantom{x}\\vphantom{X}"},
        {"smash", "\\smash{X}"}, {"boxed", "\\boxed{x=1}"}, {"not", "\\not= \\not\\in \\not\\subset"},
        {"overset", "\\overset{!}{=}"}, {"underset", "\\underset{x}{\\max}"},
        {"stackrel", "\\stackrel{def}{=}"}, {"mathop", "\\mathop{\\mathrm{op}}_a^b"},
        {"class overrides", "a \\mathbin{x} b \\mathrel{y} c \\mathopen{(} d \\mathclose{)}"},
        {"pmod", "a \\equiv b \\pmod{n}"}, {"bmod", "a \\bmod b"},
        {"unicode input", "α + β ≤ ∑ x"}, {"comment", "x % comment\n + y"},
        {"color ignored", "\\color{red} x \\textcolor{blue}{y}"}, {"empty", ""},
        {"whitespace", "   "}, {"empty group", "{}"},
        {"more accents", "\\mathring{a} \\dddot{x} \\overleftrightarrow{AB} \\overleftarrow{v} \\widecheck{xy}"},
        {"more delimiters", "\\lVert x \\rVert \\lvert y \\rvert \\llbracket a \\rrbracket \\Bigl\\langle b \\Bigr\\rangle"},
        {"stretchy arrows/slashes", "\\left\\uparrow \\frac{a}{b} \\right\\Downarrow \\left/ \\frac{a}{b} \\right\\backslash"},
        {"more big ops", "\\iiint \\oiint \\coprod_i \\bigoplus_i \\bigotimes_i \\bigvee \\bigwedge \\biguplus \\bigsqcup"},
        {"more envs", "\\begin{dcases} a & b \\\\ c & d \\end{dcases} \\begin{rcases} a \\\\ b \\end{rcases}"},
        {"align aliases", "\\begin{align*} a &= b \\end{align*} \\begin{split} a &= b \\end{split} \\begin{alignat}{2} a &= b \\end{alignat}"},
        {"gather aliases", "\\begin{gather} a \\\\ b \\end{gather} \\begin{equation} E = mc^2 \\end{equation}"},
        {"misc symbols", "\\hbar \\ell \\wp \\Re \\Im \\aleph \\emptyset \\varnothing \\top \\bot \\angle \\triangle \\nexists \\neg \\prime \\surd"},
        {"more arrows", "\\implies \\iff \\longrightarrow \\Longleftarrow \\hookrightarrow \\leftrightarrows \\rightleftharpoons \\nearrow \\leadsto"},
        {"more relations", "\\ll \\gg \\prec \\succeq \\simeq \\cong \\asymp \\doteq \\models \\vdash \\perp \\parallel \\mid \\nmid \\coloneqq"},
        {"more binops", "\\oplus \\otimes \\odot \\setminus \\wedge \\vee \\star \\circ \\bullet \\div \\ast \\dagger \\amalg"},
        {"old font switches", "{\\bf x} {\\rm d} {\\cal L} {\\it y}"},
        {"limits on integral", "\\int\\limits_{0}^{1}"}, {"displaylimits", "\\sum\\displaylimits_i"},
    };
    for (const auto &c : cases)
        QTest::newRow(c[0]) << QString::fromUtf8(c[1]);
}

void TestMath::constructs()
{
    QFETCH(QString, tex);
    for (bool display : {true, false}) {
        const auto r = R(tex, display);
        QVERIFY2(r.ok, qPrintable(QStringLiteral("%1 -> %2 @%3").arg(tex, r.error).arg(r.errorOffset)));
        QVERIFY(r.list.width >= 0 && r.list.ascent >= 0 && r.list.descent >= 0);
        if (!tex.trimmed().isEmpty() && tex != QLatin1String("{}") && !tex.contains(QLatin1String("phantom")))
            QVERIFY2(!r.list.runs.isEmpty() || !r.list.paths.isEmpty() || !r.list.rules.isEmpty(),
                     qPrintable(tex));
        // every primitive must lie inside the reported box (no clipping)
        const QRectF ink = inkBounds(r.list);
        if (!ink.isEmpty()) {
            const qreal tol = 0.1; // exact outlines on both sides
            QVERIFY2(ink.left() >= -tol && ink.right() <= r.list.width + tol
                         && ink.top() >= -r.list.ascent - tol && ink.bottom() <= r.list.descent + tol,
                     qPrintable(QStringLiteral("%1: ink %2,%3 %4x%5 vs w=%6 a=%7 d=%8")
                                    .arg(tex).arg(ink.left()).arg(ink.top()).arg(ink.width())
                                    .arg(ink.height()).arg(r.list.width).arg(r.list.ascent)
                                    .arg(r.list.descent)));
        }
        // Rasterised pixels: the exact outlines are inside the box (checked
        // above); visible coverage stays within the box grown by the 0.5px
        // position snapping and rounded out to whole pixels.
        const Overshoot o = paintedOvershoot(r.list);
        QVERIFY2(o.max() <= 0,
                 qPrintable(QStringLiteral("%1: painted overshoot L%2 R%3 T%4 B%5 px")
                                .arg(tex).arg(o.left).arg(o.right).arg(o.top).arg(o.bottom)));
    }
}

void TestMath::corpusRenders()
{
    for (int i = 0; i < kMathCorpusSize; ++i) {
        const auto r = R(QString::fromUtf8(kMathCorpus[i].tex));
        QVERIFY2(r.ok, qPrintable(QStringLiteral("%1: %2").arg(QLatin1String(kMathCorpus[i].label), r.error)));
        QVERIFY(r.list.width > 0 && r.list.height() > 0);
    }
}

void TestMath::fractionGeometry()
{
    const auto f = R(QStringLiteral("\\frac{1}{2}"));
    const auto one = R(QStringLiteral("1"));
    const auto two = R(QStringLiteral("2"));
    QVERIFY(f.ok && one.ok && two.ok);
    // taller than numerator + denominator stacked
    QVERIFY(f.list.height() > one.list.height() + two.list.height());
    QCOMPARE(f.list.rules.size(), 1);
    const QRectF bar = f.list.rules.first().rect;
    // bar centred on the math axis: compare with the minus sign's ink centre
    const auto minus = R(QStringLiteral("-"));
    const G m = glyph(minus.list, ucs(0x2212));
    QVERIFY(m);
    const qreal axisY = m.ink.center().y();
    QVERIFY2(qAbs(bar.center().y() - axisY) < 0.05 * g_em,
             qPrintable(QStringLiteral("bar %1 axis %2").arg(bar.center().y()).arg(axisY)));
    QVERIFY(axisY < -0.15 * g_em && axisY > -0.35 * g_em);
    // numerator above the bar, denominator below, both centred on it
    const G n = glyph(f.list, QStringLiteral("1"));
    const G d = glyph(f.list, QStringLiteral("2"));
    QVERIFY(n && d);
    QVERIFY(n.ink.bottom() < bar.top());
    QVERIFY(d.ink.top() > bar.bottom());
    QVERIFY(qAbs(n.cx() - bar.center().x()) < 0.5);
    QVERIFY(qAbs(d.cx() - bar.center().x()) < 0.5);
    // fraction is vertically balanced around the axis (within 0.25 em)
    QVERIFY(qAbs((f.list.ascent + axisY) - (f.list.descent - axisY)) < 0.25 * g_em);
}

void TestMath::scriptGeometry()
{
    const auto sup = R(QStringLiteral("x^2"));
    const auto sub = R(QStringLiteral("x_2"));
    const auto both = R(QStringLiteral("x_i^2"));
    QVERIFY(sup.ok && sub.ok && both.ok);
    const G x = glyph(sup.list, mi('x'));
    const G s2 = glyph(sup.list, QStringLiteral("2"));
    QVERIFY(x && s2);
    QCOMPARE(x.y, 0.0);
    QVERIFY(s2.y < -0.25 * g_em);       // raised
    QVERIFY(s2.px < 0.8 * g_em);        // script size
    QVERIFY(s2.x >= x.x + x.w - 0.5);   // to the right of the base
    const G b2 = glyph(sub.list, QStringLiteral("2"));
    QVERIFY(b2 && b2.y > 0.1 * g_em);   // lowered
    const G bi = glyph(both.list, mi('i'));
    const G b22 = glyph(both.list, QStringLiteral("2"));
    QVERIFY(bi && b22);
    QVERIFY(b22.y < 0 && bi.y > 0);
    // gap between sup bottom and sub top respects a minimum
    QVERIFY(bi.ink.top() - b22.ink.bottom() > 0.1 * g_em);
    // nested superscript climbs further and shrinks (scriptscript)
    const auto nested = R(QStringLiteral("e^{x^2}"));
    const G nx = glyph(nested.list, mi('x'));
    const G n2 = glyph(nested.list, QStringLiteral("2"));
    QVERIFY(nx && n2);
    QVERIFY(n2.y < nx.y);
    QVERIFY(n2.px < nx.px);
}

void TestMath::inlineVsDisplay()
{
    const auto d = R(QStringLiteral("\\frac{a}{b}"), true);
    const auto t = R(QStringLiteral("\\frac{a}{b}"), false);
    QVERIFY(d.ok && t.ok);
    QVERIFY(t.list.height() < d.list.height());
    QVERIFY(t.list.ascent < d.list.ascent);
    // text-style fraction uses script-size numerator
    const G ta = glyph(t.list, mi('a'));
    const G da = glyph(d.list, mi('a'));
    QVERIFY(ta && da);
    QVERIFY(ta.px < da.px);
    // \dfrac inline is as big as display
    const auto df = R(QStringLiteral("\\dfrac{a}{b}"), false);
    QVERIFY(qAbs(df.list.height() - d.list.height()) < 0.5);
    // inline sum puts limits to the right; display stacks them
    const auto sd = R(QStringLiteral("\\sum_{i=1}^n"), true);
    const auto si = R(QStringLiteral("\\sum_{i=1}^n"), false);
    QVERIFY(si.list.height() < sd.list.height());
    QVERIFY(si.list.width > 0 && sd.list.width > 0);
}

void TestMath::inlineBaseline()
{
    // "x" inline sits on the baseline: no descent, ascent ~ x-height
    const auto x = R(QStringLiteral("x"), false);
    QVERIFY(x.ok);
    QVERIFY(x.list.descent < 0.05 * g_em);
    QVERIFY(x.list.ascent > 0.35 * g_em && x.list.ascent < 0.6 * g_em);
    QCOMPARE(x.list.runs.first().pos.y(), 0.0);
    // "E=mc^2" inline: baseline glyphs at y=0, ascent bounded (< 1 em)
    const auto e = R(QStringLiteral("E=mc^2"), false);
    QVERIFY(e.ok);
    QCOMPARE(glyph(e.list, mi('E')).y, 0.0);
    QCOMPARE(glyph(e.list, mi('c')).y, 0.0);
    QVERIFY(e.list.ascent < 1.15 * g_em); // DejaVu's tall digits reach ~1.0 em
    QVERIFY(e.list.descent < 0.1 * g_em);
    // an inline fraction straddles the axis: both ascent and descent > 0
    const auto f = R(QStringLiteral("\\frac{a}{b}"), false);
    QVERIFY(f.list.ascent > 0.5 * g_em && f.list.descent > 0.15 * g_em);
    QVERIFY(f.list.height() < 1.8 * g_em);
}

void TestMath::matrixColumns()
{
    const auto m = R(QStringLiteral("\\begin{matrix} 1 & 222 \\\\ 33 & 4 \\end{matrix}"));
    QVERIFY(m.ok);
    const G a = glyph(m.list, QStringLiteral("1"));
    const G b = glyph(m.list, QStringLiteral("222"));
    const G c = glyph(m.list, QStringLiteral("33"));
    const G d = glyph(m.list, QStringLiteral("4"));
    QVERIFY(a && b && c && d);
    QVERIFY(qAbs(a.cx() - c.cx()) < 0.5);
    QVERIFY(qAbs(b.cx() - d.cx()) < 0.5);
    QVERIFY(b.x > c.x + c.w);
    QCOMPARE(a.y, b.y);
    QCOMPARE(c.y, d.y);
    QVERIFY(c.y > a.y + 0.9 * g_em); // second row below
    // pmatrix delimiters cover both rows
    const auto p = R(QStringLiteral("\\begin{pmatrix} 1 & 222 \\\\ 33 & 4 \\end{pmatrix}"));
    QVERIFY(p.ok);
    QVERIFY(p.list.height() >= m.list.height() - 0.5);
    QVERIFY(p.list.width > m.list.width);
}

void TestMath::alignedColumns()
{
    const auto r = R(QStringLiteral("\\begin{aligned} 1 &= 2 \\\\ 333 &= 44 + 5 \\end{aligned}"));
    QVERIFY(r.ok);
    const G e0 = glyph(r.list, QStringLiteral("="), 0);
    const G e1 = glyph(r.list, QStringLiteral("="), 1);
    QVERIFY(e0 && e1);
    QVERIFY(qAbs(e0.x - e1.x) < 0.01);
    QVERIFY(e1.y > e0.y);
    const G one = glyph(r.list, QStringLiteral("1"));
    const G three = glyph(r.list, QStringLiteral("333"));
    QVERIFY(one && three);
    // left column right-aligned at the '&'
    QVERIFY(qAbs((one.x + one.w) - (three.x + three.w)) < 0.5);
    // relation spacing preserved after '&' (thick space before '=')
    QVERIFY(e0.x - (one.x + one.w) > 0.2 * g_em);
}

void TestMath::leftRightGrows()
{
    const auto small = R(QStringLiteral("\\left( x \\right)"));
    const QString tall = QStringLiteral("\\frac{\\frac{a}{b}}{\\frac{c}{d}}");
    const auto big = R(QStringLiteral("\\left(") + tall + QStringLiteral("\\right)"));
    const auto inner = R(tall);
    QVERIFY(small.ok && big.ok && inner.ok);
    QVERIFY(big.list.height() > 1.8 * small.list.height());
    // the delimiters are drawn as paths (size variants / assemblies) that
    // reach the content extent
    QRectF delimInk;
    for (const auto &p : big.list.paths)
        delimInk = delimInk.united(p.boundingRect());
    QVERIFY2(!delimInk.isEmpty(), "tall \\left( should be drawn as a path");
    QVERIFY(delimInk.height() >= 0.9 * inner.list.height());
    // grows monotonically with content
    qreal prev = 0;
    QString content = QStringLiteral("x");
    for (int i = 0; i < 6; ++i) {
        const auto r = R(QStringLiteral("\\left(") + content + QStringLiteral("\\right)"));
        QVERIFY(r.ok);
        QVERIFY(r.list.height() >= prev);
        prev = r.list.height();
        content = QStringLiteral("\\frac{%1}{y}").arg(content);
    }
    // very tall content uses a glyph assembly (several path pieces per side)
    QString deep = QStringLiteral("\\begin{matrix}") + QStringLiteral("x\\\\").repeated(12) + QStringLiteral("\\end{matrix}");
    const auto huge = R(QStringLiteral("\\left(") + deep + QStringLiteral("\\right)"));
    QVERIFY(huge.ok);
    QVERIFY(huge.list.paths.size() >= 4);
    QVERIFY(huge.list.height() > 10 * g_em);
    // \right. produces no glyph on the right
    const auto open = R(QStringLiteral("\\left\\{ \\frac{a}{b} \\right."));
    QVERIFY(open.ok);
}

void TestMath::radicalCovers()
{
    const auto body = R(QStringLiteral("\\frac{a}{b}"));
    const auto rad = R(QStringLiteral("\\sqrt{\\frac{a}{b}}"));
    QVERIFY(body.ok && rad.ok);
    QVERIFY(rad.list.ascent > body.list.ascent);
    QVERIFY(rad.list.width > body.list.width);
    QVERIFY(rad.list.rules.size() >= 2); // fraction bar + vinculum
    // vinculum is the highest rule and spans the body
    QRectF top = rad.list.rules.first().rect;
    for (const auto &r : rad.list.rules)
        if (r.rect.top() < top.top())
            top = r.rect;
    QVERIFY(top.width() >= body.list.width - 0.5);
    QVERIFY(top.top() < -body.list.ascent);
    // the radical sign reaches below the body
    QRectF sign;
    for (const auto &p : rad.list.paths)
        sign = sign.united(p.boundingRect());
    for (const auto &r : rad.list.runs)
        if (r.text == ucs(0x221A))
            sign = sign.united(runInk(rad.list, r));
    QVERIFY(!sign.isEmpty());
    QVERIFY(sign.bottom() >= body.list.descent - 0.5);
    QVERIFY(qAbs(sign.top() - top.top()) < 0.1 * g_em); // sign joins the vinculum
    // index is small and to the upper left
    const auto idx = R(QStringLiteral("\\sqrt[3]{x}"));
    const G three = glyph(idx.list, QStringLiteral("3"));
    QVERIFY(three);
    QVERIFY(three.y < 0);
    QVERIFY(three.px < 0.6 * g_em);
}

void TestMath::bigOperatorLimits()
{
    const auto d = R(QStringLiteral("\\sum_{i=1}^{n}"), true);
    QVERIFY(d.ok);
    QVERIFY2(!d.list.paths.isEmpty(), "display sum uses the display-size variant");
    const QRectF op = d.list.paths.first().boundingRect();
    const G n = glyph(d.list, mi('n'));
    const G i = glyph(d.list, mi('i'));
    QVERIFY(n && i);
    QVERIFY(n.ink.bottom() < op.top()); // upper limit above
    QVERIFY(i.ink.top() > op.bottom()); // lower limit below
    QVERIFY(qAbs(n.cx() - op.center().x()) < 0.15 * g_em);
    QVERIFY(op.height() > 1.2 * g_em);
    // inline: scripts to the right
    const auto t = R(QStringLiteral("\\sum_{i=1}^{n}"), false);
    const G opRun = glyph(t.list, ucs(0x2211));
    const G tn = glyph(t.list, mi('n'));
    QVERIFY(opRun && tn);
    QVERIFY(tn.x >= opRun.x + opRun.w - 0.5);
    // display integral: large variant, scripts to the right, not stacked
    const auto in = R(QStringLiteral("\\int_0^1"), true);
    QVERIFY(in.ok && !in.list.paths.isEmpty());
    const QRectF iop = in.list.paths.first().boundingRect();
    const auto inText = R(QStringLiteral("\\int"), false);
    QVERIFY(iop.height() > 1.25 * inText.list.height()); // display variant is larger
    const G one = glyph(in.list, QStringLiteral("1"));
    const G zero = glyph(in.list, QStringLiteral("0"));
    QVERIFY(one && zero);
    QVERIFY(one.x > iop.center().x());
    QVERIFY(one.y < zero.y);
    // the subscript is tucked left by the integral's italic correction
    // (equal when the font provides none, e.g. STIX 1.1 "STIX Math")
    QVERIFY(one.x >= zero.x);
    if (math::resolvedFontFamily() == QLatin1String("STIX Two Math"))
        QVERIFY(one.x > zero.x);
    // big operator centred on the axis
    const auto minus = R(QStringLiteral("-"));
    const qreal axisY = glyph(minus.list, ucs(0x2212)).ink.center().y();
    QVERIFY(qAbs(iop.center().y() - axisY) < 0.05 * g_em);
    // \lim limits stack in display
    const auto lim = R(QStringLiteral("\\lim_{x\\to 0}"), true);
    const G lx = glyph(lim.list, mi('x'));
    QVERIFY(lx && lx.y > 0.3 * g_em);
}

void TestMath::accentsAndFonts()
{
    // accent sits above its base and is horizontally near it
    const auto a = R(QStringLiteral("\\hat{a}"));
    QVERIFY(a.ok && !a.list.paths.isEmpty());
    const G base = glyph(a.list, mi('a'));
    QVERIFY(base);
    const QRectF acc = a.list.paths.first().boundingRect();
    QVERIFY(acc.bottom() <= base.ink.top() + 0.05 * g_em);
    QVERIFY(qAbs(acc.center().x() - base.ink.center().x()) < 0.2 * g_em);
    // wide accents stretch over the base
    const auto w = R(QStringLiteral("\\widehat{abcd}"));
    QVERIFY(w.ok && !w.list.paths.isEmpty());
    QVERIFY(w.list.paths.first().boundingRect().width() > 0.7 * w.list.width);
    // Unicode math alphabets (or styled plain letters without them)
    if (hasMathAlnum()) {
        QVERIFY(glyph(R(QStringLiteral("\\mathbb{R}")).list, ucs(0x211D)));
        QVERIFY(glyph(R(QStringLiteral("\\mathbf{v}")).list, ucs(0x1D42F)));
        QVERIFY(glyph(R(QStringLiteral("\\mathcal{L}")).list, ucs(0x2112)));
        QVERIFY(glyph(R(QStringLiteral("\\mathfrak{g}")).list, ucs(0x1D524)));
    } else {
        const auto bf = R(QStringLiteral("\\mathbf{v}"));
        QVERIFY(glyph(bf.list, QStringLiteral("v")));
        QVERIFY(bf.list.fonts[bf.list.runs.first().font].bold());
        const auto it = R(QStringLiteral("x"));
        QVERIFY(it.list.fonts[it.list.runs.first().font].italic());
    }
    QVERIFY(glyph(R(QStringLiteral("\\mathrm{d}")).list, QStringLiteral("d")));
    // digits upright, letters italic
    const auto mix = R(QStringLiteral("2x"));
    QVERIFY(glyph(mix.list, QStringLiteral("2")) && glyph(mix.list, mi('x')));
    // \text uses the text font with spaces preserved
    const auto t = R(QStringLiteral("\\text{if } x"));
    QVERIFY(glyph(t.list, QStringLiteral("if ")));
    // bin -> ord demotion: unary minus gets no medium space
    const auto unary = R(QStringLiteral("-x"));
    const auto binary = R(QStringLiteral("a-x"));
    const G ux = glyph(unary.list, mi('x')), um = glyph(unary.list, ucs(0x2212));
    const G bx = glyph(binary.list, mi('x')), bm = glyph(binary.list, ucs(0x2212));
    QVERIFY(ux && um && bx && bm);
    const qreal unaryGap = ux.x - (um.x + um.w);
    const qreal binaryGap = bx.x - (bm.x + bm.w);
    QVERIFY(qAbs(unaryGap) < 0.02 * g_em);
    QVERIFY(qAbs(binaryGap - 4.0 / 18.0 * g_em) < 0.02 * g_em); // medium space 4mu
    // relation gets a thick space (5mu) in display, none in script style
    const auto rel = R(QStringLiteral("a=b"));
    const G ra = glyph(rel.list, mi('a')), req = glyph(rel.list, QStringLiteral("="));
    QVERIFY(qAbs(req.x - (ra.x + ra.w) - 5.0 / 18.0 * g_em) < 0.02 * g_em);
}

void TestMath::byteSizeEstimate()
{
    const auto small = R(QStringLiteral("x"));
    const auto big = R(QString::fromUtf8(kMathCorpus[14].tex));
    QVERIFY(small.list.byteSize() > 0);
    QVERIFY(big.list.byteSize() > small.list.byteSize());
    QVERIFY(big.list.byteSize() < 512 * 1024);
}

void TestMath::errors_data()
{
    QTest::addColumn<QString>("tex");
    QTest::addColumn<int>("offset");
    QTest::addColumn<QString>("needle");
    QTest::newRow("unbalanced open") << "x + {a + b" << 4 << "brace";
    QTest::newRow("unbalanced open nested") << "\\frac{a}{b" << 8 << "brace";
    QTest::newRow("unbalanced close") << "a + b}" << 5 << "}";
    QTest::newRow("unknown command") << "x + \\foo{y}" << 4 << "\\foo";
    QTest::newRow("left without right") << "a + \\left( b + c" << 4 << "\\right";
    QTest::newRow("right without left") << "a \\right)" << 2 << "\\left";
    QTest::newRow("bad delimiter") << "\\left x \\right)" << 6 << "delimiter";
    QTest::newRow("end mismatch") << "\\begin{matrix} a \\end{pmatrix}" << 17 << "does not match";
    QTest::newRow("missing end") << "x \\begin{cases} a" << 2 << "\\end{cases}";
    QTest::newRow("end without begin") << "a \\end{matrix}" << 2 << "\\begin";
    QTest::newRow("unknown env") << "\\begin{foo} a \\end{foo}" << 7 << "foo";
    QTest::newRow("double superscript") << "x^1^2" << 3 << "superscript";
    QTest::newRow("double subscript") << "x_1_2" << 3 << "subscript";
    QTest::newRow("missing frac arg") << "\\frac{a}" << 0 << "\\frac";
    QTest::newRow("missing sup arg") << "x^" << 1 << "superscript";
    QTest::newRow("misplaced amp") << "{a & b}" << 3 << "&";
    QTest::newRow("lone backslash") << "x\\" << 1 << "backslash";
    QTest::newRow("hash") << "x#" << 1 << "#";
    QTest::newRow("dollar") << "a$b" << 1 << "$";
    QTest::newRow("sqrt index unclosed") << "\\sqrt[3{x}" << 5 << "]";
    QTest::newRow("bad array spec") << "\\begin{array}{cx} a \\end{array}" << 15 << "column";
    QTest::newRow("limits misplaced") << "x\\limits" << 1 << "operator";
    QTest::newRow("text unclosed") << "\\text{abc" << 5 << "brace";
}

void TestMath::errors()
{
    QFETCH(QString, tex);
    QFETCH(int, offset);
    QFETCH(QString, needle);
    const auto r = R(tex);
    QVERIFY2(!r.ok, qPrintable(tex));
    QVERIFY2(r.error.contains(needle, Qt::CaseInsensitive),
             qPrintable(QStringLiteral("'%1' lacks '%2'").arg(r.error, needle)));
    QVERIFY2(r.errorOffset == offset,
             qPrintable(QStringLiteral("%1: offset %2 expected %3 (%4)").arg(tex).arg(r.errorOffset).arg(offset).arg(r.error)));
    QVERIFY(r.list.runs.isEmpty() && r.list.paths.isEmpty());
}

void TestMath::pathological_data()
{
    QTest::addColumn<QString>("tex");
    QTest::addColumn<bool>("relaxed"); // raise char/node limits to exercise deeper guards
    auto rep = [](const QString &s, int n) { return s.repeated(n); };
    const QString nestedBraces = rep(QStringLiteral("{"), 10000) + QStringLiteral("x") + rep(QStringLiteral("}"), 10000);
    const QString deepSup = rep(QStringLiteral("x^{"), 1000) + QStringLiteral("x") + rep(QStringLiteral("}"), 1000);
    const QString deepFrac = rep(QStringLiteral("\\frac{"), 200) + QStringLiteral("1") + rep(QStringLiteral("}{2}"), 200);
    const QString bareFrac = rep(QStringLiteral("\\frac"), 5000) + QStringLiteral("12");
    QString matrix = QStringLiteral("\\begin{pmatrix}");
    for (int r = 0; r < 100; ++r)
        matrix += rep(QStringLiteral("1&"), 99) + QStringLiteral("1\\\\");
    matrix += QStringLiteral("\\end{pmatrix}");
    const QString manyLeftNested = rep(QStringLiteral("\\left("), 2000) + QStringLiteral("x") + rep(QStringLiteral("\\right)"), 2000);
    const QString manyLeftFlat = rep(QStringLiteral("\\left( x \\right)"), 500);
    const QString manyLeftUnclosed = rep(QStringLiteral("\\left( x "), 600);
    const QString flat = rep(QStringLiteral("x+"), 50000);
    const QString deepSqrt = rep(QStringLiteral("\\sqrt"), 3000) + QStringLiteral("x");
    const QString scripts = rep(QStringLiteral("x_1^2"), 1600);
    const QString bigText = QStringLiteral("\\text{") + rep(QStringLiteral("a"), 1990) + QStringLiteral("}");
    const QString hugeSpace = QStringLiteral("\\hspace{1000000em}");
    const QString bigs = rep(QStringLiteral("\\Bigg("), 1500);
    const QString nodeLimit = rep(QStringLiteral("x+"), 4000);
    for (bool relaxed : {false, true}) {
        const char *tag = relaxed ? " (relaxed)" : "";
        QTest::newRow(qPrintable(QStringLiteral("100k chars%1").arg(QLatin1String(tag)))) << rep(QStringLiteral("x"), 100000) << relaxed;
        QTest::newRow(qPrintable(QStringLiteral("10k nested braces%1").arg(QLatin1String(tag)))) << nestedBraces << relaxed;
        QTest::newRow(qPrintable(QStringLiteral("1000-deep x^{x^{...}}%1").arg(QLatin1String(tag)))) << deepSup << relaxed;
        QTest::newRow(qPrintable(QStringLiteral("200-deep frac%1").arg(QLatin1String(tag)))) << deepFrac << relaxed;
        QTest::newRow(qPrintable(QStringLiteral("5000 bare frac%1").arg(QLatin1String(tag)))) << bareFrac << relaxed;
        QTest::newRow(qPrintable(QStringLiteral("100x100 matrix%1").arg(QLatin1String(tag)))) << matrix << relaxed;
        QTest::newRow(qPrintable(QStringLiteral("2000 nested left%1").arg(QLatin1String(tag)))) << manyLeftNested << relaxed;
        QTest::newRow(qPrintable(QStringLiteral("500 flat left/right%1").arg(QLatin1String(tag)))) << manyLeftFlat << relaxed;
        QTest::newRow(qPrintable(QStringLiteral("600 unclosed left%1").arg(QLatin1String(tag)))) << manyLeftUnclosed << relaxed;
        QTest::newRow(qPrintable(QStringLiteral("100k flat atoms%1").arg(QLatin1String(tag)))) << flat << relaxed;
        QTest::newRow(qPrintable(QStringLiteral("3000 nested sqrt%1").arg(QLatin1String(tag)))) << deepSqrt << relaxed;
        QTest::newRow(qPrintable(QStringLiteral("1600 scripted atoms%1").arg(QLatin1String(tag)))) << scripts << relaxed;
        QTest::newRow(qPrintable(QStringLiteral("2000-char text%1").arg(QLatin1String(tag)))) << bigText << relaxed;
        QTest::newRow(qPrintable(QStringLiteral("huge hspace%1").arg(QLatin1String(tag)))) << hugeSpace << relaxed;
        QTest::newRow(qPrintable(QStringLiteral("1500 Bigg%1").arg(QLatin1String(tag)))) << bigs << relaxed;
        QTest::newRow(qPrintable(QStringLiteral("8000 atoms (node limit)%1").arg(QLatin1String(tag)))) << nodeLimit << relaxed;
    }
}

void TestMath::pathological()
{
    QFETCH(QString, tex);
    QFETCH(bool, relaxed);
    math::Limits lim;
    if (relaxed) {
        lim.maxSourceChars = 1 << 20;
        lim.maxNodes = 1 << 20;
        lim.maxDepth = 1 << 20; // clamped internally
    }
    QElapsedTimer t;
    t.start();
    const auto r = R(tex, true, kPt, lim);
    const qint64 us = t.nsecsElapsed() / 1000;
    std::printf("  pathological %-34s %6lld us  ok=%d nodes=%d bytes=%lld %s\n",
                QTest::currentDataTag(), us, int(r.ok), r.nodeCount,
                (long long)r.list.byteSize(), qPrintable(r.error.left(60)));
    QVERIFY2(us < 50000, qPrintable(QStringLiteral("took %1 us").arg(us)));
    if (r.ok) {
        QVERIFY(r.list.byteSize() < 16 * 1024 * 1024);
        QVERIFY(r.list.width < 4000 * g_em && r.list.height() < 4000 * g_em);
    } else {
        QVERIFY(!r.error.isEmpty());
    }
}

void TestMath::fuzz()
{
    // Deterministic token soup: never crash/hang, offsets in range, ok
    // results keep all ink inside their box.
    static const char *tokens[] = {
        "\\frac", "{", "}", "^", "_", "\\left(", "\\right)", "\\left.", "\\right|", "\\middle|",
        "\\begin{matrix}", "\\end{matrix}", "\\begin{pmatrix}", "\\end{pmatrix}",
        "\\begin{array}{c|c}", "\\end{array}", "\\begin{cases}", "\\end{cases}", "\\hline",
        "&", "\\\\", "x", "1", "+", "-", "=", "\\sqrt", "[", "]", "\\sum", "\\int", "\\lim",
        "\\limits", "\\hat", "\\widehat", "\\vec", "\\text{a b}", "\\mathbb", "\\mathbf",
        "\\overbrace", "\\underbrace", "'", "\\big(", "\\Bigg]", "\\not", "\\over", "\\binom",
        "\\alpha", "\\infty", "%c\n", "~", "\\,", "\\!", "\\quad", "\\color{red}", "\\displaystyle",
        "\\scriptstyle", "\\operatorname", "\\phantom", "\\boxed", "\\overset", " ", "\\foo", "$",
        "#", "\\", "\\hspace{2em}", "\\\\[3pt]", "\\stackrel", "\\pmod", "\\ldots",
    };
    const int nTok = int(sizeof(tokens) / sizeof(tokens[0]));
    quint32 state = 0x12345678u;
    auto rnd = [&state](quint32 n) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state % n;
    };
    int okCount = 0;
    qint64 worstUs = 0;
    QString worst;
    for (int iter = 0; iter < 3000; ++iter) {
        QString tex;
        const int len = 1 + int(rnd(60));
        for (int k = 0; k < len; ++k)
            tex += QString::fromUtf8(tokens[rnd(quint32(nTok))]);
        QElapsedTimer t;
        t.start();
        const auto r = R(tex, rnd(2) == 0);
        const qint64 us = t.nsecsElapsed() / 1000;
        if (us > worstUs) {
            worstUs = us;
            worst = tex;
        }
        QVERIFY2(us < 50000, qPrintable(tex));
        if (r.ok) {
            ++okCount;
            const QRectF ink = inkBounds(r.list);
            if (!ink.isEmpty())
                QVERIFY2(ink.left() >= -0.1 && ink.right() <= r.list.width + 0.1
                             && ink.top() >= -r.list.ascent - 0.1 && ink.bottom() <= r.list.descent + 0.1,
                         qPrintable(tex));
        } else {
            QVERIFY2(!r.error.isEmpty(), qPrintable(tex));
            QVERIFY2(r.errorOffset >= -1 && r.errorOffset <= tex.size(), qPrintable(tex));
        }
    }
    std::printf("  fuzz (token soup): 3000 inputs, %d rendered ok, worst %lld us\n", okCount, (long long)worstUs);

    // Grammar-based: random *valid* nested formulas must all render, finish
    // quickly and keep their ink inside the reported box.
    std::function<QString(int)> gen = [&](int depth) -> QString {
        static const char *atoms[] = {"x", "y", "1", "42", "\\alpha", "\\infty", "+", "-", "=",
                                      "\\leq", "\\cdot", ",", "f(x)", "\\partial", "A"};
        const quint32 pick = depth <= 0 ? 0 : rnd(16);
        auto sub = [&]() { return gen(depth - 1 - int(rnd(2))); };
        switch (pick) {
        case 0: case 1: case 2: return QString::fromUtf8(atoms[rnd(15)]);
        case 3: return QStringLiteral("\\frac{%1}{%2}").arg(sub(), sub());
        case 4: return QStringLiteral("\\sqrt{%1}").arg(sub());
        case 5: return QStringLiteral("\\sqrt[%1]{%2}").arg(sub(), sub());
        case 6: return QStringLiteral("{%1}^{%2}_{%3}").arg(sub(), sub(), sub());
        case 7: return QStringLiteral("\\left(%1\\right)").arg(sub());
        case 8: return QStringLiteral("\\left\\{%1\\middle|%2\\right.").arg(sub(), sub());
        case 9: return QStringLiteral("\\sum_{%1}^{%2}%3").arg(sub(), sub(), sub());
        case 10: return QStringLiteral("\\int_{%1}^{%2}%3\\,dx").arg(sub(), sub(), sub());
        case 11: return QStringLiteral("\\begin{pmatrix}%1&%2\\\\%3&%4\\end{pmatrix}").arg(sub(), sub(), sub(), sub());
        case 12: return QStringLiteral("\\begin{aligned}%1&=%2\\\\&=%3\\end{aligned}").arg(sub(), sub(), sub());
        case 13: return QStringLiteral("\\widehat{%1}\\overline{%2}").arg(sub(), sub());
        case 14: return QStringLiteral("\\overbrace{%1}^{%2}").arg(sub(), sub());
        default: return QStringLiteral("%1 %2").arg(sub(), sub());
        }
    };
    int valid = 0;
    qint64 worstValidUs = 0;
    for (int iter = 0; iter < 1500; ++iter) {
        const QString tex = gen(2 + int(rnd(5)));
        QElapsedTimer t;
        t.start();
        const auto r = R(tex, rnd(2) == 0);
        const qint64 us = t.nsecsElapsed() / 1000;
        worstValidUs = qMax(worstValidUs, us);
        QVERIFY2(us < 50000, qPrintable(tex));
        if (!r.ok) {
            // only the declared limits may reject a well-formed formula
            QVERIFY2(r.error.contains(QLatin1String("nested too deeply"))
                         || r.error.contains(QLatin1String("too long"))
                         || r.error.contains(QLatin1String("too complex")),
                     qPrintable(tex + QStringLiteral(" -> ") + r.error));
            continue;
        }
        ++valid;
        const QRectF ink = inkBounds(r.list);
        QVERIFY2(ink.isEmpty()
                     || (ink.left() >= -0.1 && ink.right() <= r.list.width + 0.1
                         && ink.top() >= -r.list.ascent - 0.1 && ink.bottom() <= r.list.descent + 0.1),
                 qPrintable(tex));
    }
    std::printf("  fuzz (grammar): 1500 inputs, %d rendered ok, worst %lld us\n", valid, (long long)worstValidUs);
}

void TestMath::cancellation()
{
    // already cancelled
    std::atomic<bool> cancel{true};
    math::Style st;
    QElapsedTimer t;
    t.start();
    auto r = math::render(QStringLiteral("x^2"), st, {}, &cancel);
    QVERIFY(!r.ok);
    QCOMPARE(r.error, QStringLiteral("cancelled"));
    QVERIFY(t.elapsed() < 5);

    // cancelled mid-flight from another thread
    cancel = false;
    math::Limits lim;
    lim.maxSourceChars = 1 << 22;
    lim.maxNodes = 1 << 22;
    lim.maxMillis = 20000;
    const QString heavy = QStringLiteral("\\frac{x}{y}+").repeated(150000);
    math::Result res;
    qint64 cancelAt = 0, doneAt = 0;
    QElapsedTimer clock;
    clock.start();
    QThread *worker = QThread::create([&] {
        res = math::render(heavy, st, lim, &cancel);
        doneAt = clock.nsecsElapsed();
    });
    worker->start();
    QThread::msleep(10);
    cancelAt = clock.nsecsElapsed();
    cancel = true;
    QVERIFY(worker->wait(5000));
    delete worker;
    QVERIFY(!res.ok);
    QCOMPARE(res.error, QStringLiteral("cancelled"));
    const qint64 latencyUs = (doneAt - cancelAt) / 1000;
    std::printf("  cancel latency: %lld us\n", (long long)latencyUs);
    QVERIFY(latencyUs < 10000);

    // deadline
    lim.maxMillis = 15;
    cancel = false;
    t.restart();
    r = math::render(heavy, st, lim, &cancel);
    QVERIFY(!r.ok);
    QVERIFY(r.error.contains(QLatin1String("time limit")));
    QVERIFY(t.elapsed() < 30);
}

void TestMath::concurrency()
{
    std::vector<QString> inputs;
    // Cold glyphs first: these alphabets are not used by earlier tests, so
    // the concurrent pass also exercises cache misses / font engines on
    // worker threads.
    inputs.push_back(QStringLiteral("\\mathfrak{ABCDEFG} \\boxplus \\boxtimes \\lll \\ggg \\sqrt[4]{\\mathsf{XYZ}}"));
    inputs.push_back(QStringLiteral("\\mathtt{HELLO} \\circlearrowleft \\twoheadrightarrow \\Bigg\\lceil q \\Bigg\\rceil"));
    for (int i = 0; i < kMathCorpusSize; ++i)
        inputs.push_back(QString::fromUtf8(kMathCorpus[i].tex));
    const int N = int(inputs.size());
    std::vector<math::Result> concurrent(size_t(N) * 4);
    QThreadPool pool;
    pool.setMaxThreadCount(4);
    std::atomic<int> started{0};
    for (int w = 0; w < 4; ++w) {
        pool.start([&, w] {
            started++;
            while (started.load() < 4)
                QThread::yieldCurrentThread();
            for (int k = 0; k < N; ++k) {
                const int idx = (k + w * 7) % N; // different order per thread
                concurrent[size_t(w * N + idx)] = R(inputs[size_t(idx)], (idx + w) % 2 == 0);
            }
        });
    }
    pool.waitForDone();
    int mismatches = 0;
    for (int w = 0; w < 4; ++w) {
        for (int k = 0; k < N; ++k) {
            const auto ref = R(inputs[size_t(k)], (k + w) % 2 == 0);
            const auto &got = concurrent[size_t(w * N + k)];
            if (ref.ok != got.ok || !sameList(ref.list, got.list)) {
                ++mismatches;
                qWarning() << "mismatch" << w << k << inputs[size_t(k)];
            }
        }
    }
    QCOMPARE(mismatches, 0);
    // results rendered on workers paint fine on the GUI thread
    QImage img(400, 200, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    QPainter p(&img);
    concurrent[0].list.paint(&p, QPointF(10, 100), Qt::black);
    p.end();
    bool inked = false;
    for (int y = 0; y < img.height() && !inked; ++y)
        for (int x = 0; x < img.width() && !inked; ++x)
            inked = qGray(img.pixel(x, y)) < 128;
    QVERIFY(inked);
}

void TestMath::perf()
{
    std::vector<QString> corpus;
    for (int i = 0; i < kMathCorpusSize; ++i)
        corpus.push_back(QString::fromUtf8(kMathCorpus[i].tex));
    math::Style st;
    st.pointSize = 14;
    // warm (metrics cache already hot from earlier tests; one pass anyway)
    for (const QString &s : corpus)
        QVERIFY(math::render(s, st).ok);
    const int passes = 12;
    std::vector<double> us;
    us.reserve(size_t(passes) * corpus.size());
    QElapsedTimer t;
    for (int p = 0; p < passes; ++p) {
        for (size_t i = 0; i < corpus.size(); ++i) {
            st.display = (p % 2 == 0);
            t.start();
            const auto r = math::render(corpus[i], st);
            us.push_back(t.nsecsElapsed() / 1000.0);
            QVERIFY(r.ok);
        }
    }
    // paint cost on the GUI thread (2x QImage)
    std::vector<double> paintUs;
    QImage img(QSize(2400, 400), QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(2);
    qint64 bytes = 0;
    for (size_t i = 0; i < corpus.size(); ++i) {
        st.display = true;
        const auto r = math::render(corpus[i], st);
        bytes += r.list.byteSize();
        for (int k = 0; k < 3; ++k) {
            img.fill(Qt::white);
            QPainter p(&img);
            t.start();
            r.list.paint(&p, QPointF(4, 100), Qt::black);
            p.end();
            paintUs.push_back(t.nsecsElapsed() / 1000.0);
        }
    }
    double sum = 0;
    for (double v : us)
        sum += v;
    const math::detail::FontData *fd = math::detail::fontData();
    std::printf("PERF first-load: QFontDatabase::families() %lld us (platform font DB, app-wide); "
                "resolvedFontFamily() %lld us (math font resolution + MATH table + %zu glyph outlines; "
                "FontData build %lld us); first render (cold glyph cache) %lld us; same formula again %lld us\n",
                (long long)g_fontDbMicros, (long long)g_resolveMicros, fd->shapes.size(),
                (long long)fd->initMicros, (long long)g_firstRenderMicros,
                (long long)g_secondRenderMicros);
    std::printf("PERF render: n=%zu (corpus %zu x %d passes, 14pt, alternating display/inline) "
                "p50=%.1f us p95=%.1f us p99=%.1f us mean=%.1f us max=%.1f us\n",
                us.size(), corpus.size(), passes, percentile(us, 0.50), percentile(us, 0.95),
                percentile(us, 0.99), sum / double(us.size()), percentile(us, 1.0));
    std::printf("PERF paint (2x QImage, GUI thread): n=%zu p50=%.1f us p95=%.1f us p99=%.1f us\n",
                paintUs.size(), percentile(paintUs, 0.5), percentile(paintUs, 0.95), percentile(paintUs, 0.99));
    std::printf("PERF display-list byteSize: mean %.0f bytes over %zu formulas\n",
                double(bytes) / double(corpus.size()), corpus.size());
    QVERIFY(us.size() >= 200);
}

QTEST_MAIN(TestMath)
#include "tst_math.moc"
