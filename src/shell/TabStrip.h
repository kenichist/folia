// The note tab strip (docs/ARCHITECTURE.md §5, "Tabs"), placed where a
// layout says ui.tabs{}. One custom-painted widget: no child widget per tab,
// no style sheet, no timers or animations. It shows what it is given and
// reports clicks; the tab list itself lives in AppController.
//
// Cost model:
//  * Hover changes repaint the two tab rects involved, not the strip.
//  * Elided titles are cached per tab and recomputed only when a tab's width
//    or title changes; setTabs() with an unchanged list does nothing.
//  * setAppearance() compares first, so a shell re-render with the same
//    colors does not repaint.
//  * Typing in the note never reaches it; renaming a page repaints its tab.
#pragma once

#include <QColor>
#include <QString>
#include <QVector>
#include <QWidget>

namespace folia {

class TabStrip : public QWidget {
    Q_OBJECT
public:
    struct Item {
        quint32 uid = 0; // stable across moves
        QString title;
        bool operator==(const Item &o) const { return uid == o.uid && title == o.title; }
    };
    struct Appearance {
        QColor bg{243, 243, 245};
        QColor activeBg{255, 255, 255};
        QColor text{30, 30, 32};
        QColor dim{120, 120, 128};
        QColor hoverBg{0, 0, 0, 18};
        QColor border{222, 222, 226};
        int minTabWidth = 72;
        int maxTabWidth = 220;
        int height = 36;
        bool operator==(const Appearance &o) const;
        bool operator!=(const Appearance &o) const { return !(*this == o); }
    };

    explicit TabStrip(QWidget *parent = nullptr);

    void setTabs(const QVector<Item> &items, int active);
    // Updates one tab's title (a rename); repaints that tab only.
    void setTitle(quint32 uid, const QString &title);
    void setAppearance(const Appearance &a);
    const Appearance &appearance() const { return m_look; }
    // The layout's `visible`, and `auto_hide` (shown only with 2+ tabs).
    void setLayoutVisible(bool on);
    void setAutoHide(bool on);

    int count() const { return int(m_items.size()); }
    int activeIndex() const { return m_active; }
    QString titleAt(int i) const { return m_items.value(i).title; }
    // Geometry, in widget coordinates (tests and the bench click these).
    QRect tabRect(int i) const;
    QRect closeRect(int i) const;
    QRect newTabRect() const;
    int tabAt(const QPoint &p) const;
    int paintCount() const { return m_paints; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void activated(int index);
    void closeRequested(int index);
    void closeOthersRequested(int index);
    void closeRightRequested(int index);
    void moved(int from, int to);
    void newRequested();

protected:
    bool event(QEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;

private:
    struct Slot {
        QString elided;
        int elidedWidth = -1; // text width the elision was computed for
    };
    enum class Part { None, Tab, Close, New };

    void relayout();
    int tabWidth() const;
    int tabsLeft() const { return 8; }
    int tabsAreaWidth() const;
    int contentWidth() const;
    bool closeShown(int i) const;
    void setHover(int index, Part part);
    void updateTab(int i);
    void clampScroll();
    void ensureVisible(int i);
    void applyVisible();
    QString elidedTitle(int i, int textWidth);

    QVector<Item> m_items;
    QVector<Slot> m_slots;
    int m_active = -1;
    Appearance m_look;
    bool m_layoutVisible = true;
    bool m_autoHide = false;

    int m_scroll = 0;        // horizontal offset when the tabs overflow
    int m_frozenWidth = 0;   // tab width kept after a mouse close, until the mouse leaves
    int m_hover = -1;
    Part m_hoverPart = Part::None;
    int m_pressed = -1;      // tab under a left press (or -1)
    Part m_pressedPart = Part::None;
    QPoint m_pressPos;
    int m_grabDx = 0;        // press x relative to the dragged tab's left edge
    quint32 m_dragUid = 0;   // the tab being dragged (0: none)
    int m_dragX = 0;         // its left edge while dragged
    int m_paints = 0;
};

} // namespace folia
