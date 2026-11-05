#include "math/MathFont.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QPainterPath>
#include <QGuiApplication>
#include <QRawFont>
#include <QSet>
#include <QTransform>
#include <QtEndian>

#include <atomic>
#include <mutex>
#include <shared_mutex>

namespace folia {
namespace math {
namespace detail {

namespace {

constexpr qreal kRefPx = 100.0; // measurement size; metrics are stored per em

// ------------------------------------------------------------ MATH parsing --
struct Reader {
    const uchar *d = nullptr;
    int n = 0;
    bool ok = true;
    quint16 u16(int o)
    {
        if (o < 0 || o + 2 > n) {
            ok = false;
            return 0;
        }
        return quint16((d[o] << 8) | d[o + 1]);
    }
    qint16 s16(int o) { return qint16(u16(o)); }
};

// Coverage table -> (glyph id -> coverage index)
std::unordered_map<quint32, int> readCoverage(Reader &r, int off)
{
    std::unordered_map<quint32, int> cov;
    if (off <= 0)
        return cov;
    const int format = r.u16(off);
    if (format == 1) {
        const int count = r.u16(off + 2);
        for (int i = 0; i < count && r.ok; ++i)
            cov[r.u16(off + 4 + 2 * i)] = i;
    } else if (format == 2) {
        const int count = r.u16(off + 2);
        for (int i = 0; i < count && r.ok; ++i) {
            const int rec = off + 4 + 6 * i;
            const int start = r.u16(rec), end = r.u16(rec + 2), startIdx = r.u16(rec + 4);
            for (int g = start; g <= end && g - start < 65536; ++g)
                cov[quint32(g)] = startIdx + (g - start);
        }
    }
    return cov;
}

// MathItalicsCorrectionInfo / MathTopAccentAttachment share a layout.
std::unordered_map<quint32, qreal> readGlyphValues(Reader &r, int off, qreal upem)
{
    std::unordered_map<quint32, qreal> out;
    if (off <= 0)
        return out;
    const int covOff = r.u16(off);
    const int count = r.u16(off + 2);
    auto cov = readCoverage(r, off + covOff);
    for (const auto &kv : cov) {
        if (kv.second < count)
            out[kv.first] = r.s16(off + 4 + 4 * kv.second) / upem;
    }
    return out;
}

struct RawMath {
    bool ok = false;
    qreal upem = 1000;
    std::unordered_map<quint32, qreal> italics, accents;
    std::unordered_map<quint32, int> vertCov, horizCov;
    int variantsOff = 0, vertCount = 0, horizCount = 0;
};

GlyphConstruction readConstruction(Reader &r, int off, qreal upem)
{
    GlyphConstruction gc;
    const int asmOff = r.u16(off);
    const int count = r.u16(off + 2);
    for (int i = 0; i < count && r.ok; ++i) {
        GlyphVariant v;
        v.gid = r.u16(off + 4 + 4 * i);
        v.size = r.u16(off + 6 + 4 * i) / upem;
        gc.variants.push_back(v);
    }
    if (asmOff) {
        const int a = off + asmOff;
        const int parts = r.u16(a + 4);
        for (int i = 0; i < parts && r.ok; ++i) {
            const int p = a + 6 + 10 * i;
            GlyphPart gp;
            gp.gid = r.u16(p);
            gp.startConnector = r.u16(p + 2) / upem;
            gp.endConnector = r.u16(p + 4) / upem;
            gp.fullAdvance = r.u16(p + 6) / upem;
            gp.extender = r.u16(p + 8) & 1;
            gc.parts.push_back(gp);
        }
    }
    return gc;
}

// Characters whose outlines/variants we may draw as paths (delimiters,
// radicals, big operators, accents, horizontal braces/arrows).
const char32_t kShapeChars[] = {
    '(', ')', '[', ']', '{', '}', '|', '/', '\\', 0x2016, 0x2223, 0x2225,
    0x27E8, 0x27E9, 0x230A, 0x230B, 0x2308, 0x2309, 0x27E6, 0x27E7,
    0x2191, 0x2193, 0x2195, 0x21D1, 0x21D3, 0x21D5, 0x221A,
    0x222B, 0x222C, 0x222D, 0x222E, 0x222F, 0x2230, 0x2A0C,
    0x2211, 0x220F, 0x2210, 0x22C3, 0x22C2, 0x22C1, 0x22C0,
    0x2A00, 0x2A01, 0x2A02, 0x2A04, 0x2A06,
    0x23DE, 0x23DF, 0x23DC, 0x23DD, 0x23B4, 0x23B5,
    0x0300, 0x0301, 0x0302, 0x0303, 0x0304, 0x0305, 0x0306, 0x0307, 0x0308,
    0x030A, 0x030C, 0x20D6, 0x20D7, 0x20DB, 0x20E1, 0x2190, 0x2192, 0x2194,
};

void addRange(QList<char32_t> &v, char32_t a, char32_t b)
{
    for (char32_t c = a; c <= b; ++c)
        v.push_back(c);
}

QString pickFamily(const QSet<QString> &have, std::initializer_list<const char *> chain,
                   const QString &fallback)
{
    for (const char *c : chain) {
        if (have.contains(QString::fromLatin1(c).toLower()))
            return QString::fromLatin1(c);
    }
    return fallback;
}

void setDefaultConstants(FontData &fd)
{
    // TeX (cmsy10/cmex10) derived defaults, in em. Used when the font has no
    // OpenType MATH table.
    const qreal t = 0.04, xh = fd.xHeight;
    qreal *c = fd.c;
    c[MathLeading] = 0.15;
    c[AxisHeight] = 0.25;
    c[AccentBaseHeight] = xh;
    c[FlattenedAccentBaseHeight] = 0.66;
    c[SubscriptShiftDown] = 0.2;
    c[SubscriptTopMax] = 0.8 * xh;
    c[SubscriptBaselineDropMin] = 0.05;
    c[SuperscriptShiftUp] = 0.363;
    c[SuperscriptShiftUpCramped] = 0.289;
    c[SuperscriptBottomMin] = 0.25 * xh;
    c[SuperscriptBaselineDropMax] = 0.25;
    c[SubSuperscriptGapMin] = 4 * t;
    c[SuperscriptBottomMaxWithSubscript] = 0.8 * xh;
    c[SpaceAfterScript] = 0.05;
    c[UpperLimitGapMin] = 0.111;
    c[UpperLimitBaselineRiseMin] = 0.2;
    c[LowerLimitGapMin] = 0.167;
    c[LowerLimitBaselineDropMin] = 0.6;
    c[StackTopShiftUp] = 0.444;
    c[StackTopDisplayStyleShiftUp] = 0.677;
    c[StackBottomShiftDown] = 0.345;
    c[StackBottomDisplayStyleShiftDown] = 0.686;
    c[StackGapMin] = 3 * t;
    c[StackDisplayStyleGapMin] = 7 * t;
    c[StretchStackTopShiftUp] = 0.3;
    c[StretchStackBottomShiftDown] = 0.3;
    c[StretchStackGapAboveMin] = 0.1;
    c[StretchStackGapBelowMin] = 0.1;
    c[FractionNumeratorShiftUp] = 0.394;
    c[FractionNumeratorDisplayStyleShiftUp] = 0.677;
    c[FractionDenominatorShiftDown] = 0.345;
    c[FractionDenominatorDisplayStyleShiftDown] = 0.686;
    c[FractionNumeratorGapMin] = t;
    c[FractionNumDisplayStyleGapMin] = 3 * t;
    c[FractionRuleThickness] = t;
    c[FractionDenominatorGapMin] = t;
    c[FractionDenomDisplayStyleGapMin] = 3 * t;
    c[SkewedFractionHorizontalGap] = 0.35;
    c[SkewedFractionVerticalGap] = 0.1;
    c[OverbarVerticalGap] = 3 * t;
    c[OverbarRuleThickness] = t;
    c[OverbarExtraAscender] = t;
    c[UnderbarVerticalGap] = 3 * t;
    c[UnderbarRuleThickness] = t;
    c[UnderbarExtraDescender] = t;
    c[RadicalVerticalGap] = t + t / 4;
    c[RadicalDisplayStyleVerticalGap] = t + xh / 4;
    c[RadicalRuleThickness] = t;
    c[RadicalExtraAscender] = t;
    c[RadicalKernBeforeDegree] = 0.278;
    c[RadicalKernAfterDegree] = -0.556;
    fd.radicalDegreeBottomRaise = 0.6;
    fd.scriptScale = 0.7;
    fd.scriptScriptScale = 0.5;
    fd.delimitedSubFormulaMinHeight = 1.3;
    fd.displayOperatorMinHeight = 1.4;
}

FontData *buildFontData()
{
    QElapsedTimer timer;
    timer.start();
    auto *fd = new FontData;

    QSet<QString> have;
    const QStringList fams = QFontDatabase::families();
    for (const QString &f : fams)
        have.insert(f.toLower());

    QString generic;
    {
        const char *serifs[] = {"Times New Roman", "Times", "Georgia", "DejaVu Serif",
                                "Liberation Serif", "Noto Serif", "Cambria"};
        for (const char *s : serifs) {
            if (have.contains(QString::fromLatin1(s).toLower())) {
                generic = QString::fromLatin1(s);
                break;
            }
        }
        if (generic.isEmpty())
            generic = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    }

    const QByteArray env = qgetenv("FOLIA_MATH_FONT");
    if (!env.isEmpty() && have.contains(QString::fromUtf8(env).toLower()))
        fd->mathFamily = QString::fromUtf8(env);
    else
        fd->mathFamily = pickFamily(have,
                                    {"STIX Two Math", "Cambria Math", "Latin Modern Math",
                                     "XITS Math", "STIX Math", "TeX Gyre Termes Math",
                                     "Libertinus Math", "STIX Two Text", "STIXGeneral"},
                                    generic);
    fd->textFamily = pickFamily(have, {"STIX Two Text", "Cambria", "Latin Modern Roman",
                                       "STIXGeneral", "Times New Roman", "Times", "Georgia",
                                       "DejaVu Serif", "Liberation Serif"},
                                fd->mathFamily);
    if (!env.isEmpty() && fd->mathFamily == QString::fromUtf8(env))
        fd->textFamily = fd->mathFamily;
    fd->sansFamily = pickFamily(have, {"Helvetica Neue", "Helvetica", "Arial", "Segoe UI",
                                       "DejaVu Sans", "Liberation Sans", "Noto Sans"},
                                fd->textFamily);
    fd->monoFamily = pickFamily(have, {"Menlo", "Monaco", "Consolas", "DejaVu Sans Mono",
                                       "Liberation Mono", "Courier New"},
                                fd->textFamily);

    QFont mf(fd->mathFamily);
    mf.setPointSizeF(100);
    QRawFont rf = QRawFont::fromFont(mf);
    if (rf.isValid()) {
        fd->pxPerPt = rf.pixelSize() / 100.0;
        const qreal upem = rf.unitsPerEm() > 0 ? rf.unitsPerEm() : 1000;
        fd->xHeight = rf.xHeight() / rf.pixelSize();
        rf.setPixelSize(upem); // paths/advances come back in font units
        setDefaultConstants(*fd);

        const QByteArray math = rf.fontTable("MATH");
        std::unordered_map<quint32, qreal> italics, accents;
        std::unordered_map<quint32, int> vertCov, horizCov;
        int mvOff = 0, vertCount = 0, horizCount = 0;
        Reader r{reinterpret_cast<const uchar *>(math.constData()), int(math.size())};
        if (math.size() > 10 && r.u16(0) == 1) {
            const int mc = r.u16(4), gi = r.u16(6), mv = r.u16(8);
            if (mc) {
                // Some text fonts ship a partial MATH table (Debian's DejaVu
                // Serif leaves sub/superscript and limit shifts at 0), so
                // every missing or nonsensical constant falls back to the
                // TeX-derived default individually.
                const FontData defaults = *fd;
                auto pick = [&](qreal v, qreal def, bool allowNegative = false) {
                    const bool bad = allowNegative ? v == 0 : v <= 0;
                    if (bad && def != 0) {
                        ++fd->patchedConstants;
                        return def;
                    }
                    return v;
                };
                fd->scriptScale = r.s16(mc) / 100.0;
                fd->scriptScriptScale = r.s16(mc + 2) / 100.0;
                if (fd->scriptScale < 0.4 || fd->scriptScale > 0.95) {
                    fd->scriptScale = defaults.scriptScale;
                    ++fd->patchedConstants;
                }
                if (fd->scriptScriptScale < 0.3 || fd->scriptScriptScale > fd->scriptScale) {
                    fd->scriptScriptScale = qMin(defaults.scriptScriptScale, fd->scriptScale);
                    ++fd->patchedConstants;
                }
                fd->delimitedSubFormulaMinHeight =
                    pick(r.u16(mc + 4) / upem, defaults.delimitedSubFormulaMinHeight);
                fd->displayOperatorMinHeight = pick(r.u16(mc + 6) / upem, defaults.displayOperatorMinHeight);
                for (int i = 0; i < MC_COUNT; ++i) {
                    const qreal v = r.s16(mc + 8 + 4 * i) / upem;
                    // Larger than 3 em is never a sane math constant.
                    const qreal sane = qAbs(v) > 3.0 ? 0 : v;
                    fd->c[i] = i == MathLeading ? qMax<qreal>(0, sane)
                                                : pick(sane, defaults.c[i], i == RadicalKernAfterDegree);
                }
                fd->radicalDegreeBottomRaise =
                    pick(r.s16(mc + 8 + 4 * MC_COUNT) / 100.0, defaults.radicalDegreeBottomRaise);
                if (fd->radicalDegreeBottomRaise > 1)
                    fd->radicalDegreeBottomRaise = defaults.radicalDegreeBottomRaise;
                // A real math font sets all core shifts. If several are
                // missing the remaining values are not trustworthy either
                // (DejaVu sets SuperscriptBottomMin to the x-height): keep
                // only the font-geometry constants and use the TeX defaults
                // for the rest. Glyph constructions are still used.
                const int core[] = {SubscriptShiftDown, SuperscriptShiftUp, FractionRuleThickness,
                                    FractionNumeratorShiftUp, FractionDenominatorShiftDown,
                                    UpperLimitGapMin, LowerLimitGapMin, StackTopShiftUp,
                                    RadicalRuleThickness, OverbarRuleThickness};
                int missingCore = 0;
                for (int k : core)
                    missingCore += r.s16(mc + 8 + 4 * k) <= 0 ? 1 : 0;
                if (missingCore >= 2) {
                    const qreal axis = fd->c[AxisHeight], accent = fd->c[AccentBaseHeight],
                                flat = fd->c[FlattenedAccentBaseHeight];
                    for (int i = 0; i < MC_COUNT; ++i)
                        fd->c[i] = defaults.c[i];
                    fd->c[AxisHeight] = axis;
                    fd->c[AccentBaseHeight] = accent;
                    fd->c[FlattenedAccentBaseHeight] = flat;
                    fd->scriptScale = defaults.scriptScale;
                    fd->scriptScriptScale = defaults.scriptScriptScale;
                    fd->partialMathTable = true;
                }
            }
            if (gi) {
                italics = readGlyphValues(r, gi + r.u16(gi), upem);
                accents = readGlyphValues(r, gi + r.u16(gi + 2), upem);
            }
            if (mv) {
                mvOff = mv;
                fd->minConnectorOverlap = r.u16(mv) / upem;
                vertCov = readCoverage(r, mv + r.u16(mv + 2));
                horizCov = readCoverage(r, mv + r.u16(mv + 4));
                vertCount = r.u16(mv + 6);
                horizCount = r.u16(mv + 8);
            }
            fd->hasMathTable = r.ok && mc != 0;
        }

        // Letters: italic correction + top accent attachment by code point.
        QList<char32_t> letters;
        addRange(letters, 'A', 'Z');
        addRange(letters, 'a', 'z');
        addRange(letters, '0', '9');
        addRange(letters, 0x391, 0x3A9);
        addRange(letters, 0x3B1, 0x3C9);
        for (char32_t c : {0x131u, 0x237u, 0x3D1u, 0x3D5u, 0x3D6u, 0x3F0u, 0x3F1u, 0x3F5u,
                           0x2202u, 0x2207u, 0x221Eu, 0x2113u, 0x210Fu, 0x2118u})
            letters.push_back(c);
        addRange(letters, 0x2102, 0x2134);
        addRange(letters, 0x1D400, 0x1D7FF);
        for (char32_t c : kShapeChars)
            letters.push_back(c);

        QString all;
        QList<int> unitIndex;
        for (char32_t c : letters) {
            unitIndex.push_back(all.size());
            all += QString::fromUcs4(&c, 1);
        }
        // One glyph per code point (Qt returns per-code-point indices; be
        // tolerant of a per-UTF-16-unit result too).
        const QList<quint32> gids = rf.glyphIndexesForString(all);
        const bool perUnit = gids.size() == all.size() && all.size() != letters.size();
        for (int i = 0; i < letters.size(); ++i) {
            const int ui = perUnit ? unitIndex[i] : i;
            const quint32 g = ui < gids.size() ? gids[ui] : 0;
            if (!g)
                continue;
            const char32_t c = letters[i];
            fd->gidOfChar[c] = g;
            auto it = italics.find(g);
            if (it != italics.end())
                fd->italicOfChar[c] = it->second;
            auto at = accents.find(g);
            if (at != accents.end())
                fd->accentOfChar[c] = at->second;
        }
        fd->hasMathAlphanumerics = fd->gid(0x1D44E) != 0 && fd->gid(0x1D400) != 0;

        const QTransform toEm = QTransform::fromScale(1.0 / upem, 1.0 / upem);
        auto ensureShape = [&](quint32 g) {
            if (!g || fd->shapes.count(g))
                return;
            GlyphShape s;
            QPainterPath p = rf.pathForGlyph(g);
            p.setFillRule(Qt::WindingFill);
            s.path = toEm.map(p);
            s.path.setFillRule(Qt::WindingFill);
            s.ink = s.path.boundingRect();
            const QList<QPointF> adv = rf.advancesForGlyphIndexes(QList<quint32>{g});
            s.adv = adv.isEmpty() ? s.ink.right() : adv.first().x() / upem;
            auto it = italics.find(g);
            if (it != italics.end())
                s.italic = it->second;
            auto at = accents.find(g);
            if (at != accents.end())
                s.topAccent = at->second;
            fd->shapes.emplace(g, std::move(s));
        };

        for (char32_t c : kShapeChars) {
            const quint32 g = fd->gid(c);
            if (!g)
                continue;
            ensureShape(g);
            if (!fd->hasMathTable || !mvOff)
                continue;
            auto vi = vertCov.find(g);
            if (vi != vertCov.end() && vi->second < vertCount) {
                const int off = r.u16(mvOff + 10 + 2 * vi->second);
                if (off) {
                    GlyphConstruction gc = readConstruction(r, mvOff + off, upem);
                    for (const auto &v : gc.variants)
                        ensureShape(v.gid);
                    for (const auto &p : gc.parts)
                        ensureShape(p.gid);
                    fd->vertical.emplace(g, std::move(gc));
                }
            }
            auto hi = horizCov.find(g);
            if (hi != horizCov.end() && hi->second < horizCount) {
                const int off = r.u16(mvOff + 10 + 2 * vertCount + 2 * hi->second);
                if (off) {
                    GlyphConstruction gc = readConstruction(r, mvOff + off, upem);
                    for (const auto &v : gc.variants)
                        ensureShape(v.gid);
                    for (const auto &p : gc.parts)
                        ensureShape(p.gid);
                    fd->horizontal.emplace(g, std::move(gc));
                }
            }
        }
        if (!r.ok) {
            // Truncated/corrupt table: fall back to synthetic constructions.
            fd->hasMathTable = false;
            fd->vertical.clear();
            fd->horizontal.clear();
            setDefaultConstants(*fd);
        }
    } else {
        setDefaultConstants(*fd);
    }
    fd->initMicros = timer.nsecsElapsed() / 1000;
    return fd;
}

std::atomic<FontData *> g_fontData{nullptr};
std::mutex g_fontInitMutex;

// ------------------------------------------------------------ metrics cache --
struct MKey {
    int v;
    QString s;
    bool operator==(const MKey &o) const { return v == o.v && s == o.s; }
};
struct MKeyHash {
    size_t operator()(const MKey &k) const { return qHash(k.s, size_t(k.v) * 0x9E3779B9u); }
};
std::shared_mutex g_metricsMutex;
std::unordered_map<MKey, GlyphMetrics, MKeyHash> *g_metrics =
    new std::unordered_map<MKey, GlyphMetrics, MKeyHash>(); // leaked on purpose
constexpr size_t kMaxMetricsEntries = 20000;
constexpr int kMaxCachedTextLength = 24;

} // namespace

const FontData *fontData()
{
    FontData *p = g_fontData.load(std::memory_order_acquire);
    if (p)
        return p;
    std::lock_guard<std::mutex> lock(g_fontInitMutex);
    p = g_fontData.load(std::memory_order_relaxed);
    if (p)
        return p;
    if (!qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        return nullptr; // fonts need the platform font database
    p = buildFontData();
    g_fontData.store(p, std::memory_order_release);
    return p;
}

QFont makeFont(const FontData &fd, int variant, qreal px)
{
    const FontRole role = variantRole(variant);
    QFont f(fd.family(role));
    // Pixel size (not points): the display list is in logical pixels and
    // must not depend on the DPI of the device it is painted on.
    f.setPixelSize(qMax(1, qRound(px)));
    if (variantItalic(variant))
        f.setItalic(true);
    if (variantBold(variant))
        f.setWeight(QFont::Bold);
    if (role == RoleMono)
        f.setStyleHint(QFont::Monospace);
    else if (role == RoleSans)
        f.setStyleHint(QFont::SansSerif);
    f.setKerning(false);
    f.setHintingPreference(QFont::PreferNoHinting);
    return f;
}

GlyphMetrics measure(const FontData &fd, int variant, const QString &text, qint64 *missNanos)
{
    MKey key{variant, text};
    {
        std::shared_lock<std::shared_mutex> lock(g_metricsMutex);
        auto it = g_metrics->find(key);
        if (it != g_metrics->end())
            return it->second;
    }
    QElapsedTimer missTimer;
    missTimer.start();
    const QFont f = makeFont(fd, variant, kRefPx);
    const QFontMetricsF fm(f);
    GlyphMetrics m;
    m.adv = fm.horizontalAdvance(text) / kRefPx;
    const QList<uint> ucs = text.toUcs4();
    if (ucs.size() == 1)
        m.inFont = fm.inFontUcs4(ucs.first());
    // Exact ink from glyph outlines (QPainterPath::addText, font fallback
    // included). Engine glyph boxes are not exact: CoreText's
    // tightBoundingRect() pads right/bottom by up to ~1px and FreeType rounds
    // glyph boxes to whole pixels. Multi-character strings are composed from
    // the (cached) single-character boxes laid end to end, so a long \text
    // costs O(n) cache lookups rather than an O(n) outline extraction.
    QRectF r;
    if (ucs.size() == 1) {
        if (!text.trimmed().isEmpty()) {
            QPainterPath outline;
            outline.addText(QPointF(0, 0), f, text);
            r = outline.boundingRect();
            if (r.isEmpty())
                r = fm.tightBoundingRect(text);
        }
    } else if (!ucs.isEmpty()) {
        qreal pen = 0;
        bool any = false;
        qreal x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        for (uint u : ucs) {
            const char32_t c = char32_t(u);
            const GlyphMetrics g = measure(fd, variant, QString::fromUcs4(&c, 1), nullptr);
            if (g.x1 > g.x0 || g.y1 > g.y0) {
                x0 = any ? qMin(x0, pen + g.x0) : pen + g.x0;
                x1 = any ? qMax(x1, pen + g.x1) : pen + g.x1;
                y0 = any ? qMin(y0, g.y0) : g.y0;
                y1 = any ? qMax(y1, g.y1) : g.y1;
                any = true;
            }
            pen += g.adv;
        }
        if (any) {
            // Shaping (ligatures) may change the total advance slightly;
            // stretch the horizontal extent accordingly.
            const qreal k = pen > 0 ? m.adv / pen : 1.0;
            r = QRectF(QPointF(x0 * k * kRefPx, y0 * kRefPx), QPointF(x1 * k * kRefPx, y1 * kRefPx));
        }
    }
    if (r.isValid() && !r.isEmpty()) {
        m.x0 = r.left() / kRefPx;
        m.y0 = r.top() / kRefPx;
        m.x1 = r.right() / kRefPx;
        m.y1 = r.bottom() / kRefPx;
    }
    if (text.size() <= kMaxCachedTextLength) {
        std::unique_lock<std::shared_mutex> lock(g_metricsMutex);
        if (g_metrics->size() < kMaxMetricsEntries)
            g_metrics->emplace(std::move(key), m);
    }
    if (missNanos)
        *missNanos += missTimer.nsecsElapsed();
    return m;
}

} // namespace detail
} // namespace math
} // namespace folia
