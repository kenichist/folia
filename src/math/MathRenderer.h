// Local native TeX-subset renderer (no browser engine, no external process).
//
// Contract (frozen v1):
//  * render() is pure and thread-safe: it may run on any thread, uses only
//    QFont/QFontMetricsF/QPainterPath, and never touches widgets.
//  * Output is a resolution-independent display list positioned relative to
//    the formula baseline (y = 0, positive y goes down, ascent above baseline).
//  * Errors never throw: Result.ok == false carries a message and the source
//    offset so the editor can keep the original source editable.
//  * Limits bound pathological input (length, nesting, node count, time).
//    `cancel` is polled; when it becomes true render() returns quickly with
//    ok=false, error="cancelled".
// The supported subset is documented in docs/MATH_SUBSET.md.
#pragma once

#include <QColor>
#include <QFont>
#include <QLineF>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>
#include <atomic>

class QPainter;

namespace folia {

class MathDisplayList {
public:
    struct GlyphRun {
        QPointF pos;      // baseline origin of the run
        int font = 0;     // index into fonts
        QString text;
    };
    struct Rule {         // filled rectangle (fraction bars, overlines, sqrt vinculum)
        QRectF rect;
    };

    QVector<QFont> fonts;
    QVector<GlyphRun> runs;
    QVector<Rule> rules;
    QVector<QPainterPath> paths; // filled paths (radical signs, stretchy delimiters)

    qreal width = 0;
    qreal ascent = 0;   // distance above baseline (>= 0)
    qreal descent = 0;  // distance below baseline (>= 0)

    qreal height() const { return ascent + descent; }
    // Approximate heap bytes held (used for byte-capped caches).
    qint64 byteSize() const;
    // Paint with the baseline origin at `origin`.
    void paint(QPainter *p, const QPointF &origin, const QColor &color) const;
};

namespace math {

struct Style {
    qreal pointSize = 14;
    bool display = true;      // display style (\displaystyle) vs inline (\textstyle)
};

struct Limits {
    int maxSourceChars = 8192;
    int maxDepth = 48;        // group / environment nesting
    int maxNodes = 6000;      // atoms after macro expansion
    int maxMillis = 40;       // wall-clock budget per render
};

struct Result {
    bool ok = false;
    QString error;
    int errorOffset = -1;     // offset in source, -1 if unknown
    MathDisplayList list;     // valid when ok
    int nodeCount = 0;
};

Result render(const QString &tex, const Style &style, const Limits &limits = {},
              const std::atomic<bool> *cancel = nullptr);

// Font family actually used for math on this system (for the release report).
QString resolvedFontFamily();

} // namespace math
} // namespace folia
