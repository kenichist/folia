// Native editing surface (editor-and-input owner). A QTextEdit whose document
// is the active PageSession's QTextDocument laid out by VirtualLayout. IME,
// selection, undo/redo, clipboard and keyboard traversal are Qt's native
// text control; this class adds note-specific keys, plain-text clipboard,
// page-link activation, web links (bubble + Mod+K editor), the inline-math
// preview bubble and latency probes.
// It never calls into Lua.
#pragma once

#include "core/Api.h"
#include "editor/PageSession.h"
#include "storage/AttachmentStore.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QTextEdit>
#include <QTimer>
#include <memory>

namespace folia {

class EditorContext;
class PageSession;
class MathDisplayList;
class LinkBubble;
class LinkEditor;

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

    // A web link in the text: a named link (label with an anchor format) or
    // a bare URL. Positions are document positions, [start, end).
    struct LinkHit {
        int start = -1;
        int end = -1;
        QString href;
        bool named = false;
        bool valid() const { return start >= 0 && end > start; }
    };
    // The link containing the character at `pos`.
    LinkHit linkAt(int pos) const;
    // The link the caret is in or right after (what the bubble shows).
    LinkHit linkNearCaret() const;
    // Mod+K: edit the link at the caret, or link the selection (or a new
    // label typed into the form). False (with a reason) where links cannot go.
    bool editLink(QString *why = nullptr);
    bool isEditingLink() const;

    // Images (docs/ATTACHMENTS.md): imported on a worker, inserted after the
    // caret's line once each file is stored. Paste and drag-and-drop go
    // through insertFromMimeData; this is also what the file picker uses.
    bool importImages(const QList<AttachmentStore::Source> &sources);
    // Edit ▸ Insert Image…: a file picker. False when nothing was chosen.
    bool chooseImages();
    // Edit ▸ Insert PDF…: the same import, for PDF files.
    bool choosePdfs();
    // The image sources a clipboard or drop carries (empty: treat as text).
    static QList<AttachmentStore::Source> imageSources(const QMimeData *data);

signals:
    void openPageRequested(folia::PageId id);
    // A PDF card was clicked (or Mod+Enter on its line).
    void openPdfRequested(const QString &hash, const QString &name);
    void openUrlRequested(const QString &href);
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

    // Links
    LinkHit linkUnderPoint(const QPointF &docPt) const;
    void scheduleLinkPopups();
    void updateLinkBubble();
    void placePopup(QWidget *w, int start);
    void applyLink(const QString &text, const QString &href);
    void removeLink();
    void closeLinkEditor();
    // Typing at the edge of a link continues as plain text (as in Google Docs).
    void preventLinkGrowth();
    // `[label](url)` just typed becomes a named link.
    bool autoLinkTypedMarkdown();
    LinkBubble *m_bubble = nullptr;
    LinkEditor *m_linkEditor = nullptr;
    QTimer m_linkTimer;
    int m_bubbleStart = -1;
    int m_editStart = -1;
    int m_editEnd = -1;

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
