// Renders the math corpus plus inline paragraphs into a PNG for visual review.
//   math_gallery [out.png] [--boxes] [--only <substring>] [--scale <dpr>]
//                [--cols <n>] [--pt <size>] [--tex <formula>]...
// --tex renders the given formulas instead of the built-in corpus.
#include "MathCorpus.h"
#include "math/MathRenderer.h"

#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QStringList>
#include <cstdio>
#include <vector>

#ifndef FOLIA_MATH_GALLERY_DEFAULT
#define FOLIA_MATH_GALLERY_DEFAULT "math-gallery.png"
#endif

using namespace folia;

namespace {

struct Cell {
    QString label;
    math::Result result;
};

// Inline paragraph: text with $...$ formulas laid out on one baseline.
struct InlinePiece {
    bool math;
    QString text;
    math::Result result;
};

std::vector<InlinePiece> parseInline(const QString &para, qreal pt)
{
    std::vector<InlinePiece> out;
    const QStringList parts = para.split(QLatin1Char('$'));
    for (int i = 0; i < parts.size(); ++i) {
        InlinePiece p;
        p.math = i % 2 == 1;
        p.text = parts[i];
        if (p.math) {
            math::Style st;
            st.pointSize = pt;
            st.display = false;
            p.result = math::render(p.text, st);
        }
        out.push_back(std::move(p));
    }
    return out;
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QString outPath = QString::fromUtf8(FOLIA_MATH_GALLERY_DEFAULT);
    bool boxes = false;
    QString only;
    qreal dpr = 2.0;
    int cols = 2;
    qreal pt = 18;
    QStringList custom;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString &a = args[i];
        if (a == QLatin1String("--boxes"))
            boxes = true;
        else if (a == QLatin1String("--only") && i + 1 < args.size())
            only = args[++i];
        else if (a == QLatin1String("--scale") && i + 1 < args.size())
            dpr = args[++i].toDouble();
        else if (a == QLatin1String("--cols") && i + 1 < args.size())
            cols = qMax(1, args[++i].toInt());
        else if (a == QLatin1String("--pt") && i + 1 < args.size())
            pt = args[++i].toDouble();
        else if (a == QLatin1String("--tex") && i + 1 < args.size())
            custom << args[++i];
        else if (!a.startsWith(QLatin1String("--")))
            outPath = a;
    }

    std::vector<Cell> cells;
    const int count = custom.isEmpty() ? kMathCorpusSize : int(custom.size());
    for (int i = 0; i < count; ++i) {
        const QString tex = custom.isEmpty() ? QString::fromUtf8(kMathCorpus[i].tex) : custom[i];
        const QString label = custom.isEmpty() ? QString::fromUtf8(kMathCorpus[i].label) : tex;
        if (!only.isEmpty() && !label.contains(only) && !tex.contains(only))
            continue;
        math::Style st;
        st.pointSize = pt;
        st.display = true;
        Cell c;
        c.label = tex;
        c.result = math::render(tex, st);
        if (!c.result.ok)
            std::fprintf(stderr, "FAILED [%s]: %s (offset %d)\n", qPrintable(label),
                         qPrintable(c.result.error), c.result.errorOffset);
        cells.push_back(std::move(c));
    }

    const QStringList paragraphs = {
        QStringLiteral("Energy $E=mc^2$, ratio $\\frac{a}{b}$ and sum $\\sum_{i=1}^n x_i$ inline."),
        QStringLiteral("Roots $x_{1,2} = \\frac{-b \\pm \\sqrt{b^2-4ac}}{2a}$, integral $\\int_0^1 f(x)\\,dx$, "
                       "set $\\{x \\in \\mathbb{R} : x > 0\\}$ and $\\left(\\frac{1}{2}\\right)^n$."),
        QStringLiteral("Matrix $\\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}$ inline, limit "
                       "$\\lim_{n\\to\\infty} (1+\\frac{1}{n})^n = e$, vector $\\vec{v}$ and $\\hat{x}$."),
    };
    const qreal textPt = pt * 0.8;
    QFont textFont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    textFont.setPointSizeF(textPt);
    QFont labelFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    labelFont.setPointSizeF(9.5);
    const QFontMetricsF lfm(labelFont), tfm(textFont);

    const qreal margin = 24, colW = cols == 1 ? 1100 : 820, gapY = 14;
    // Column layout
    std::vector<qreal> colH(size_t(cols), margin);
    struct Placed {
        size_t cell;
        int col;
        qreal y;
    };
    std::vector<Placed> placed;
    for (size_t i = 0; i < cells.size(); ++i) {
        int best = 0;
        for (int c = 1; c < cols; ++c)
            if (colH[size_t(c)] < colH[size_t(best)] - 1)
                best = c;
        const math::Result &r = cells[i].result;
        const qreal h = lfm.height() + 6 + (r.ok ? r.list.height() : 24) + gapY;
        placed.push_back({i, best, colH[size_t(best)]});
        colH[size_t(best)] += h;
    }
    qreal y0 = 0;
    for (qreal h : colH)
        y0 = qMax(y0, h);
    // Inline paragraphs below the corpus
    std::vector<std::vector<InlinePiece>> paras;
    for (const QString &p : paragraphs)
        paras.push_back(parseInline(p, textPt));
    qreal parasH = 0;
    std::vector<qreal> lineA, lineD;
    for (const auto &pieces : paras) {
        qreal a = tfm.ascent(), d = tfm.descent();
        for (const InlinePiece &p : pieces) {
            if (p.math && p.result.ok) {
                a = qMax(a, p.result.list.ascent);
                d = qMax(d, p.result.list.descent);
            }
        }
        lineA.push_back(a);
        lineD.push_back(d);
        parasH += a + d + 10;
    }
    const qreal W = margin * 2 + colW * cols;
    const qreal H = y0 + lfm.height() + 10 + parasH + margin;

    QImage img(QSize(qCeil(W * dpr), qCeil(H * dpr)), QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    const QColor ink(0x1b, 0x1b, 0x1f);
    for (const Placed &pl : placed) {
        const Cell &c = cells[pl.cell];
        const qreal x = margin + pl.col * colW;
        qreal y = pl.y;
        p.setFont(labelFont);
        p.setPen(QColor(0x80, 0x80, 0x88));
        const QString label = lfm.elidedText(c.label, Qt::ElideRight, colW - 16);
        p.drawText(QPointF(x, y + lfm.ascent()), label);
        y += lfm.height() + 6;
        if (!c.result.ok) {
            p.setPen(Qt::red);
            p.drawText(QPointF(x + 12, y + lfm.ascent()), c.result.error);
            continue;
        }
        const math::Result &r = c.result;
        const QPointF origin(x + 12, y + r.list.ascent);
        if (boxes) {
            p.setPen(QPen(QColor(120, 170, 255), 0));
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(origin.x(), origin.y() - r.list.ascent, r.list.width, r.list.height()));
            p.setPen(QPen(QColor(255, 140, 140), 0));
            p.drawLine(QPointF(origin.x(), origin.y()), QPointF(origin.x() + r.list.width, origin.y()));
        }
        r.list.paint(&p, origin, ink);
    }
    // inline paragraphs
    qreal y = y0 + 4;
    p.setFont(labelFont);
    p.setPen(QColor(0x80, 0x80, 0x88));
    p.drawText(QPointF(margin, y + lfm.ascent()),
               QStringLiteral("inline (display=false) on a %1 pt system-font baseline:").arg(textPt));
    y += lfm.height() + 10;
    for (size_t i = 0; i < paras.size(); ++i) {
        const qreal baseline = y + lineA[i];
        qreal x = margin;
        if (boxes) {
            p.setPen(QPen(QColor(255, 140, 140), 0));
            p.drawLine(QPointF(margin, baseline), QPointF(W - margin, baseline));
        }
        for (const InlinePiece &piece : paras[i]) {
            if (!piece.math) {
                p.setFont(textFont);
                p.setPen(ink);
                p.drawText(QPointF(x, baseline), piece.text);
                x += tfm.horizontalAdvance(piece.text);
            } else if (piece.result.ok) {
                if (boxes) {
                    p.setPen(QPen(QColor(120, 170, 255), 0));
                    p.setBrush(Qt::NoBrush);
                    p.drawRect(QRectF(x, baseline - piece.result.list.ascent, piece.result.list.width,
                                      piece.result.list.height()));
                }
                piece.result.list.paint(&p, QPointF(x, baseline), ink);
                x += piece.result.list.width;
            } else {
                p.setPen(Qt::red);
                p.setFont(textFont);
                p.drawText(QPointF(x, baseline), piece.text);
                x += tfm.horizontalAdvance(piece.text);
                std::fprintf(stderr, "inline FAILED: %s\n", qPrintable(piece.result.error));
            }
        }
        y += lineA[i] + lineD[i] + 10;
    }
    p.end();
    if (!img.save(outPath)) {
        std::fprintf(stderr, "could not write %s\n", qPrintable(outPath));
        return 1;
    }
    std::printf("wrote %s (%dx%d px, dpr %.1f, math font: %s)\n", qPrintable(outPath), img.width(),
                img.height(), dpr, qPrintable(math::resolvedFontFamily()));
    return 0;
}
