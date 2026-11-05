#include "editor/EditorPane.h"

#include "editor/EditorContext.h"
#include "editor/NoteEditor.h"
#include "editor/PageSession.h"
#include "editor/VirtualLayout.h"

#include <QKeyEvent>
#include <QLineEdit>
#include <QVBoxLayout>

namespace folia {

EditorPane::EditorPane(EditorContext *ctx, QWidget *parent) : QWidget(parent), m_ctx(ctx)
{
    setObjectName(QStringLiteral("folia-editor-pane"));
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    m_title = new QLineEdit(this);
    m_title->setObjectName(QStringLiteral("folia-title"));
    m_title->setFrame(false);
    m_title->setPlaceholderText(tr("Untitled"));
    m_title->setAccessibleName(tr("Page title"));
    m_editor = new NoteEditor(ctx, this);
    m_editor->setAccessibleName(tr("Page body"));
    lay->addWidget(m_title);
    lay->addWidget(m_editor, 1);
    setFocusProxy(m_editor);

    m_title->installEventFilter(this);
    connect(m_title, &QLineEdit::textEdited, this, [this](const QString &t) {
        if (m_session)
            m_session->setTitle(t);
    });
    connect(m_editor, &NoteEditor::escapePressed, this, [this] { focusTitle(); });
    connect(ctx, &EditorContext::themeChanged, this, &EditorPane::applyTheme);
    applyTheme();
}

void EditorPane::applyTheme()
{
    const EditorTheme &t = m_ctx->theme();
    QFont f = t.body;
    f.setPointSizeF(t.body.pointSizeF() * 1.9);
    f.setWeight(QFont::Bold);
    m_title->setFont(f);
    QPalette p = m_title->palette();
    p.setColor(QPalette::Base, t.background);
    p.setColor(QPalette::Window, t.background);
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::PlaceholderText, t.dim);
    m_title->setPalette(p);
    m_title->setAutoFillBackground(true);
    QPalette pp = palette();
    pp.setColor(QPalette::Window, t.background);
    setPalette(pp);
    setAutoFillBackground(true);
    m_editor->applyTheme();
    alignTitle();
}

void EditorPane::setSession(PageSession *s)
{
    if (s)
        connect(s, &PageSession::closing, this, [this, s] {
            if (m_session == s) {
                m_session = nullptr;
                m_title->clear();
                m_title->setEnabled(false);
            }
        }, Qt::DirectConnection);
    m_session = s;
    m_editor->setSession(s);
    m_title->setText(s ? s->title() : QString());
    m_title->setEnabled(s != nullptr);
    alignTitle();
}

void EditorPane::focusTitle(bool selectAll)
{
    m_title->setFocus(Qt::OtherFocusReason);
    if (selectAll)
        m_title->selectAll();
    else
        m_title->setCursorPosition(m_title->text().size());
}

void EditorPane::focusBody()
{
    m_editor->setFocus(Qt::OtherFocusReason);
}

bool EditorPane::eventFilter(QObject *o, QEvent *e)
{
    if (o == m_title && e->type() == QEvent::KeyPress) {
        auto *k = static_cast<QKeyEvent *>(e);
        if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter || k->key() == Qt::Key_Down) {
            QTextCursor c = m_editor->textCursor();
            if (k->key() != Qt::Key_Down || true) {
                c.movePosition(QTextCursor::Start);
                m_editor->setTextCursor(c);
            }
            focusBody();
            return true;
        }
    }
    return QWidget::eventFilter(o, e);
}

void EditorPane::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    alignTitle();
}

void EditorPane::alignTitle()
{
    // Align the title with the editor's text column.
    const EditorTheme &t = m_ctx->theme();
    const qreal width = m_editor->viewport()->width();
    qreal col = width - 2 * t.sideMargin;
    if (t.maxColumnWidth > 0)
        col = qMin(col, t.maxColumnWidth);
    col = qMax<qreal>(col, 160);
    const int left = int(qMax(t.sideMargin, (width - col) / 2));
    m_title->setTextMargins(left - 2, int(t.topMargin + 10), int(qMax<qreal>(0, width - left - col)), 2);
}

} // namespace folia
