#include "app/PdfViewerPage.h"

#include "app/AppController.h"
#include "core/LucideIcons.h"
#include "editor/EditorContext.h"
#include "editor/PdfView.h"
#include "shell/ShellHost.h"
#include "storage/AttachmentStore.h"

#include <QBoxLayout>
#include <QLabel>
#include <QShortcut>
#include <QToolButton>

#include <cmath>

namespace folia {

PdfViewerPage::PdfViewerPage(AppController *app, const AttachmentInfo &info, const QString &name, QWidget *parent)
    : QWidget(parent), m_app(app), m_hash(info.hash), m_name(name)
{
    setObjectName(QStringLiteral("folia-pdf-viewer"));
    setAutoFillBackground(true);
    auto *header = new QWidget(this);
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(8, 6, 16, 6);
    hl->setSpacing(6);
    m_back = new QToolButton(header);
    m_back->setObjectName(QStringLiteral("pdf.back"));
    m_back->setAutoRaise(true);
    m_back->setText(tr("Notes"));
    m_back->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_back->setIconSize(QSize(16, 16));
    m_back->setToolTip(tr("Back to your notes (Esc)"));
    m_back->setAccessibleName(tr("Back to notes"));
    connect(m_back, &QToolButton::clicked, this, [this] { m_app->closePdf(); });
    auto *title = new QLabel(name, header);
    title->setObjectName(QStringLiteral("pdf.title"));
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.2);
    tf.setWeight(QFont::DemiBold);
    title->setFont(tf);
    title->setTextInteractionFlags(Qt::NoTextInteraction);
    title->setMinimumWidth(60);
    m_pageLabel = new QLabel(header);
    m_pageLabel->setObjectName(QStringLiteral("pdf.page"));
    auto tool = [&](const QString &text, const QString &tip) {
        auto *b = new QToolButton(header);
        b->setAutoRaise(true);
        b->setText(text);
        b->setToolTip(tip);
        b->setAccessibleName(tip);
        return b;
    };
    m_zoomOut = tool(QStringLiteral("−"), tr("Zoom out (Mod+-)"));
    m_zoomIn = tool(QStringLiteral("+"), tr("Zoom in (Mod+=)"));
    m_fit = tool(tr("Fit"), tr("Fit width (Mod+0)"));
    m_zoomLabel = new QLabel(header);
    m_zoomLabel->setMinimumWidth(44);
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    hl->addWidget(m_back);
    hl->addSpacing(6);
    hl->addWidget(title, 1);
    hl->addWidget(m_pageLabel);
    hl->addSpacing(12);
    hl->addWidget(m_zoomOut);
    hl->addWidget(m_zoomLabel);
    hl->addWidget(m_zoomIn);
    hl->addWidget(m_fit);

    // Pinned while open: the viewer may reopen the file (after being hidden)
    // even if the note's reference is deleted meanwhile.
    app->attachments()->pin(info.hash);
    m_view = new PdfView(app->editorContext(), info, app->attachments()->pathFor(info.hash), this);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(header);
    lay->addWidget(m_view, 1);
    setFocusProxy(m_view);

    connect(m_zoomOut, &QToolButton::clicked, m_view, [this] { m_view->zoomGesture(0.8); });
    connect(m_zoomIn, &QToolButton::clicked, m_view, [this] { m_view->zoomGesture(1.25); });
    connect(m_fit, &QToolButton::clicked, m_view, &PdfView::fitWidth);
    connect(m_view, &PdfView::currentPageChanged, this, &PdfViewerPage::updatePageLabel);
    connect(m_view, &PdfView::zoomChanged, this, [this](qreal z) {
        // 100 % = the page at its printed size (1 pt = 1 logical px).
        m_zoomLabel->setText(QStringLiteral("%1 %").arg(qRound(z * 100)));
    });
    for (const QKeySequence &k : {QKeySequence(Qt::Key_Escape), QKeySequence(QKeySequence::Close)}) {
        auto *sc = new QShortcut(k, this);
        sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(sc, &QShortcut::activated, this, [this] { m_app->closePdf(); });
    }
    connect(app->editorContext(), &EditorContext::themeChanged, this, &PdfViewerPage::refresh);
    updatePageLabel(0);
    refresh();
}

PdfViewerPage::~PdfViewerPage()
{
    delete m_view; // first: its requests and its document go before the pin
    m_view = nullptr;
    m_app->attachments()->unpin(m_hash);
}

void PdfViewerPage::updatePageLabel(int page)
{
    m_pageLabel->setText(tr("Page %1 of %2").arg(page + 1).arg(m_view->pageCount()));
}

void PdfViewerPage::refresh()
{
    const EditorTheme &t = m_app->editorContext()->theme();
    QPalette p = palette();
    p.setColor(QPalette::Window, t.background);
    p.setColor(QPalette::WindowText, t.text);
    setPalette(p);
    m_back->setIcon(icons::icon(QStringLiteral("chevron-left"), t.text));
    const QString button = QStringLiteral("QToolButton{border:none;border-radius:6px;padding:4px 8px;color:%1;}"
                                          "QToolButton:hover{background-color:%2;}")
                               .arg(t.text.name(), t.selection.name(QColor::HexArgb));
    for (QToolButton *b : {m_back, m_zoomOut, m_zoomIn, m_fit})
        b->setStyleSheet(button);
    QPalette lp = m_pageLabel->palette();
    lp.setColor(QPalette::WindowText, t.dim);
    m_pageLabel->setPalette(lp);
    m_zoomLabel->setPalette(lp);
}

} // namespace folia
