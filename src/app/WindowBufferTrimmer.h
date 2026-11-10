// Releases the spare buffers of a window's backing store while it is idle.
//
// On macOS, Qt draws a window into IOSurface buffers of the window's full
// pixel size (13.7 MB for 1180x759 points at 2x) and keeps a swap chain of
// them: the one on screen, plus another whenever a paint arrives while the
// window server still holds the previous frame, up to three (four at times).
// It never releases them, so an idle window keeps 27-41 MB that only matter
// while it repaints frame after frame.
//
// After `idleMs` without a paint, this gives the window a new backing store
// and repaints it once. The old store goes, with its spare buffers; the
// screen shows the same pixels from the single buffer of the new one. The
// next paints grow the chain again as they need to (the first two cost about
// 4 ms more each: a new buffer, and a copy of the unpainted area into it).
// Measurements: docs/BENCHMARKS.md §6g.
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>

class QWidget;

namespace folia {

class WindowBufferTrimmer : public QObject {
    Q_OBJECT
public:
    // `window`: a top-level widget. Inert where supported() is false.
    explicit WindowBufferTrimmer(QWidget *window, QObject *parent = nullptr);

    static bool supported();

    // Trim after this long without a paint; 0 (the default) turns it off.
    void setIdleMs(int ms);
    int idleMs() const { return m_idleMs; }

    // Trim now. False when there is nothing to do (no paint since the last
    // trim, the window is hidden) or supported() is false.
    bool trimNow();
    int trims() const { return m_trims; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void painted();
    void idleCheck();

    QPointer<QWidget> m_window;
    QTimer m_timer;
    QElapsedTimer m_sincePaint;
    int m_idleMs = 0;
    int m_trims = 0;
    bool m_paintedSinceTrim = false;
    bool m_trimming = false;
};

} // namespace folia
