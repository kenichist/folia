#include "shell/TabStrip.h"

#include "core/Instrument.h"
#include "core/LucideIcons.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QHash>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <QWheelEvent>

namespace folia {

namespace {
constexpr int kNewSlot = 40;   // room kept right of the tabs for "+"
constexpr int kNewSize = 28;
constexpr int kTop = 6;        // tabs start this far below the strip's top
constexpr int kRadius = 8;
constexpr int kFlare = 6;      // the active tab's outward curves at the bottom
constexpr int kClose = 16;
constexpr int kPad = 12;
} // namespace

bool TabStrip::Appearance::operator==(const Appearance &o) const
{
    return bg == o.bg && activeBg == o.activeBg && text == o.text && dim == o.dim && hoverBg == o.hoverBg
        && border == o.border && minTabWidth == o.minTabWidth && maxTabWidth == o.maxTabWidth && height == o.height;
}

TabStrip::TabStrip(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("tabs"));
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setAccessibleName(tr("Tabs"));
}

QSize TabStrip::sizeHint() const
{
    return {tabsLeft() + 2 * m_look.minTabWidth + kNewSlot, m_look.height};
}

QSize TabStrip::minimumSizeHint() const
{
    return {tabsLeft() + m_look.minTabWidth + kNewSlot, m_look.height};
}

// ------------------------------------------------------------ model ----
void TabStrip::setTabs(const QVector<Item> &items, int active)
{
    if (items == m_items && active == m_active)
        return;
    const bool grew = items.size() > m_items.size();
    const bool activeChanged = active != m_active
        || (active >= 0 && active < items.size() && (m_active < 0 || m_active >= m_items.size()
                                                     || items[active].uid != m_items[m_active].uid));
    // Elisions are kept for tabs whose title did not change.
    QHash<quint32, int> was;
    for (int i = 0; i < m_items.size(); ++i)
        was.insert(m_items[i].uid, i);
    QVector<Slot> kept(items.size());
    for (int i = 0; i < items.size(); ++i) {
        const int j = was.value(items[i].uid, -1);
        if (j >= 0 && m_items[j].title == items[i].title)
            kept[i] = m_slots[j];
    }
    m_items = items;
    m_slots = kept;
    m_active = items.isEmpty() ? -1 : qBound(0, active, int(items.size()) - 1);
    if (m_dragUid && std::none_of(m_items.cbegin(), m_items.cend(), [this](const Item &it) { return it.uid == m_dragUid; }))
        m_dragUid = 0;
    if (grew)
        m_frozenWidth = 0; // a new tab: widths fit the strip again
    m_hover = -1;
    m_hoverPart = Part::None;
    if (underMouse()) {
        const QPoint p = mapFromGlobal(QCursor::pos());
        const int i = tabAt(p);
        setHover(i, i < 0 ? (newTabRect().contains(p) ? Part::New : Part::None)
                          : closeRect(i).contains(p) ? Part::Close : Part::Tab);
    }
    clampScroll();
    if (activeChanged)
        ensureVisible(m_active);
    applyVisible();
    update();
}

void TabStrip::setTitle(quint32 uid, const QString &title)
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].uid != uid)
            continue;
        if (m_items[i].title == title)
            return;
        m_items[i].title = title;
        m_slots[i] = Slot();
        updateTab(i);
        return;
    }
}

void TabStrip::setAppearance(const Appearance &a)
{
    if (a == m_look)
        return;
    const bool resized = a.height != m_look.height;
    m_look = a;
    for (Slot &s : m_slots)
        s = Slot();
    if (resized)
        updateGeometry();
    clampScroll();
    update();
}

void TabStrip::setLayoutVisible(bool on)
{
    m_layoutVisible = on;
    applyVisible();
}

void TabStrip::setAutoHide(bool on)
{
    m_autoHide = on;
    applyVisible();
}

void TabStrip::applyVisible()
{
    const bool v = m_layoutVisible && (!m_autoHide || m_items.size() > 1);
    if (v == isHidden())
        setVisible(v);
}

// --------------------------------------------------------- geometry ----
int TabStrip::tabsAreaWidth() const
{
    return qMax(0, width() - tabsLeft() - kNewSlot);
}

int TabStrip::tabWidth() const
{
    if (m_frozenWidth > 0)
        return m_frozenWidth;
    const int n = qMax(1, int(m_items.size()));
    return qBound(m_look.minTabWidth, tabsAreaWidth() / n, qMax(m_look.minTabWidth, m_look.maxTabWidth));
}

int TabStrip::contentWidth() const
{
    return int(m_items.size()) * tabWidth();
}

QRect TabStrip::tabRect(int i) const
{
    if (i < 0 || i >= m_items.size())
        return {};
    const int w = tabWidth();
    return {tabsLeft() + i * w - m_scroll, kTop, w, m_look.height - kTop};
}

bool TabStrip::closeShown(int i) const
{
    return i == m_active || (i == m_hover && tabWidth() >= 90);
}

QRect TabStrip::closeRect(int i) const
{
    const QRect r = tabRect(i);
    if (r.isNull())
        return {};
    return {r.right() - kPad + 4 - kClose, r.top() + (r.height() - kClose) / 2, kClose, kClose};
}

QRect TabStrip::newTabRect() const
{
    const int y = kTop / 2 + (m_look.height - kNewSize) / 2;
    if (contentWidth() > tabsAreaWidth())
        return {width() - kNewSlot + (kNewSlot - kNewSize) / 2, y, kNewSize, kNewSize};
    return {tabsLeft() + contentWidth() + 4, y, kNewSize, kNewSize};
}

int TabStrip::tabAt(const QPoint &p) const
{
    if (m_items.isEmpty() || p.y() < 0 || p.y() >= height())
        return -1;
    if (contentWidth() > tabsAreaWidth() && p.x() >= tabsLeft() + tabsAreaWidth())
        return -1; // the "+" slot
    const int x = p.x() - tabsLeft() + m_scroll;
    if (x < 0)
        return -1;
    const int i = x / tabWidth();
    return i < m_items.size() ? i : -1;
}

void TabStrip::clampScroll()
{
    m_scroll = qBound(0, m_scroll, qMax(0, contentWidth() - tabsAreaWidth()));
}

void TabStrip::ensureVisible(int i)
{
    if (i < 0 || contentWidth() <= tabsAreaWidth())
        return;
    const int w = tabWidth();
    const int left = i * w;
    if (left < m_scroll)
        m_scroll = left;
    else if (left + w > m_scroll + tabsAreaWidth())
        m_scroll = left + w - tabsAreaWidth();
    clampScroll();
}

void TabStrip::updateTab(int i)
{
    const QRect r = tabRect(i);
    if (!r.isNull())
        update(r.adjusted(-kFlare - 1, -1, kFlare + 1, 1));
}

void TabStrip::setHover(int index, Part part)
{
    if (index == m_hover && part == m_hoverPart)
        return;
    const int old = m_hover;
    const bool newWas = m_hoverPart == Part::New;
    m_hover = index;
    m_hoverPart = part;
    // A tab's hover also hides the separators beside it: repaint a little wider.
    if (old >= 0)
        updateTab(old);
    if (index >= 0)
        updateTab(index);
    if (newWas != (part == Part::New))
        update(newTabRect().adjusted(-1, -1, 1, 1));
}

QString TabStrip::elidedTitle(int i, int textWidth)
{
    Slot &s = m_slots[i];
    if (s.elidedWidth != textWidth) {
        const QString t = m_items[i].title.isEmpty() ? tr("Untitled") : m_items[i].title;
        s.elided = fontMetrics().elidedText(t, Qt::ElideRight, qMax(0, textWidth));
        s.elidedWidth = textWidth;
    }
    return s.elided;
}

// ------------------------------------------------------------ paint ----
void TabStrip::paintEvent(QPaintEvent *e)
{
    ++m_paints;
    instr::add(instr::PaintEvents);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const int h = m_look.height;
    p.fillRect(e->rect(), m_look.bg);
    p.setPen(QPen(m_look.border, 1));
    p.drawLine(QPointF(0, h - 0.5), QPointF(width(), h - 0.5));

    const bool overflow = contentWidth() > tabsAreaWidth();
    if (overflow)
        p.setClipRect(QRect(0, 0, tabsLeft() + tabsAreaWidth(), h));

    const int dragIndex = [this] {
        for (int i = 0; i < m_items.size(); ++i)
            if (m_items[i].uid == m_dragUid)
                return i;
        return -1;
    }();
    auto drawLabel = [&](int i, const QRect &r, bool active) {
        const bool close = closeShown(i) || i == dragIndex;
        const int textW = r.width() - 2 * kPad - (close ? kClose + 2 : 0);
        p.setPen(active ? m_look.text : m_look.dim);
        p.drawText(QRect(r.left() + kPad, r.top(), qMax(0, textW), r.height() - 2), Qt::AlignVCenter | Qt::AlignLeft,
                   elidedTitle(i, textW));
        if (!close)
            return;
        const QRect c = QRect(r.right() - kPad + 4 - kClose, r.top() + (r.height() - kClose) / 2, kClose, kClose);
        if (i == m_hover && m_hoverPart == Part::Close && m_dragUid == 0) {
            p.setPen(Qt::NoPen);
            p.setBrush(m_look.hoverBg);
            p.drawEllipse(c);
        }
        icons::paint(&p, QStringLiteral("x"), QRectF(c).adjusted(2, 2, -2, -2), active ? m_look.text : m_look.dim, 2.2);
    };
    auto activePath = [&](const QRect &r) {
        const qreal l = r.left() + 0.5, rt = r.right() + 0.5, t = r.top() + 0.5, b = h;
        QPainterPath path(QPointF(l - kFlare, b));
        path.quadTo(QPointF(l, b), QPointF(l, b - kFlare));
        path.lineTo(l, t + kRadius);
        path.quadTo(QPointF(l, t), QPointF(l + kRadius, t));
        path.lineTo(rt - kRadius, t);
        path.quadTo(QPointF(rt, t), QPointF(rt, t + kRadius));
        path.lineTo(rt, b - kFlare);
        path.quadTo(QPointF(rt, b), QPointF(rt + kFlare, b));
        return path;
    };
    auto drawRaised = [&](int i, const QRect &r) {
        const QPainterPath path = activePath(r);
        p.setPen(Qt::NoPen);
        p.setBrush(m_look.activeBg);
        p.drawPath(path);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(m_look.border, 1));
        p.drawPath(path); // the open outline: no line along the bottom
        drawLabel(i, r, true);
    };

    for (int i = 0; i < m_items.size(); ++i) {
        if (i == m_active || i == dragIndex)
            continue;
        const QRect r = tabRect(i);
        if (!r.adjusted(-kFlare, 0, kFlare, 0).intersects(e->rect()))
            continue;
        const bool hovered = i == m_hover && m_dragUid == 0;
        if (hovered) {
            p.setPen(Qt::NoPen);
            p.setBrush(m_look.hoverBg);
            p.drawRoundedRect(QRectF(r).adjusted(2, 1, -2, -5), 7, 7);
        }
        // Separators between plain tabs, as in Chrome.
        const bool nextPlain = i + 1 < m_items.size() && i + 1 != m_active && i + 1 != m_hover && i + 1 != dragIndex;
        if (!hovered && nextPlain) {
            p.setPen(QPen(m_look.border, 1));
            p.drawLine(QPointF(r.right() + 0.5, r.top() + 8), QPointF(r.right() + 0.5, h - 10));
        }
        drawLabel(i, r, false);
    }
    if (m_active >= 0 && m_active != dragIndex)
        drawRaised(m_active, tabRect(m_active));
    if (dragIndex >= 0) {
        QRect r = tabRect(dragIndex);
        r.moveLeft(m_dragX);
        drawRaised(dragIndex, r);
    }
    p.setClipping(false);
    const QRect nr = newTabRect();
    if (nr.intersects(e->rect())) {
        if (m_hoverPart == Part::New) {
            p.setPen(Qt::NoPen);
            p.setBrush(m_look.hoverBg);
            p.drawRoundedRect(QRectF(nr), 7, 7);
        }
        icons::paint(&p, QStringLiteral("plus"), QRectF(nr).adjusted(6, 6, -6, -6),
                     m_hoverPart == Part::New ? m_look.text : m_look.dim);
    }
}

void TabStrip::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    clampScroll();
    ensureVisible(m_active);
}

// ------------------------------------------------------------ input ----
void TabStrip::mousePressEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    const int i = tabAt(pos);
    m_pressPos = pos;
    m_pressed = -1;
    m_pressedPart = Part::None;
    if (e->button() == Qt::MiddleButton) {
        m_pressed = i;
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    if (i < 0) {
        if (newTabRect().contains(pos))
            m_pressedPart = Part::New;
        return;
    }
    m_pressed = i;
    // The close button of a tab under the mouse is hit-tested as shown.
    const bool close = (i == m_active || tabWidth() >= 90) && closeRect(i).contains(pos);
    m_pressedPart = close ? Part::Close : Part::Tab;
    if (!close) {
        m_grabDx = pos.x() - tabRect(i).left();
        if (i != m_active)
            emit activated(i); // on press, as in Chrome
    }
}

void TabStrip::mouseMoveEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    if ((e->buttons() & Qt::LeftButton) && m_pressedPart == Part::Tab && m_pressed >= 0) {
        if (!m_dragUid && (pos - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
            const int i = qBound(0, m_pressed, int(m_items.size()) - 1);
            m_dragUid = m_items.value(i).uid;
        }
        if (m_dragUid) {
            const int w = tabWidth();
            const int minX = tabsLeft() - m_scroll;
            m_dragX = qBound(minX, pos.x() - m_grabDx, minX + (int(m_items.size()) - 1) * w);
            int from = -1;
            for (int k = 0; k < m_items.size(); ++k)
                if (m_items[k].uid == m_dragUid)
                    from = k;
            const int to = qBound(0, (m_dragX + w / 2 - minX) / w, int(m_items.size()) - 1);
            if (from >= 0 && to != from)
                emit moved(from, to); // the model moves it and calls setTabs()
            update();
            return;
        }
    }
    if (e->buttons() != Qt::NoButton)
        return;
    const int i = tabAt(pos);
    Part part = Part::None;
    if (i >= 0)
        part = (i == m_active || tabWidth() >= 90) && closeRect(i).contains(pos) ? Part::Close : Part::Tab;
    else if (newTabRect().contains(pos))
        part = Part::New;
    setHover(i, part);
}

void TabStrip::mouseReleaseEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    const int i = tabAt(pos);
    if (e->button() == Qt::MiddleButton) {
        if (i >= 0 && i == m_pressed) {
            m_frozenWidth = tabWidth();
            emit closeRequested(i);
        }
        m_pressed = -1;
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    const Part part = m_pressedPart;
    const int pressed = m_pressed;
    m_pressed = -1;
    m_pressedPart = Part::None;
    if (m_dragUid) {
        m_dragUid = 0;
        update();
        return;
    }
    if (part == Part::Close && i == pressed && closeRect(i).contains(pos)) {
        // Widths stay as they are until the mouse leaves, so the next tab's
        // close button lands under the pointer (as in Chrome).
        m_frozenWidth = tabWidth();
        emit closeRequested(i);
    } else if (part == Part::New && newTabRect().contains(pos)) {
        emit newRequested();
    }
}

void TabStrip::mouseDoubleClickEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    if (e->button() == Qt::LeftButton && tabAt(pos) < 0 && !newTabRect().contains(pos)) {
        emit newRequested(); // empty space
        return;
    }
    mousePressEvent(e); // a fast second click on × or + acts again
}

void TabStrip::leaveEvent(QEvent *e)
{
    QWidget::leaveEvent(e);
    setHover(-1, Part::None);
    if (m_frozenWidth) {
        m_frozenWidth = 0;
        clampScroll();
        update();
    }
}

void TabStrip::wheelEvent(QWheelEvent *e)
{
    if (contentWidth() <= tabsAreaWidth()) {
        e->ignore();
        return;
    }
    const QPoint px = e->pixelDelta();
    const QPoint deg = e->angleDelta();
    const int d = !px.isNull() ? (qAbs(px.x()) > qAbs(px.y()) ? px.x() : px.y())
                               : (qAbs(deg.x()) > qAbs(deg.y()) ? deg.x() : deg.y()) / 2;
    m_scroll -= d;
    clampScroll();
    update();
    e->accept();
}

void TabStrip::contextMenuEvent(QContextMenuEvent *e)
{
    const int i = tabAt(e->pos());
    if (i < 0)
        return;
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    QAction *close = menu->addAction(tr("Close Tab"));
    QAction *others = menu->addAction(tr("Close Other Tabs"));
    QAction *right = menu->addAction(tr("Close Tabs to the Right"));
    close->setEnabled(m_items.size() > 1);
    others->setEnabled(m_items.size() > 1);
    right->setEnabled(i + 1 < m_items.size());
    const quint32 uid = m_items[i].uid;
    auto indexOf = [this, uid] {
        for (int k = 0; k < m_items.size(); ++k)
            if (m_items[k].uid == uid)
                return k;
        return -1;
    };
    connect(close, &QAction::triggered, this, [this, indexOf] { if (int k = indexOf(); k >= 0) emit closeRequested(k); });
    connect(others, &QAction::triggered, this, [this, indexOf] { if (int k = indexOf(); k >= 0) emit closeOthersRequested(k); });
    connect(right, &QAction::triggered, this, [this, indexOf] { if (int k = indexOf(); k >= 0) emit closeRightRequested(k); });
    menu->popup(e->globalPos()); // no nested event loop
}

bool TabStrip::event(QEvent *e)
{
    if (e->type() == QEvent::ToolTip) {
        auto *he = static_cast<QHelpEvent *>(e);
        const int i = tabAt(he->pos());
        if (i >= 0) {
            const QString t = m_items[i].title.isEmpty() ? tr("Untitled") : m_items[i].title;
            QToolTip::showText(he->globalPos(), t, this, tabRect(i));
        } else if (newTabRect().contains(he->pos())) {
            QToolTip::showText(he->globalPos(), tr("New tab"), this, newTabRect());
        } else {
            QToolTip::hideText();
            e->ignore();
        }
        return true;
    }
    return QWidget::event(e);
}

} // namespace folia
