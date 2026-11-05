// Native editing surface (editor-and-input owner). A QTextEdit whose document
// is the active PageSession's QTextDocument laid out by VirtualLayout. IME,
// selection, undo/redo, clipboard and keyboard traversal are Qt's native
// text control; this class adds note-specific keys, plain-text clipboard,
// page-link activation, the inline-math preview bubble and latency probes.
// It never calls into Lua.
#pragma once

#include "core/Api.h"
#include "editor/PageSession.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QTextEdit>
#include <QTimer>
#include <memory>

namespace folia {

class EditorContext;
class PageSession;
class MathDisplayList;

class NoteEditor : public QTextEdit {
    Q_OBJECT
public:
    explicit NoteEditor(EditorContext *ctx, QWidget *parent = nullptr);
    ~NoteEditor() override;

    void setSession(PageSession *session);
    PageSession *session() const { return m_session; }
    void detachSession();

    bool hasPreedit() const { return m_preedit; }
    bool copyCodeBlockAtCursor();
    void applyTheme();
    // Persist caret/scroll into the session (called on page switch/close).
    void storeViewState();

signals:
    void openPageRequested(folia::PageId id);
    void preeditChanged(bool active);
    void escapePressed();
    void painted();

protected:
    bool event(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void inputMethodEvent(QInputMethodEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void scrollContentsBy(int dx, int dy) override;
    QMimeData *createMimeDataFromSelection() const override;
    bool canInsertFromMimeData(const QMimeData *source) const override;
    void insertFromMimeData(const QMimeData *source) override;

private:
    void updateLayoutViewport();
    void updateInlineMath();
    bool handleSmartKeys(QKeyEvent *e);
    QString plainTextForRange(int from, int to) const;

    EditorContext *m_ctx;
    QPointer<PageSession> m_session;
    bool m_preedit = false;
    bool m_pendingInput = false;
    QElapsedTimer m_inputTimer;

    // Inline math bubble
    int m_inlineStart = -1;
    int m_inlineEnd = -1;
    quint64 m_inlineHash = 0;
    std::shared_ptr<const MathDisplayList> m_inlineList;
    QString m_inlineError;

    // The caret stops blinking after a few idle seconds so an idle window
    // has no repaint loop (no wakeups, no back-buffer churn).
    void caretActivity();
    QTimer m_caretIdle;
    int m_flashTime = -1;
};

} // namespace folia
