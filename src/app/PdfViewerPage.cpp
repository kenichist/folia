#include "app/PdfViewerPage.h"

#include "app/AppController.h"
#include "core/LucideIcons.h"
#include "editor/EditorContext.h"
#include "editor/PdfView.h"
#include "storage/AttachmentStore.h"

#include <QBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QToolButton>

#include <cmath>

namespace folia {

namespace {
// Narrow panes: the zoom label, then the page label, give their room to the title.
constexpr int kNoZoomLabelWidth = 420;
constexpr int kNoPageLabelWidth = 340;
} // namespace

PdfViewerPage::PdfViewerPage(AppController *app, const AttachmentInfo &info, const QString &name, QWidget *parent)
    : QWidget(parent), m_app(app), m_hash(info.hash), m_name(name)
{
    setObjectName(QStringLiteral("folia-pdf-viewer"));
    setAutoFillBackground(true);
    auto *header = new QWidget(this);
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(6, 6, 8, 6);
    hl->setSpacing(2);
    auto tool = [&](const QString &objName, const QString &text, const QString &tip) {
        auto *b = new QToolButton(header);
        b->setObjectName(objName);
        b->setAutoRaise(true);
        b->setText(text);
        b->setToolTip(tip);
        b->setAccessibleName(tip);
        b->setIconSize(QSize(16, 16));
        return b;
    };
    m_close = tool(QStringLiteral("pdf.close"), QString(), tr("Close PDF (Esc)"));
    connect(m_close, &QToolButton::clicked, this, [this] { m_app->closePdf(); });
    m_title = new QLabel(header);
    m_title->setObjectName(QStringLiteral("pdf.title"));
    QFont tf = m_title->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.1);
    tf.setWeight(QFont::DemiBold);
    m_title->setFont(tf);
    m_title->setTextInteractionFlags(Qt::NoTextInteraction);
    m_title->setToolTip(name);
    // Elided to whatever room is left, so a long name never widens the pane.
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_title->setMinimumWidth(0);
    m_title->installEventFilter(this); // elided again whenever its room changes
    m_pageLabel = new QLabel(header);
    m_pageLabel->setObjectName(QStringLiteral("pdf.page"));
    m_zoomOut = tool(QStringLiteral("pdf.zoom_out"), QStringLiteral("−"), tr("Zoom out (Mod+-)"));
    m_zoomIn = tool(QStringLiteral("pdf.zoom_in"), QStringLiteral("+"), tr("Zoom in (Mod+=)"));
    m_fit = tool(QStringLiteral("pdf.fit"), tr("Fit"), tr("Fit width (Mod+0)"));
    m_swap = tool(QStringLiteral("pdf.swap"), QString(), tr("Swap sides"));
    connect(m_swap, &QToolButton::clicked, this, [this] { m_app->swapPanes(); });
    m_zoomLabel = new QLabel(header);
    m_zoomLabel->setMinimumWidth(44);
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    hl->addWidget(m_close);
    hl->addSpacing(4);
    hl->addWidget(m_title, 1);
    hl->addSpacing(6);
    hl->addWidget(m_pageLabel);
    hl->addSpacing(8);
    hl->addWidget(m_zoomOut);
    hl->addWidget(m_zoomLabel);
    hl->addWidget(m_zoomIn);
    hl->addWidget(m_fit);
    hl->addSpacing(4);
    hl->addWidget(m_swap);

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
    connect(app->editorContext(), &EditorContext::themeChanged, this, &PdfViewerPage::refresh);
    updatePageLabel(0);
    refresh();
}

PdfViewerPage::~PdfViewerPage()
{
    release();
}

void PdfViewerPage::release()
{
    if (!m_view)
        return; // released already: the app may be gone by now
    disconnect(m_app->editorContext(), nullptr, this, nullptr);
    delete m_view; // first: its requests and its document go before the pin
    m_view = nullptr;
    m_app->attachments()->unpin(m_hash);
}

bool PdfViewerPage::isCloseKey(const QKeyEvent *e)
{
    return (e->key() == Qt::Key_Escape && e->modifiers() == Qt::NoModifier) || e->matches(QKeySequence::Close);
}

bool PdfViewerPage::event(QEvent *e)
{
    // Esc and Mod+W belong to this tab while it has focus: claiming them
    // here (instead of a QShortcut) keeps a layout's own window-wide binding
    // of the same key working everywhere else, with no ambiguity.
    if (e->type() == QEvent::ShortcutOverride && isCloseKey(static_cast<QKeyEvent *>(e))) {
        e->accept();
        return true;
    }
    return QWidget::event(e);
}

void PdfViewerPage::keyPressEvent(QKeyEvent *e)
{
    if (isCloseKey(e)) {
        e->accept();
        m_app->closePdf(); // destroys this page: nothing may touch it after
        return;
    }
    QWidget::keyPressEvent(e);
}

bool PdfViewerPage::eventFilter(QObject *o, QEvent *e)
{
    if (o == m_title && e->type() == QEvent::Resize)
        elideTitle();
    return QWidget::eventFilter(o, e);
}

void PdfViewerPage::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    m_zoomLabel->setVisible(width() >= kNoZoomLabelWidth);
    m_pageLabel->setVisible(width() >= kNoPageLabelWidth);
}

void PdfViewerPage::elideTitle()
{
    const QString t = m_title->fontMetrics().elidedText(m_name, Qt::ElideMiddle, qMax(0, m_title->width()));
    if (t != m_title->text())
        m_title->setText(t);
}

void PdfViewerPage::updatePageLabel(int page)
{
    if (m_view)
        m_pageLabel->setText(tr("%1 / %2").arg(page + 1).arg(m_view->pageCount()));
}

void PdfViewerPage::refresh()
{
    const EditorTheme &t = m_app->editorContext()->theme();
    QPalette p = palette();
    p.setColor(QPalette::Window, t.background);
    p.setColor(QPalette::WindowText, t.text);
    setPalette(p);
    m_close->setIcon(icons::icon(QStringLiteral("x"), t.text));
    m_swap->setIcon(icons::icon(QStringLiteral("arrow-left-right"), t.text));
    const QString button = QStringLiteral("QToolButton{border:none;border-radius:6px;padding:4px 6px;color:%1;}"
                                          "QToolButton:hover{background-color:%2;}")
                               .arg(t.text.name(), t.selection.name(QColor::HexArgb));
    for (QToolButton *b : {m_close, m_zoomOut, m_zoomIn, m_fit, m_swap})
        if (b->styleSheet() != button)
            b->setStyleSheet(button);
    QPalette lp = m_pageLabel->palette();
    lp.setColor(QPalette::WindowText, t.dim);
    m_pageLabel->setPalette(lp);
    m_zoomLabel->setPalette(lp);
}

} // namespace folia
