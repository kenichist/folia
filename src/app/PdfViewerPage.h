// The PDF viewer tab (docs/ATTACHMENTS.md §4): opened beside the note in
// the editor slot's split when a PDF card is clicked, on the side the user
// last chose. A header (close, the file name, the page indicator, zoom, swap
// sides) over one PdfView. Esc or Mod+W (while it has focus), the close
// button or the pdf.close command close it. Created when opened and
// destroyed when closed, which closes the document.
#pragma once

#include "core/Attachments.h"

#include <QWidget>

class QLabel;
class QToolButton;

namespace folia {

class AppController;
class PdfView;

class PdfViewerPage : public QWidget {
    Q_OBJECT
public:
    PdfViewerPage(AppController *app, const AttachmentInfo &info, const QString &name, QWidget *parent);
    ~PdfViewerPage() override;

    PdfView *view() const { return m_view; }
    QString hash() const { return m_hash; }
    QString name() const { return m_name; }
    // Frees the view now: its queued renders, its pages and tiles in the
    // cache, its document handle and the attachment pin. The widget itself
    // may then be deleted later (closing from its own key or button).
    void release();

protected:
    bool event(QEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    void refresh();
    void updatePageLabel(int page);
    void elideTitle();
    static bool isCloseKey(const QKeyEvent *e);

    AppController *m_app;
    QString m_hash;
    QString m_name;
    PdfView *m_view = nullptr;
    QToolButton *m_close = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_pageLabel = nullptr;
    QLabel *m_zoomLabel = nullptr;
    QToolButton *m_zoomOut = nullptr;
    QToolButton *m_zoomIn = nullptr;
    QToolButton *m_fit = nullptr;
    QToolButton *m_swap = nullptr;
};

} // namespace folia
