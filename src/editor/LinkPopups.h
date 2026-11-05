// Google Docs-style link UI floating over the editor:
//  * LinkBubble: shown while the caret is in a link — the address (click to
//    open) plus Open / Copy / Edit / Remove buttons. Never takes focus.
//  * LinkEditor: the "Text" + "Link" form opened with Mod+K or Edit.
// Both are plain child widgets of the editor; NoteEditor positions them and
// performs the actual edits.
#pragma once

#include <QFrame>

class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;

namespace folia {

struct EditorTheme;

class LinkBubble : public QFrame {
    Q_OBJECT
public:
    explicit LinkBubble(QWidget *parent);
    void setLink(const QString &href, bool removable);
    QString href() const { return m_href; }
    void applyTheme(const EditorTheme &t);

signals:
    void openRequested();
    void copyRequested();
    void editRequested();
    void removeRequested();

private:
    QToolButton *iconButton(const QString &icon, const QString &tip);
    QString m_href;
    QLabel *m_icon;
    QToolButton *m_url;
    QToolButton *m_open;
    QToolButton *m_copy;
    QToolButton *m_edit;
    QToolButton *m_remove;
    QWidget *m_sep;
};

class LinkEditor : public QFrame {
    Q_OBJECT
public:
    explicit LinkEditor(QWidget *parent);
    // Fills the form and focuses the field to type into next.
    void start(const QString &text, const QString &href);
    QString text() const;
    QString href() const;
    void applyTheme(const EditorTheme &t);

signals:
    void applied(const QString &text, const QString &href);
    void cancelled();

protected:
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    void submit();
    QLineEdit *m_text;
    QLineEdit *m_url;
    QPushButton *m_apply;
    QLabel *m_textLabel;
    QLabel *m_urlLabel;
};

} // namespace folia
