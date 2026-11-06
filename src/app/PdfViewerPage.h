// The PDF viewer page (docs/ATTACHMENTS.md §4): shown inside the main window
// in place of the layout, like Settings, when a PDF card is clicked. A
// header (back to the notes, the file name, the page indicator, zoom) over
// one PdfView. Esc, Mod+W or the back button return to the note. Created
// when opened and destroyed when closed, which closes the document.
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

private:
    void refresh();
    void updatePageLabel(int page);

    AppController *m_app;
    QString m_hash;
    QString m_name;
    PdfView *m_view = nullptr;
    QToolButton *m_back = nullptr;
    QLabel *m_pageLabel = nullptr;
    QLabel *m_zoomLabel = nullptr;
    QToolButton *m_zoomOut = nullptr;
    QToolButton *m_zoomIn = nullptr;
    QToolButton *m_fit = nullptr;
};

} // namespace folia
