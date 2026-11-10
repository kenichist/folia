#include "app/WindowBufferTrimmer.h"

#include "core/Instrument.h"

#include <QBackingStore>
#include <QEvent>
#include <QWidget>
#include <QWindow>

#ifdef Q_OS_MACOS
// QWidget::setBackingStore() is all it takes, but Qt 6 declares it private
// (it was public in Qt 5, is still documented and is still exported). An
// explicit template instantiation may name a private member, which is how its
// address is taken here. A Qt without the function fails to compile here
// rather than misbehave at run time.
namespace {
template <typename Tag, typename Tag::type Member>
struct PrivateAccess {
    friend typename Tag::type privateMember(Tag) { return Member; }
};
struct SetBackingStore {
    using type = void (QWidget::*)(QBackingStore *);
    friend type privateMember(SetBackingStore);
};
template struct PrivateAccess<SetBackingStore, &QWidget::setBackingStore>;
} // namespace
#endif

namespace folia {

WindowBufferTrimmer::WindowBufferTrimmer(QWidget *window, QObject *parent) : QObject(parent), m_window(window)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &WindowBufferTrimmer::idleCheck);
    if (!supported() || !window)
        return;
    window->installEventFilter(this);
    if (QWindow *handle = window->windowHandle())
        handle->installEventFilter(this);
}

bool WindowBufferTrimmer::supported()
{
#ifdef Q_OS_MACOS
    return true;
#else
    return false; // other platforms' backing stores hold one buffer
#endif
}

void WindowBufferTrimmer::setIdleMs(int ms)
{
    m_idleMs = qMax(0, ms);
    m_timer.stop();
    if (m_idleMs > 0 && m_paintedSinceTrim && supported())
        m_timer.start(m_idleMs);
}

bool WindowBufferTrimmer::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::UpdateRequest: // the widget is about to paint what changed
        if (watched == m_window)
            painted();
        break;
    case QEvent::Expose: // the window system asked for a repaint
        if (watched != m_window)
            painted();
        break;
    case QEvent::Show:
    case QEvent::WinIdChange: // the native window exists (again): watch it too
        if (watched == m_window && m_window->windowHandle())
            m_window->windowHandle()->installEventFilter(this);
        break;
    default:
        break;
    }
    return false;
}

void WindowBufferTrimmer::painted()
{
    if (m_trimming)
        return;
    m_paintedSinceTrim = true;
    m_sincePaint.start();
    // Not re-armed on every paint: idleCheck() waits for the remainder.
    if (m_idleMs > 0 && !m_timer.isActive())
        m_timer.start(m_idleMs);
}

void WindowBufferTrimmer::idleCheck()
{
    if (m_idleMs <= 0 || !m_paintedSinceTrim)
        return;
    const qint64 idle = m_sincePaint.isValid() ? m_sincePaint.elapsed() : m_idleMs;
    if (idle < m_idleMs) {
        m_timer.start(int(m_idleMs - idle));
        return;
    }
    trimNow();
}

bool WindowBufferTrimmer::trimNow()
{
#ifdef Q_OS_MACOS
    QWidget *w = m_window;
    if (!w || !m_paintedSinceTrim || !w->isVisible() || !w->windowHandle())
        return false;
    QElapsedTimer t;
    t.start();
    m_trimming = true;
    // The widget owns the new store and deletes the old one. The buffer on
    // screen stays alive (the window's layer holds it) until the repaint
    // below puts the new store's buffer there. A window the system is not
    // showing (covered, another Space) repaints when it is exposed again.
    (w->*privateMember(SetBackingStore()))(new QBackingStore(w->windowHandle()));
    w->repaint();
    m_trimming = false;
    m_paintedSinceTrim = false;
    ++m_trims;
    instr::add(instr::WindowBufferTrims);
    instr::record("window_buffer_trim_us", t.nsecsElapsed() / 1000);
    return true;
#else
    return false;
#endif
}

} // namespace folia
