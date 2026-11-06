#include "app/PaneSplitter.h"

#include <QMouseEvent>
#include <QPainter>
#include <QSplitterHandle>

#include <cmath>

namespace folia {

// A 1 px line with a 5 px grab area (QSplitter's "tiny mode" for handle
// widths under 5: the handle overlaps its neighbours, only the line is
// painted). It repaints only when the theme changes; dragging moves it.
class PaneSplitterHandle : public QSplitterHandle {
public:
    PaneSplitterHandle(Qt::Orientation o, PaneSplitter *parent) : QSplitterHandle(o, parent), m_split(parent)
    {
        setAccessibleName(tr("Split divider"));
        setToolTip(tr("Drag to resize; double-click for equal widths"));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(contentsRect(), m_split->lineColor());
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        QSplitterHandle::mouseReleaseEvent(e);
        if (e->button() == Qt::LeftButton)
            m_split->released();
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        m_split->setRatio(0.5);
        emit m_split->ratioCommitted(m_split->ratio());
    }

private:
    PaneSplitter *m_split;
};

PaneSplitter::PaneSplitter(QWidget *primary, QWidget *parent) : QSplitter(Qt::Horizontal, parent), m_primary(primary)
{
    setObjectName(QStringLiteral("folia-pane-splitter"));
    setChildrenCollapsible(false); // never lay a pane out at a degenerate width
    setOpaqueResize(true);         // live, one step per (coalesced) mouse move
    setHandleWidth(1);
    setFrameShape(QFrame::NoFrame);
    addWidget(primary);
}

QSplitterHandle *PaneSplitter::createHandle()
{
    return new PaneSplitterHandle(orientation(), this);
}

QWidget *PaneSplitter::setSecondary(QWidget *w)
{
    if (w == m_secondary)
        return nullptr;
    QWidget *old = takeSecondary();
    if (!w)
        return old;
    m_secondary = w;
    w->setMinimumWidth(kMinPaneWidth);
    m_primary->setMinimumWidth(kMinPaneWidth);
    insertWidget(m_side == Side::Left ? 0 : 1, w);
    setCollapsible(0, false);
    setCollapsible(1, false);
    applyRatio();
    return old;
}

QWidget *PaneSplitter::takeSecondary()
{
    QWidget *w = m_secondary;
    if (!w)
        return nullptr;
    m_secondary = nullptr;
    w->hide();
    w->setParent(nullptr); // out of the splitter (its handle goes with it)
    m_primary->setMinimumWidth(0);
    return w;
}

void PaneSplitter::setSide(Side s)
{
    if (s == m_side)
        return;
    m_side = s;
    if (!m_secondary)
        return;
    // Moving a child within the splitter keeps its parent: the editor is
    // never re-parented, so an IME composition or the caret survive a swap.
    insertWidget(s == Side::Left ? 0 : 1, m_secondary);
    applyRatio();
}

double PaneSplitter::ratio() const
{
    return m_ratio;
}

void PaneSplitter::setRatio(double r)
{
    if (!std::isfinite(r))
        r = 0.5;
    m_ratio = qBound(kMinRatio, r, kMaxRatio);
    applyRatio();
}

void PaneSplitter::setLineColor(const QColor &c)
{
    if (c == m_line || !c.isValid())
        return;
    m_line = c;
    for (int i = 0; i < count(); ++i)
        if (QSplitterHandle *h = handle(i))
            h->update();
}

void PaneSplitter::applyRatio()
{
    if (!m_secondary)
        return;
    const int avail = width() - handleWidth();
    if (avail <= 0)
        return; // not laid out yet: applied on the first resize
    int second = int(std::lround(avail * m_ratio));
    if (avail >= 2 * kMinPaneWidth)
        second = qBound(kMinPaneWidth, second, avail - kMinPaneWidth);
    const int first = avail - second;
    if (m_side == Side::Left)
        setSizes({second, first});
    else
        setSizes({first, second});
}

void PaneSplitter::resizeEvent(QResizeEvent *e)
{
    if (m_secondary) {
        // One distribution per resize, from the user's ratio (QSplitter's own
        // proportional rescale would drift after the window was squeezed to
        // the panes' minimum widths, and a second pass would resize both
        // panes twice per step).
        QWidget::resizeEvent(e);
        applyRatio();
        return;
    }
    QSplitter::resizeEvent(e);
}

void PaneSplitter::released()
{
    if (!m_secondary)
        return;
    const QList<int> s = sizes();
    if (s.size() != 2 || s[0] + s[1] <= 0)
        return;
    const int second = m_side == Side::Left ? s[0] : s[1];
    m_ratio = qBound(kMinRatio, double(second) / double(s[0] + s[1]), kMaxRatio);
    emit ratioCommitted(m_ratio);
}

} // namespace folia
