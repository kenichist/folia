// The shell's single "editor" primitive: page title field + native editor.
// One instance exists for the app's lifetime; layouts re-parent it rather
// than recreate it, so the document, undo stack, selection and an active IME
// composition survive shell reloads.
#pragma once

#include "core/Api.h"
#include "editor/PageSession.h"

#include <QPointer>
#include <QWidget>

class QLineEdit;

namespace folia {

class EditorContext;
class NoteEditor;
class PageSession;

class EditorPane : public QWidget {
    Q_OBJECT
public:
    explicit EditorPane(EditorContext *ctx, QWidget *parent = nullptr);

    NoteEditor *editor() const { return m_editor; }
    QLineEdit *titleField() const { return m_title; }
    void setSession(PageSession *s);
    PageSession *session() const { return m_session; }
    void focusTitle(bool selectAll = false);
    void focusBody();
    void applyTheme();

protected:
    bool eventFilter(QObject *o, QEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    void alignTitle();

    EditorContext *m_ctx;
    QLineEdit *m_title;
    NoteEditor *m_editor;
    QPointer<PageSession> m_session;
};

} // namespace folia
