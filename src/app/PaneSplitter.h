// The editor slot's two "tabs" side by side: the note (the one EditorPane,
// always present, never re-parented by this widget) and at most one other
// pane (the PDF viewer), on its left or its right, with a draggable divider.
//
// Cost model (docs/ARCHITECTURE.md §5, "Split view"):
//  * One tab open: the splitter holds the editor only, so it has no handle,
//    paints nothing and adds one widget to the tree.
//  * The divider resizes both panes live, one step per frame (macOS coalesces
//    drags to the display's refresh rate). Each pane keeps its own resize
//    cost low: the PDF view draws its cached pixels scaled and renders once
//    the size settles; the note re-lays out only what is visible.
//  * Panes never collapse to zero width, so neither lays out at a degenerate
//    width, and the split never repaints on its own.
//  * The divider position is reported once per drag (on release), not per
//    step, so persisting it costs one settings write.
#pragma once

#include <QSplitter>

#include <functional>

namespace folia {

class PaneSplitter : public QSplitter {
    Q_OBJECT
public:
    enum class Side { Left, Right }; // where the second pane goes

    explicit PaneSplitter(QWidget *primary, QWidget *parent = nullptr);

    QWidget *primary() const { return m_primary; }
    QWidget *secondary() const { return m_secondary; }
    // Places `w` beside the primary pane (or replaces the current one, which
    // is removed from the splitter and returned, not deleted).
    QWidget *setSecondary(QWidget *w);
    // Removes the second pane (not deleted) and returns it.
    QWidget *takeSecondary();

    Side side() const { return m_side; }
    void setSide(Side s);
    void swapSides() { setSide(m_side == Side::Left ? Side::Right : Side::Left); }

    // The second pane's share of the width, 0..1.
    double ratio() const;
    void setRatio(double r);
    static constexpr double kMinRatio = 0.15;
    static constexpr double kMaxRatio = 0.85;
    static constexpr int kMinPaneWidth = 260;

    // The divider's line color (named shell color "border").
    void setLineColor(const QColor &c);
    QColor lineColor() const { return m_line; }

signals:
    // Once per divider drag, when it is released (and on a double-click,
    // which resets the split to equal widths).
    void ratioCommitted(double ratio);

protected:
    QSplitterHandle *createHandle() override;
    void resizeEvent(QResizeEvent *e) override;

private:
    friend class PaneSplitterHandle;
    void applyRatio();
    void released();

    QWidget *m_primary;
    QWidget *m_secondary = nullptr;
    Side m_side = Side::Right;
    double m_ratio = 0.5;
    QColor m_line{222, 222, 226};
};

} // namespace folia
