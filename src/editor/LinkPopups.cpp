#include "editor/LinkPopups.h"

#include "core/LucideIcons.h"
#include "editor/EditorContext.h"

#include <QApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>

namespace folia {

namespace {

struct PopupColors {
    QColor bg, text, dim, accent, border, hover, field;
};

PopupColors colorsFor(const EditorTheme &t)
{
    PopupColors c;
    const bool light = t.background.lightness() > 128;
    c.bg = light ? t.background : t.background.lighter(135);
    c.text = t.text;
    c.dim = t.dim;
    c.accent = t.accent;
    c.border = t.dim;
    c.border.setAlpha(light ? 95 : 120); // no drop shadow (offscreen blur on every repaint): a firmer border
    c.hover = t.text;
    c.hover.setAlpha(light ? 20 : 34);
    c.field = light ? t.background.darker(103) : t.background.lighter(150);
    return c;
}

QString rgba(const QColor &c)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

} // namespace

// ============================================================ bubble =====
LinkBubble::LinkBubble(QWidget *parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("folia-link-bubble"));
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_StyledBackground, true);
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(10, 5, 6, 5);
    row->setSpacing(2);
    m_icon = new QLabel(this);
    m_icon->setFixedSize(16, 16);
    row->addWidget(m_icon);
    row->addSpacing(4);
    m_url = new QToolButton(this);
    m_url->setObjectName(QStringLiteral("url"));
    m_url->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_url->setFocusPolicy(Qt::NoFocus);
    m_url->setCursor(Qt::PointingHandCursor);
    m_url->setToolTip(tr("Open link"));
    connect(m_url, &QToolButton::clicked, this, &LinkBubble::openRequested);
    row->addWidget(m_url);
    row->addSpacing(6);
    m_sep = new QWidget(this);
    m_sep->setObjectName(QStringLiteral("sep"));
    m_sep->setFixedSize(1, 18);
    m_sep->setAttribute(Qt::WA_StyledBackground, true);
    row->addWidget(m_sep);
    row->addSpacing(4);
    m_open = iconButton(QStringLiteral("external-link"), tr("Open link (⌘-click a link)"));
    m_copy = iconButton(QStringLiteral("copy"), tr("Copy link address"));
    m_edit = iconButton(QStringLiteral("pencil"), tr("Edit link (⌘K)"));
    m_remove = iconButton(QStringLiteral("unlink"), tr("Remove link"));
    connect(m_open, &QToolButton::clicked, this, &LinkBubble::openRequested);
    connect(m_copy, &QToolButton::clicked, this, &LinkBubble::copyRequested);
    connect(m_edit, &QToolButton::clicked, this, &LinkBubble::editRequested);
    connect(m_remove, &QToolButton::clicked, this, &LinkBubble::removeRequested);
    for (QToolButton *b : {m_open, m_copy, m_edit, m_remove})
        row->addWidget(b);
    hide();
}

QToolButton *LinkBubble::iconButton(const QString &icon, const QString &tip)
{
    auto *b = new QToolButton(this);
    b->setProperty("lucide", icon);
    b->setToolTip(tip);
    b->setAccessibleName(tip);
    b->setFocusPolicy(Qt::NoFocus);
    b->setCursor(Qt::PointingHandCursor);
    b->setIconSize(QSize(15, 15));
    b->setFixedSize(26, 26);
    return b;
}

void LinkBubble::setLink(const QString &href, bool removable)
{
    m_href = href;
    const QFontMetrics fm(m_url->font());
    m_url->setText(fm.elidedText(href, Qt::ElideMiddle, 280));
    m_url->setToolTip(href);
    m_remove->setVisible(removable);
    adjustSize();
}

void LinkBubble::applyTheme(const EditorTheme &t)
{
    const PopupColors c = colorsFor(t);
    setStyleSheet(QStringLiteral(
                      "#folia-link-bubble{background:%1;border:1px solid %2;border-radius:8px;}"
                      "#folia-link-bubble QToolButton{border:none;border-radius:5px;background:transparent;padding:0;}"
                      "#folia-link-bubble QToolButton:hover{background:%3;}"
                      "#folia-link-bubble QToolButton#url{color:%4;padding:3px 4px;}"
                      "#folia-link-bubble QToolButton#url:hover{background:transparent;text-decoration:underline;}"
                      "#folia-link-bubble #sep{background:%2;}")
                      .arg(rgba(c.bg), rgba(c.border), rgba(c.hover), rgba(c.accent)));
    const qreal dpr = devicePixelRatioF();
    m_icon->setPixmap(icons::icon(QStringLiteral("link"), c.dim).pixmap(QSize(14, 14), dpr));
    for (QToolButton *b : {m_open, m_copy, m_edit, m_remove})
        b->setIcon(icons::icon(b->property("lucide").toString(), c.text));
    QFont f = t.body;
    f.setPointSizeF(qMax(11.0, t.body.pointSizeF() - 2));
    m_url->setFont(f);
    if (!m_href.isEmpty())
        setLink(m_href, m_remove->isVisibleTo(this));
}

// ============================================================ editor =====
LinkEditor::LinkEditor(QWidget *parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("folia-link-editor"));
    setAttribute(Qt::WA_StyledBackground, true);
    auto *g = new QGridLayout(this);
    g->setContentsMargins(14, 12, 12, 12);
    g->setHorizontalSpacing(10);
    g->setVerticalSpacing(8);
    m_textLabel = new QLabel(tr("Text"), this);
    m_urlLabel = new QLabel(tr("Link"), this);
    m_text = new QLineEdit(this);
    m_text->setPlaceholderText(tr("Text to display"));
    m_url = new QLineEdit(this);
    m_url->setPlaceholderText(tr("Paste or type a link"));
    m_apply = new QPushButton(tr("Apply"), this);
    m_apply->setObjectName(QStringLiteral("apply"));
    m_apply->setCursor(Qt::PointingHandCursor);
    m_apply->setEnabled(false);
    m_text->setMinimumWidth(240);
    g->addWidget(m_textLabel, 0, 0);
    g->addWidget(m_text, 0, 1);
    g->addWidget(m_urlLabel, 1, 0);
    g->addWidget(m_url, 1, 1);
    g->addWidget(m_apply, 1, 2);
    connect(m_url, &QLineEdit::textChanged, this,
            [this](const QString &t) { m_apply->setEnabled(!t.trimmed().isEmpty()); });
    connect(m_apply, &QPushButton::clicked, this, &LinkEditor::submit);
    m_text->installEventFilter(this);
    m_url->installEventFilter(this);
    // Clicking anywhere else (the page, the sidebar…) dismisses the form.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        if (isVisible() && now && now != this && !isAncestorOf(now))
            emit cancelled();
    });
    hide();
}

void LinkEditor::start(const QString &text, const QString &href)
{
    m_text->setText(text);
    m_url->setText(href);
    m_apply->setEnabled(!href.trimmed().isEmpty());
    adjustSize();
    show();
    raise();
    QLineEdit *first = text.isEmpty() ? m_text : m_url;
    first->setFocus(Qt::PopupFocusReason);
    first->selectAll();
}

QString LinkEditor::text() const { return m_text->text(); }
QString LinkEditor::href() const { return m_url->text(); }

void LinkEditor::submit()
{
    if (m_url->text().trimmed().isEmpty()) {
        m_url->setFocus();
        return;
    }
    emit applied(m_text->text(), m_url->text());
}

bool LinkEditor::eventFilter(QObject *o, QEvent *e)
{
    if (e->type() == QEvent::KeyPress && (o == m_text || o == m_url)) {
        auto *k = static_cast<QKeyEvent *>(e);
        if (k->key() == Qt::Key_Escape) {
            emit cancelled();
            return true;
        }
        if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
            if (o == m_text && m_url->text().trimmed().isEmpty())
                m_url->setFocus(); // Google Docs flow: Text, then Link
            else
                submit();
            return true;
        }
    }
    return QFrame::eventFilter(o, e);
}

void LinkEditor::applyTheme(const EditorTheme &t)
{
    const PopupColors c = colorsFor(t);
    QColor accentText = c.accent.lightness() > 150 ? QColor(20, 20, 20) : QColor(255, 255, 255);
    QColor disabled = c.dim;
    disabled.setAlpha(90);
    setStyleSheet(QStringLiteral(
                      "#folia-link-editor{background:%1;border:1px solid %2;border-radius:10px;}"
                      "#folia-link-editor QLabel{color:%3;}"
                      "#folia-link-editor QLineEdit{background:%4;color:%5;border:1px solid %2;border-radius:6px;"
                      "padding:5px 7px;selection-background-color:%8;}"
                      "#folia-link-editor QLineEdit:focus{border:1px solid %6;}"
                      "#folia-link-editor QPushButton#apply{background:%6;color:%7;border:none;border-radius:6px;"
                      "padding:5px 14px;font-weight:600;}"
                      "#folia-link-editor QPushButton#apply:disabled{background:%9;}")
                      .arg(rgba(c.bg), rgba(c.border), rgba(c.dim), rgba(c.field), rgba(c.text), rgba(c.accent),
                           rgba(accentText), rgba(t.selection), rgba(disabled)));
    QFont f = t.body;
    f.setPointSizeF(qMax(11.0, t.body.pointSizeF() - 1.5));
    for (QWidget *w : {static_cast<QWidget *>(m_text), static_cast<QWidget *>(m_url), static_cast<QWidget *>(m_apply),
                       static_cast<QWidget *>(m_textLabel), static_cast<QWidget *>(m_urlLabel)})
        w->setFont(f);
}

} // namespace folia
