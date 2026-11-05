// Internal: math font resolution, OpenType MATH table data and a process-wide
// glyph metrics cache. Not part of the public contract.
//
// Thread-safety: fontData() builds an immutable FontData exactly once (guarded
// by a mutex, published through an atomic pointer) and never frees it. All
// QRawFont work happens inside that one-time build on the calling thread; the
// result holds only plain data and QPainterPaths that are never mutated again,
// so any thread may read it. measure() caches plain numbers behind a
// shared_mutex and creates its QFont/QFontMetricsF locally per miss.
#pragma once

#include <QFont>
#include <QPainterPath>
#include <QRectF>
#include <QString>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace folia {
namespace math {
namespace detail {

// Font variant id: bit0 bold, bit1 italic, bits2-3 role.
enum FontRole : int { RoleMath = 0, RoleText = 1, RoleSans = 2, RoleMono = 3 };
inline int fontVariant(FontRole r, bool italic = false, bool bold = false)
{
    return (int(r) << 2) | (italic ? 2 : 0) | (bold ? 1 : 0);
}
inline FontRole variantRole(int v) { return FontRole((v >> 2) & 3); }
inline bool variantItalic(int v) { return v & 2; }
inline bool variantBold(int v) { return v & 1; }

// OpenType MATH constants (MathValueRecord block), same order as the spec.
enum MC {
    MathLeading, AxisHeight, AccentBaseHeight, FlattenedAccentBaseHeight,
    SubscriptShiftDown, SubscriptTopMax, SubscriptBaselineDropMin,
    SuperscriptShiftUp, SuperscriptShiftUpCramped, SuperscriptBottomMin,
    SuperscriptBaselineDropMax, SubSuperscriptGapMin,
    SuperscriptBottomMaxWithSubscript, SpaceAfterScript, UpperLimitGapMin,
    UpperLimitBaselineRiseMin, LowerLimitGapMin, LowerLimitBaselineDropMin,
    StackTopShiftUp, StackTopDisplayStyleShiftUp, StackBottomShiftDown,
    StackBottomDisplayStyleShiftDown, StackGapMin, StackDisplayStyleGapMin,
    StretchStackTopShiftUp, StretchStackBottomShiftDown,
    StretchStackGapAboveMin, StretchStackGapBelowMin, FractionNumeratorShiftUp,
    FractionNumeratorDisplayStyleShiftUp, FractionDenominatorShiftDown,
    FractionDenominatorDisplayStyleShiftDown, FractionNumeratorGapMin,
    FractionNumDisplayStyleGapMin, FractionRuleThickness,
    FractionDenominatorGapMin, FractionDenomDisplayStyleGapMin,
    SkewedFractionHorizontalGap, SkewedFractionVerticalGap, OverbarVerticalGap,
    OverbarRuleThickness, OverbarExtraAscender, UnderbarVerticalGap,
    UnderbarRuleThickness, UnderbarExtraDescender, RadicalVerticalGap,
    RadicalDisplayStyleVerticalGap, RadicalRuleThickness, RadicalExtraAscender,
    RadicalKernBeforeDegree, RadicalKernAfterDegree,
    MC_COUNT
};

// All lengths below are in em units (multiply by the pixel size).
struct GlyphMetrics {
    qreal adv = 0;
    qreal x0 = 0, y0 = 0, x1 = 0, y1 = 0; // ink box, y down, baseline at 0
    bool inFont = true;                   // primary font covers the text
};

struct GlyphShape {
    QPainterPath path;    // outline, em units, baseline origin, y down
    qreal adv = 0;
    QRectF ink;
    qreal italic = 0;
    qreal topAccent = qQNaN();
};

struct GlyphVariant {
    quint32 gid = 0;
    qreal size = 0;
};

struct GlyphPart {
    quint32 gid = 0;
    qreal startConnector = 0, endConnector = 0, fullAdvance = 0;
    bool extender = false;
};

struct GlyphConstruction {
    std::vector<GlyphVariant> variants; // increasing size
    std::vector<GlyphPart> parts;       // bottom->top / left->right; empty if none
};

struct FontData {
    QString mathFamily, textFamily, sansFamily, monoFamily;
    bool hasMathTable = false;
    bool hasMathAlphanumerics = false;
    qreal pxPerPt = 1;      // pixels per point at the default font DPI
    qreal c[MC_COUNT] = {};
    qreal scriptScale = 0.7, scriptScriptScale = 0.5;
    qreal delimitedSubFormulaMinHeight = 1.3, displayOperatorMinHeight = 1.3;
    qreal radicalDegreeBottomRaise = 0.6;
    qreal minConnectorOverlap = 0.02;
    qreal xHeight = 0.45;
    int patchedConstants = 0;      // MATH constants replaced by defaults
    bool partialMathTable = false; // core constants missing: defaults used for all shifts

    std::unordered_map<char32_t, quint32> gidOfChar;
    std::unordered_map<quint32, GlyphShape> shapes;
    std::unordered_map<char32_t, qreal> italicOfChar;
    std::unordered_map<char32_t, qreal> accentOfChar;
    std::unordered_map<quint32, GlyphConstruction> vertical, horizontal;
    qint64 initMicros = 0;

    quint32 gid(char32_t ch) const
    {
        auto it = gidOfChar.find(ch);
        return it == gidOfChar.end() ? 0u : it->second;
    }
    const GlyphShape *shape(quint32 g) const
    {
        auto it = shapes.find(g);
        return it == shapes.end() ? nullptr : &it->second;
    }
    const GlyphShape *shapeForChar(char32_t ch) const
    {
        quint32 g = gid(ch);
        return g ? shape(g) : nullptr;
    }
    const GlyphConstruction *verticalFor(char32_t ch) const
    {
        auto it = vertical.find(gid(ch));
        return it == vertical.end() ? nullptr : &it->second;
    }
    const GlyphConstruction *horizontalFor(char32_t ch) const
    {
        auto it = horizontal.find(gid(ch));
        return it == horizontal.end() ? nullptr : &it->second;
    }
    qreal italicCorrection(char32_t ch) const
    {
        auto it = italicOfChar.find(ch);
        return it == italicOfChar.end() ? 0 : it->second;
    }
    qreal topAccent(char32_t ch) const
    {
        auto it = accentOfChar.find(ch);
        return it == accentOfChar.end() ? qQNaN() : it->second;
    }
    const QString &family(FontRole r) const
    {
        switch (r) {
        case RoleText: return textFamily;
        case RoleSans: return sansFamily;
        case RoleMono: return monoFamily;
        default: return mathFamily;
        }
    }
};

// Lazily built, never freed. nullptr when no QGuiApplication exists yet.
const FontData *fontData();
// True once fontData() has been built (never builds it).
bool fontDataReady();

// Cached ink/advance metrics of `text` in `variant`, normalized to 1 em.
// *missNanos (optional) receives the time spent when this was a cache miss.
GlyphMetrics measure(const FontData &fd, int variant, const QString &text,
                     qint64 *missNanos = nullptr);

// A fresh (unshared) QFont for `variant` whose em is `px` pixels.
QFont makeFont(const FontData &fd, int variant, qreal px);

} // namespace detail
} // namespace math
} // namespace folia
