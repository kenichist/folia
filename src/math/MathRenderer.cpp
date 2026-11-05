// Public entry points of the native math renderer (see MathRenderer.h).
#include "math/MathRenderer.h"

#include "math/MathAst.h"
#include "math/MathFont.h"
#include "math/MathLayout.h"

#include <QPainter>

namespace folia {

qint64 MathDisplayList::byteSize() const
{
    // Heap estimate: container storage + per-element payloads. QFont private
    // data is ~300 bytes; a path element is {x, y, type} = 24 bytes.
    qint64 s = qint64(sizeof(*this));
    s += qint64(fonts.capacity()) * qint64(sizeof(QFont)) + qint64(fonts.size()) * 320;
    s += qint64(runs.capacity()) * qint64(sizeof(GlyphRun));
    for (const GlyphRun &r : runs)
        s += 32 + qint64(r.text.capacity()) * 2;
    s += qint64(rules.capacity()) * qint64(sizeof(Rule));
    s += qint64(paths.capacity()) * qint64(sizeof(QPainterPath));
    for (const QPainterPath &p : paths)
        s += 96 + qint64(p.elementCount()) * 24;
    return s;
}

void MathDisplayList::paint(QPainter *p, const QPointF &origin, const QColor &color) const
{
    if (!p || (runs.isEmpty() && rules.isEmpty() && paths.isEmpty()))
        return;
    p->save();
    p->translate(origin);
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setRenderHint(QPainter::TextAntialiasing, true);
    if (!rules.isEmpty() || !paths.isEmpty()) {
        p->setPen(Qt::NoPen);
        p->setBrush(color);
        for (const Rule &r : rules)
            p->drawRect(r.rect);
        for (const QPainterPath &path : paths)
            p->drawPath(path);
    }
    if (!runs.isEmpty()) {
        p->setPen(color);
        p->setBrush(Qt::NoBrush);
        int current = -1;
        for (const GlyphRun &r : runs) {
            if (r.font < 0 || r.font >= fonts.size())
                continue;
            if (r.font != current) {
                p->setFont(fonts[r.font]);
                current = r.font;
            }
            p->drawText(r.pos, r.text);
        }
    }
    p->restore();
}

namespace math {

Result render(const QString &tex, const Style &style, const Limits &limits,
              const std::atomic<bool> *cancel)
{
    Result r;
    if (cancel && cancel->load(std::memory_order_relaxed)) {
        r.error = QStringLiteral("cancelled");
        return r;
    }
    detail::Ctx ctx;
    ctx.lim = limits;
    // Non-positive limits fall back to the defaults; depth is additionally
    // clamped so recursion stays far below a 512 KiB worker-thread stack.
    const Limits defaults;
    if (ctx.lim.maxSourceChars <= 0)
        ctx.lim.maxSourceChars = defaults.maxSourceChars;
    if (ctx.lim.maxNodes <= 0)
        ctx.lim.maxNodes = defaults.maxNodes;
    if (ctx.lim.maxMillis <= 0)
        ctx.lim.maxMillis = defaults.maxMillis;
    ctx.maxDepth = qBound(1, ctx.lim.maxDepth <= 0 ? defaults.maxDepth : ctx.lim.maxDepth, 100);
    ctx.cancel = cancel;

    if (tex.size() > ctx.lim.maxSourceChars) {
        r.error = QStringLiteral("Formula too long (%1 characters; limit %2)")
                      .arg(tex.size())
                      .arg(ctx.lim.maxSourceChars);
        r.errorOffset = ctx.lim.maxSourceChars;
        return r;
    }
    if (!(style.pointSize > 0.5 && style.pointSize < 2000)) {
        r.error = QStringLiteral("Invalid point size");
        return r;
    }
    // Font resolution happens once per process and is not charged to the
    // per-formula time budget.
    const detail::FontData *fd = detail::fontData();
    if (!fd) {
        r.error = QStringLiteral("Math fonts unavailable (no QGuiApplication)");
        return r;
    }
    ctx.timer.start();

    detail::Ast ast;
    ast.nodes.reserve(size_t(qMin<qsizetype>(tex.size() + 4, 512)));
    detail::parse(tex, ast, ctx);
    r.nodeCount = int(ast.nodes.size());
    if (ctx.failed) {
        r.error = ctx.error;
        r.errorOffset = ctx.offset;
        return r;
    }
    if (!detail::layout(*fd, ast, ctx, style.pointSize * fd->pxPerPt, style.display, r.list)) {
        r.error = ctx.error;
        r.errorOffset = ctx.offset;
        r.list = MathDisplayList();
        return r;
    }
    r.ok = true;
    return r;
}

QString resolvedFontFamily()
{
    const detail::FontData *fd = detail::fontData();
    return fd ? fd->mathFamily : QString();
}

bool ready() { return detail::fontDataReady(); }

} // namespace math
} // namespace folia
