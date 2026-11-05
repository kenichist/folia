#include "app/SettingsPage.h"

#include "app/AppController.h"
#include "app/Preferences.h"
#include "core/Instrument.h"
#include "core/LucideIcons.h"
#include "editor/EditorContext.h"
#include "shell/CommandRegistry.h"
#include "shell/ShellHost.h"
#include "storage/Store.h"

#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QColorDialog>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QUrl>

namespace folia {

namespace {

int s_lastTab = 0; // reopen on the section used last (per process)

// "font.size" -> m["font"]["size"]; one level of nesting is all a theme has.
QVariant pathValue(const QVariantMap &m, const QString &path)
{
    const int dot = path.indexOf(QLatin1Char('.'));
    if (dot < 0)
        return m.value(path);
    return m.value(path.left(dot)).toMap().value(path.mid(dot + 1));
}

void setPath(QVariantMap &m, const QString &path, const QVariant &v)
{
    const int dot = path.indexOf(QLatin1Char('.'));
    if (dot < 0) {
        m.insert(path, v);
        return;
    }
    QVariantMap sub = m.value(path.left(dot)).toMap();
    sub.insert(path.mid(dot + 1), v);
    m.insert(path.left(dot), sub);
}

void removePath(QVariantMap &m, const QString &path)
{
    const int dot = path.indexOf(QLatin1Char('.'));
    if (dot < 0) {
        m.remove(path);
        return;
    }
    const QString head = path.left(dot);
    QVariantMap sub = m.value(head).toMap();
    sub.remove(path.mid(dot + 1));
    if (sub.isEmpty())
        m.remove(head);
    else
        m.insert(head, sub);
}

// "15 pt" rather than "15.0 pt" / "15,0 pt"; "1.05" keeps what it needs.
class TrimmedSpinBox : public QDoubleSpinBox {
public:
    using QDoubleSpinBox::QDoubleSpinBox;
    QString textFromValue(double v) const override
    {
        QString s = locale().toString(v, 'f', decimals());
        const QString dp = locale().decimalPoint();
        if (s.contains(dp)) {
            while (s.endsWith(QLatin1Char('0')))
                s.chop(1);
            if (s.endsWith(dp))
                s.chop(dp.size());
        }
        return s;
    }
};

QLabel *noteLabel(const QString &text = {})
{
    auto *l = new QLabel(text);
    l->setWordWrap(true);
    l->setForegroundRole(QPalette::PlaceholderText); // the theme's dim color
    QFont f = l->font();
    f.setPointSizeF(f.pointSizeF() - 1);
    l->setFont(f);
    return l;
}

QString modeName(const QString &mode)
{
    if (mode == QLatin1String("dark"))
        return SettingsPage::tr("Dark");
    if (mode == QLatin1String("system"))
        return SettingsPage::tr("Match System");
    return SettingsPage::tr("Light");
}

QFormLayout *newForm(QWidget *page)
{
    auto *form = new QFormLayout(page);
    form->setContentsMargins(18, 16, 18, 16);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(10);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // Controls keep their size hint; columns holding wrapped notes fill the
    // row (expanding), so their labels wrap at the real width.
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    return form;
}

QWidget *column(int topMargin = 0)
{
    auto *w = new QWidget;
    w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(0, topMargin, 0, 0);
    v->setSpacing(4);
    return w;
}

const QStringList kAppearancePaths = {
    QStringLiteral("mode"), QStringLiteral("colors.accent"), QStringLiteral("font.family"),
    QStringLiteral("font.size"), QStringLiteral("mono.family"), QStringLiteral("mono.size"),
};
const QStringList kEditorPaths = {
    QStringLiteral("editor.max_width"), QStringLiteral("editor.line_height"),
    QStringLiteral("editor.paragraph_spacing"), QStringLiteral("editor.margin"),
};

} // namespace

// ------------------------------------------------------ font families -----
FontFamilyCombo::FontFamilyCombo(bool monospaceOnly, QWidget *parent) : QComboBox(parent), m_mono(monospaceOnly)
{
    addItem(QString(), QString());
    setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    setMinimumContentsLength(24);
    setMaxVisibleItems(20);
}

void FontFamilyCombo::setDefaultLabel(const QString &label)
{
    setItemText(0, label);
}

void FontFamilyCombo::setFamily(const QString &family)
{
    if (family.isEmpty()) {
        setCurrentIndex(0);
        return;
    }
    int i = findData(family);
    if (i < 0) {
        addItem(family, family); // not listed yet (list not filled, or not installed)
        i = count() - 1;
    }
    setCurrentIndex(i);
}

QString FontFamilyCombo::family() const
{
    return currentData().toString();
}

qint64 FontFamilyCombo::populate()
{
    if (m_populated)
        return 0;
    m_populated = true;
    QElapsedTimer t;
    t.start();
    const QString current = family();
    const QString label = itemText(0);
    QSignalBlocker block(this);
    clear();
    addItem(label, QString());
    QStringList families;
    for (const QString &f : QFontDatabase::families()) {
        if (!f.startsWith(QLatin1Char('.')) && !QFontDatabase::isPrivateFamily(f))
            families << f;
    }
    families.removeDuplicates();
    if (m_mono) {
        // Likely monospaced families first, by name: asking every family
        // whether it is fixed-pitch loads each one (~300 ms, +4 MB measured).
        static const QStringList hints = {
            QStringLiteral("Mono"), QStringLiteral("Code"), QStringLiteral("Courier"), QStringLiteral("Menlo"),
            QStringLiteral("Monaco"), QStringLiteral("Consol"), QStringLiteral("Terminal"), QStringLiteral("Fixed"),
            QStringLiteral("Typewriter"), QStringLiteral("Hack"), QStringLiteral("Iosevka"),
        };
        QStringList mono, other;
        for (const QString &f : std::as_const(families)) {
            bool hit = false;
            for (const QString &h : hints)
                hit = hit || f.contains(h, Qt::CaseInsensitive);
            (hit ? mono : other) << f;
        }
        for (const QString &f : std::as_const(mono))
            addItem(f, f);
        if (!mono.isEmpty() && !other.isEmpty())
            insertSeparator(count());
        families = other;
    }
    for (const QString &f : std::as_const(families))
        addItem(f, f);
    setFamily(current);
    const qint64 us = t.nsecsElapsed() / 1000;
    instr::record(m_mono ? "settings_mono_list_us" : "settings_font_list_us", us);
    return us;
}

void FontFamilyCombo::showPopup()
{
    populate();
    QComboBox::showPopup();
}

// --------------------------------------------------------------- page -----
SettingsPage::SettingsPage(AppController *app, QWidget *parent)
    : QWidget(parent), m_app(app), m_over(app->themeOverrides())
{
    setObjectName(QStringLiteral("folia-settings"));
    setAutoFillBackground(true); // the theme's background (application palette)
    m_applyTimer.setSingleShot(true);
    m_applyTimer.setInterval(16); // at most one theme apply per frame while a value is held/dragged
    connect(&m_applyTimer, &QTimer::timeout, this, &SettingsPage::applyPending);

    // Header: the way back to the notes, and the page title.
    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("settings.header"));
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(8, 6, 16, 6);
    hl->setSpacing(6);
    m_back = new QToolButton(header);
    m_back->setObjectName(QStringLiteral("settings.back"));
    m_back->setAutoRaise(true);
    m_back->setText(tr("Notes"));
    m_back->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_back->setIconSize(QSize(16, 16));
    m_back->setToolTip(tr("Back to your notes (Esc)"));
    m_back->setAccessibleName(tr("Back to notes"));
    connect(m_back, &QToolButton::clicked, this, [this] { m_app->closeSettings(); });
    auto *title = new QLabel(tr("Settings"), header);
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.35);
    tf.setWeight(QFont::DemiBold);
    title->setFont(tf);
    hl->addWidget(m_back);
    hl->addSpacing(6);
    hl->addWidget(title);
    hl->addStretch(1);

    // Body: the section list and the selected section.
    m_nav = new QListWidget(this);
    m_nav->setObjectName(QStringLiteral("settings.sections"));
    m_nav->setFrameShape(QFrame::NoFrame);
    m_nav->setFixedWidth(212);
    m_nav->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_nav->setSpacing(1);
    m_sections = new QStackedWidget(this);
    const std::pair<QString, QWidget *> sections[] = {
        {tr("General"), buildGeneral()},
        {tr("Appearance"), buildAppearance()},
        {tr("Editor"), buildEditor()},
        {tr("Layout"), buildLayout()},
    };
    for (const auto &[name, content] : sections) {
        auto *item = new QListWidgetItem(name, m_nav);
        item->setSizeHint(QSize(0, 30));
        // Each section scrolls on its own and keeps a readable width.
        auto *scroll = new QScrollArea;
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->viewport()->setAutoFillBackground(false);
        auto *holder = new QWidget;
        holder->setAutoFillBackground(false);
        auto *h = new QHBoxLayout(holder);
        h->setContentsMargins(24, 12, 24, 24);
        content->setMaximumWidth(680);
        h->addWidget(content, 1, Qt::AlignTop | Qt::AlignLeft);
        h->addStretch(0);
        scroll->setWidget(holder);
        m_sections->addWidget(scroll);
    }
    connect(m_nav, &QListWidget::currentRowChanged, this, [this](int i) {
        if (i < 0)
            return;
        m_sections->setCurrentIndex(i);
        s_lastTab = i;
    });
    m_nav->setCurrentRow(qBound(0, s_lastTab, m_nav->count() - 1));

    auto *divider = new QFrame(this);
    divider->setFrameShape(QFrame::HLine);
    divider->setFrameShadow(QFrame::Plain);
    divider->setForegroundRole(QPalette::Mid);
    auto *body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addWidget(m_nav);
    body->addWidget(m_sections, 1);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(header);
    lay->addWidget(divider);
    lay->addLayout(body, 1);

    for (const QKeySequence &k : {QKeySequence(Qt::Key_Escape), QKeySequence(QKeySequence::Close)}) {
        auto *sc = new QShortcut(k, this);
        sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(sc, &QShortcut::activated, this, [this] { m_app->closeSettings(); });
    }
    setFocusProxy(m_nav); // opening Settings puts the keyboard on the section list
    m_nav->viewport()->installEventFilter(this); // paintCount(): its paints count too
    // The section list and back button follow the theme (refresh()): flat,
    // rounded, in the layout's sidebar / selection / hover colors.
    m_refreshers.push_back([this] {
        const EditorTheme &t = m_app->editorContext()->theme();
        ShellHost *shell = m_app->shell();
        const auto named = [shell, &t](const char *name, const QColor &fallback) {
            const QString c = shell->namedColor(QString::fromLatin1(name));
            return c.isEmpty() ? fallback.name(QColor::HexArgb) : c;
        };
        m_back->setIcon(icons::icon(QStringLiteral("chevron-left"), t.text));
        m_back->setStyleSheet(QStringLiteral("QToolButton{border:none;border-radius:6px;padding:4px 10px 4px 6px;color:%1;}"
                                             "QToolButton:hover{background-color:%2;}QToolButton:pressed{background-color:%3;}")
                                  .arg(t.text.name(), named("hover_bg", t.dim), named("border", t.dim)));
        m_nav->setStyleSheet(QStringLiteral("QListWidget{background-color:%1;border:none;padding:8px 8px;outline:0;}"
                                            "QListWidget::item{color:%2;padding:0 10px;border-radius:6px;}"
                                            "QListWidget::item:hover{background-color:%3;}"
                                            "QListWidget::item:selected{background-color:%4;color:%2;}")
                                 .arg(named("sidebar", t.background), t.text.name(), named("hover_bg", t.dim),
                                      named("selection_bg", t.selection)));
    });

    // Keep in step with the app: a new layout, an OS light/dark switch.
    // Queued: loaded() fires before the host records the new file path.
    connect(app->shell(), &ShellHost::loaded, this, [this] { refresh(); }, Qt::QueuedConnection);
    connect(app->shell(), &ShellHost::loadFailed, this, [this] { refresh(); }, Qt::QueuedConnection);
    connect(app->editorContext(), &EditorContext::themeChanged, this, [this] { refresh(); });
}

int SettingsPage::sectionCount() const
{
    return m_nav->count();
}

int SettingsPage::section() const
{
    return m_nav->currentRow();
}

void SettingsPage::setSection(int index)
{
    m_nav->setCurrentRow(qBound(0, index, m_nav->count() - 1));
}

void SettingsPage::paintEvent(QPaintEvent *e)
{
    ++m_paints;
    QWidget::paintEvent(e);
}

bool SettingsPage::eventFilter(QObject *o, QEvent *e)
{
    if (e->type() == QEvent::Paint && o == m_nav->viewport())
        ++m_paints;
    return QWidget::eventFilter(o, e);
}

void SettingsPage::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    refresh();
}

qint64 SettingsPage::populateFontLists()
{
    return m_bodyFamily->populate() + m_monoFamily->populate();
}

void SettingsPage::runCommand(const QString &name, const QVariantMap &args)
{
    const CommandResult r = m_app->commands()->dispatchCommand(name, args);
    if (!r.ok && m_layoutStatus)
        m_layoutStatus->setText(r.error);
}

// --------------------------------------------------- override plumbing ----
QVariant SettingsPage::over(const QString &path) const
{
    return pathValue(m_over, path);
}

void SettingsPage::setOver(const QString &path, const QVariant &value)
{
    setPath(m_over, path, value);
    scheduleApply();
}

void SettingsPage::clearOver(const QStringList &paths)
{
    for (const QString &p : paths)
        removePath(m_over, p);
    scheduleApply();
}

void SettingsPage::scheduleApply()
{
    if (!m_applyTimer.isActive())
        m_applyTimer.start();
}

void SettingsPage::applyPending()
{
    m_applyTimer.stop();
    if (m_over != m_app->themeOverrides())
        m_app->setThemeOverrides(m_over); // emits themeChanged -> refresh()
    refresh();
}

void SettingsPage::setTitlebarInset(int left, int height)
{
    QWidget *header = m_back->parentWidget();
    header->layout()->setContentsMargins(left > 0 ? left : 8, 6, 16, 6);
    header->setMinimumHeight(height);
}

void SettingsPage::refresh()
{
    if (!isVisible())
        return; // no work while hidden; showEvent() refreshes
    m_refreshing = true;
    for (const auto &f : m_refreshers)
        f();
    const QColor dim = m_app->editorContext()->theme().dim;
    for (const Reset &r : m_resets) {
        r.button->setIcon(icons::icon(QStringLiteral("rotate-ccw"), dim));
        r.button->setVisible(over(r.path).isValid());
    }
    m_refreshing = false;
}

QWidget *SettingsPage::withReset(QWidget *control, const QString &path, const QString &what)
{
    auto *row = new QWidget;
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(4);
    h->addWidget(control);
    auto *reset = new QToolButton(row);
    reset->setObjectName(control->objectName() + QStringLiteral(".reset"));
    reset->setAutoRaise(true);
    reset->setIconSize(QSize(14, 14));
    reset->setToolTip(tr("Use the layout's %1").arg(what));
    reset->setAccessibleName(reset->toolTip());
    QSizePolicy sp = reset->sizePolicy();
    sp.setRetainSizeWhenHidden(true); // rows do not shift when it appears
    reset->setSizePolicy(sp);
    connect(reset, &QToolButton::clicked, this, [this, path] { clearOver({path}); });
    m_resets.push_back({reset, path});
    h->addWidget(reset);
    h->addStretch(1);
    return row;
}

void SettingsPage::addRow(QFormLayout *form, const QString &label, QWidget *control, QWidget *row)
{
    auto *l = new QLabel(label);
    l->setBuddy(control);
    form->addRow(l, row ? row : control);
}

QWidget *SettingsPage::doubleSpin(const QString &path, double min, double max, double step, int decimals,
                                    const QString &suffix, std::function<double()> effective, const QString &what)
{
    auto *s = new TrimmedSpinBox;
    s->setObjectName(QStringLiteral("settings.") + path);
    s->setRange(min, max);
    s->setSingleStep(step);
    s->setDecimals(decimals);
    s->setSuffix(suffix);
    s->setKeyboardTracking(false); // typed values apply on Enter / focus out, not per keystroke
    s->setAccelerated(true);
    connect(s, &QDoubleSpinBox::valueChanged, this, [this, path](double v) {
        if (!m_refreshing)
            setOver(path, v);
    });
    m_refreshers.push_back([s, effective] { s->setValue(effective()); });
    return withReset(s, path, what);
}

QWidget *SettingsPage::intSpin(const QString &path, int min, int max, int step, const QString &suffix,
                                 std::function<double()> effective, const QString &what, const QString &special)
{
    auto *s = new QSpinBox;
    s->setObjectName(QStringLiteral("settings.") + path);
    s->setRange(min, max);
    s->setSingleStep(step);
    s->setSuffix(suffix);
    if (!special.isEmpty())
        s->setSpecialValueText(special);
    s->setKeyboardTracking(false);
    s->setAccelerated(true);
    connect(s, &QSpinBox::valueChanged, this, [this, path](int v) {
        if (!m_refreshing)
            setOver(path, v);
    });
    m_refreshers.push_back([s, effective] { s->setValue(qRound(effective())); });
    return withReset(s, path, what);
}

// ------------------------------------------------------------- General ----
QWidget *SettingsPage::buildGeneral()
{
    auto *page = new QWidget;
    QFormLayout *form = newForm(page);
    Store *store = m_app->store();

    auto *restore = new QCheckBox(tr("Reopen the last page when Folia starts"));
    restore->setObjectName(QStringLiteral("settings.restore_last_page"));
    restore->setChecked(prefs::flag(store->setting(prefs::kRestoreLastPage)));
    connect(restore, &QCheckBox::toggled, this, [store](bool on) {
        store->setSetting(prefs::kRestoreLastPage, prefs::flagValue(on));
    });
    form->addRow(tr("Startup:"), restore);

    const QString home = QDir::toNativeSeparators(m_app->homeDir());
    auto *path = new QLabel;
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    path->setText(path->fontMetrics().elidedText(home, Qt::ElideMiddle, 340));
    path->setToolTip(home);
#ifdef Q_OS_MACOS
    auto *reveal = new QPushButton(tr("Show in Finder"));
#else
    auto *reveal = new QPushButton(tr("Open Folder"));
#endif
    connect(reveal, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_app->homeDir()));
    });
    QWidget *dataCol = column();
    auto *v = static_cast<QVBoxLayout *>(dataCol->layout());
    v->setSpacing(6);
    v->addWidget(path);
    v->addWidget(noteLabel(tr("Your pages (workspace.db) and the editable layout files (shell/).")));
    v->addWidget(reveal, 0, Qt::AlignLeft);
    form->addRow(tr("Data folder:"), dataCol);

    form->addRow(tr("Version:"), new QLabel(tr("Folia %1").arg(QStringLiteral(FOLIA_VERSION))));
    return page;
}

// ---------------------------------------------------------- Appearance ----
QWidget *SettingsPage::buildAppearance()
{
    auto *page = new QWidget;
    QFormLayout *form = newForm(page);
    ShellHost *shell = m_app->shell();
    EditorContext *ectx = m_app->editorContext();

    m_mode = new QComboBox;
    m_mode->setObjectName(QStringLiteral("settings.mode"));
    m_mode->addItem(QString(), QString());
    m_mode->addItem(tr("Light"), QStringLiteral("light"));
    m_mode->addItem(tr("Dark"), QStringLiteral("dark"));
    m_mode->addItem(tr("Match System"), QStringLiteral("system"));
    connect(m_mode, &QComboBox::activated, this, [this](int i) {
        const QString v = m_mode->itemData(i).toString();
        if (v.isEmpty())
            clearOver({QStringLiteral("mode")});
        else
            setOver(QStringLiteral("mode"), v);
    });
    m_modeNote = noteLabel();
    QWidget *modeCol = column();
    auto *mv = static_cast<QVBoxLayout *>(modeCol->layout());
    mv->addWidget(m_mode, 0, Qt::AlignLeft);
    mv->addWidget(m_modeNote);
    addRow(form, tr("Appearance:"), m_mode, modeCol);
    m_refreshers.push_back([this, shell] {
        const QVariantMap layout = shell->layoutTheme();
        m_mode->setItemText(0, tr("Layout's setting (%1)").arg(modeName(layout.value(QStringLiteral("mode")).toString())));
        m_mode->setCurrentIndex(qMax(0, m_mode->findData(over(QStringLiteral("mode")).toString())));
        QVariantMap colors = layout.value(QStringLiteral("colors")).toMap();
        colors.remove(QStringLiteral("accent"));
        m_modeNote->setText(colors.isEmpty() ? QString()
                                             : tr("This layout sets some of its own colors, so Light and Dark "
                                                  "change only the others."));
        m_modeNote->setVisible(!colors.isEmpty());
    });

    m_accent = new QToolButton;
    m_accent->setObjectName(QStringLiteral("settings.colors.accent"));
    m_accent->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_accent->setIconSize(QSize(28, 16));
    connect(m_accent, &QToolButton::clicked, this, &SettingsPage::pickAccent);
    addRow(form, tr("Accent color:"), m_accent, withReset(m_accent, QStringLiteral("colors.accent"), tr("accent color")));
    m_refreshers.push_back([this, ectx] {
        const QColor c = ectx->theme().accent;
        const qreal dpr = devicePixelRatioF();
        QPixmap pm(QSize(28, 16) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(c.darker(125));
        p.setBrush(c);
        p.drawRoundedRect(QRectF(0.5, 0.5, 27, 15), 4, 4);
        p.end();
        m_accent->setIcon(QIcon(pm));
        m_accent->setText(c.name());
    });

    auto fontRows = [&](const QString &key, bool mono, FontFamilyCombo *&combo, const QString &familyLabel,
                        const QString &sizeLabel) {
        combo = new FontFamilyCombo(mono);
        combo->setObjectName(QStringLiteral("settings.%1.family").arg(key));
        FontFamilyCombo *c = combo;
        connect(c, &QComboBox::activated, this, [this, c, key](int) {
            const QString f = c->family();
            if (f.isEmpty())
                clearOver({key + QStringLiteral(".family")});
            else
                setOver(key + QStringLiteral(".family"), f);
        });
        addRow(form, familyLabel, c);
        m_refreshers.push_back([this, c, key, mono, shell] {
            QString fam = pathValue(shell->layoutTheme(), key + QStringLiteral(".family")).toString();
            if (fam.isEmpty())
                fam = mono ? QFontDatabase::systemFont(QFontDatabase::FixedFont).family() : tr("System font");
            c->setDefaultLabel(tr("Layout's font (%1)").arg(fam));
            c->setFamily(over(key + QStringLiteral(".family")).toString());
        });
        QWidget *size = doubleSpin(key + QStringLiteral(".size"), 8, 40, 1, 1, tr(" pt"), [ectx, mono] {
            return mono ? ectx->theme().mono.pointSizeF() : ectx->theme().body.pointSizeF();
        }, mono ? tr("code size") : tr("text size"));
        addRow(form, sizeLabel, size->findChild<QDoubleSpinBox *>(), size);
    };
    fontRows(QStringLiteral("font"), false, m_bodyFamily, tr("Text font:"), tr("Text size:"));
    fontRows(QStringLiteral("mono"), true, m_monoFamily, tr("Code font:"), tr("Code size:"));

    m_resetAppearance = new QPushButton(tr("Use Layout's Appearance"));
    m_resetAppearance->setObjectName(QStringLiteral("settings.reset_appearance"));
    connect(m_resetAppearance, &QPushButton::clicked, this, [this] { clearOver(kAppearancePaths); });
    auto *about = noteLabel();
    about->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_refreshers.push_back([this, about, shell] {
        bool any = false;
        for (const QString &p : kAppearancePaths)
            any = any || over(p).isValid();
        m_resetAppearance->setEnabled(any);
        about->setText(tr("These choices sit on top of the layout file (“%1”) and never change it.")
                           .arg(shell->activeName()));
    });
    form->addRow(QString(), about);
    form->addRow(QString(), m_resetAppearance);
    return page;
}

void SettingsPage::pickAccent()
{
    const QVariant before = over(QStringLiteral("colors.accent"));
    auto *dlg = new QColorDialog(m_app->editorContext()->theme().accent, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(tr("Accent Color"));
    connect(dlg, &QColorDialog::currentColorChanged, this, [this](const QColor &c) {
        if (c.isValid())
            setOver(QStringLiteral("colors.accent"), c.name()); // live preview
    });
    connect(dlg, &QColorDialog::colorSelected, this, [this](const QColor &c) {
        if (c.isValid())
            setOver(QStringLiteral("colors.accent"), c.name());
    });
    connect(dlg, &QDialog::rejected, this, [this, before] {
        if (before.isValid())
            setOver(QStringLiteral("colors.accent"), before);
        else
            clearOver({QStringLiteral("colors.accent")});
    });
    dlg->open();
}

// -------------------------------------------------------------- Editor ----
QWidget *SettingsPage::buildEditor()
{
    auto *page = new QWidget;
    QFormLayout *form = newForm(page);
    EditorContext *ectx = m_app->editorContext();
    auto rowFor = [&](const QString &label, QWidget *row) {
        addRow(form, label, row->findChild<QAbstractSpinBox *>(), row);
    };
    rowFor(tr("Text width:"), intSpin(QStringLiteral("editor.max_width"), 0, 2400, 20, tr(" px"),
                                       [ectx] { return ectx->theme().maxColumnWidth; }, tr("text width"), tr("Full width")));
    rowFor(tr("Line height:"), doubleSpin(QStringLiteral("editor.line_height"), 1.0, 2.5, 0.05, 2, QString(),
                                          [ectx] { return ectx->theme().lineSpacing; }, tr("line height")));
    rowFor(tr("Paragraph spacing:"), intSpin(QStringLiteral("editor.paragraph_spacing"), 0, 48, 1, tr(" px"),
                                             [ectx] { return ectx->theme().paragraphSpacing; }, tr("paragraph spacing")));
    rowFor(tr("Side margin:"), intSpin(QStringLiteral("editor.margin"), 4, 400, 4, tr(" px"),
                                       [ectx] { return ectx->theme().sideMargin; }, tr("side margin")));

    m_resetEditor = new QPushButton(tr("Use Layout's Editor Settings"));
    m_resetEditor->setObjectName(QStringLiteral("settings.reset_editor"));
    connect(m_resetEditor, &QPushButton::clicked, this, [this] { clearOver(kEditorPaths); });
    m_refreshers.push_back([this] {
        bool any = false;
        for (const QString &p : kEditorPaths)
            any = any || over(p).isValid();
        m_resetEditor->setEnabled(any);
    });
    form->addRow(QString(), m_resetEditor);

    // Math: the renderer is loaded at startup, so this applies on the next launch.
    m_math = new QCheckBox(tr("Render math formulas"));
    m_math->setObjectName(QStringLiteral("settings.math"));
    m_mathNote = noteLabel();
    Store *store = m_app->store();
    m_math->setChecked(prefs::flag(store->setting(prefs::kMath)));
    auto updateNote = [this] {
#ifdef FOLIA_WITH_MATH
        if (m_app->mathDisabledByFlag()) {
            m_math->setEnabled(false);
            m_mathNote->setText(tr("Turned off for this run by --no-math."));
        } else {
            const bool loaded = m_app->previews() != nullptr;
            m_mathNote->setText(m_math->isChecked() != loaded ? tr("Takes effect the next time Folia starts.")
                                                              : tr("When off, formulas show only their source."));
        }
#else
        m_math->setEnabled(false);
        m_mathNote->setText(tr("This build does not include the formula renderer."));
#endif
    };
    connect(m_math, &QCheckBox::toggled, this, [store, updateNote](bool on) {
        store->setSetting(prefs::kMath, prefs::flagValue(on));
        updateNote();
    });
    updateNote();
    QWidget *mathCol = column(8);
    auto *mv = static_cast<QVBoxLayout *>(mathCol->layout());
    mv->addWidget(m_math);
    mv->addWidget(m_mathNote);
    form->addRow(tr("Math:"), mathCol);
    return page;
}

// -------------------------------------------------------------- Layout ----
QWidget *SettingsPage::buildLayout()
{
    auto *page = new QWidget;
    QFormLayout *form = newForm(page);
    ShellHost *shell = m_app->shell();

    auto *intro = new QLabel(tr("Folia's whole window — sidebar, toolbar, shortcuts and theme — is a Lua file "
                                "you can edit in any text editor. Saving the file applies it right away; if it "
                                "has an error, the last working layout stays."));
    intro->setWordWrap(true);
    intro->setMinimumWidth(420);
    form->addRow(intro);

    m_layout = new QComboBox;
    m_layout->setObjectName(QStringLiteral("settings.layout"));
    connect(m_layout, &QComboBox::activated, this, [this](int i) {
        const QString name = m_layout->itemData(i).toString();
        if (!name.isEmpty() && name != m_app->activeLayout())
            runCommand(QStringLiteral("shell.use"), {{QStringLiteral("name"), name}});
    });
    addRow(form, tr("Layout:"), m_layout);

    m_layoutPath = new QLabel;
    m_layoutPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_layoutStatus = noteLabel();
    m_layoutStatus->setObjectName(QStringLiteral("settings.layout_status"));
    QWidget *fileCol = column();
    auto *fv = static_cast<QVBoxLayout *>(fileCol->layout());
    fv->addWidget(m_layoutPath);
    fv->addWidget(m_layoutStatus);
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(6);
    auto button = [&](const QString &text, const QString &command) {
        auto *b = new QPushButton(text);
        connect(b, &QPushButton::clicked, this, [this, command] { runCommand(command); });
        buttons->addWidget(b);
    };
    button(tr("Edit Layout File"), QStringLiteral("shell.edit"));
    button(tr("Open Shell Folder"), QStringLiteral("shell.open_folder"));
    button(tr("Reload"), QStringLiteral("shell.reload"));
    buttons->addStretch(1);
    fv->addLayout(buttons);
    form->addRow(tr("File:"), fileCol);

    m_live = new QCheckBox(tr("Reload the layout when its file is saved"));
    m_live->setObjectName(QStringLiteral("settings.live_reload"));
    m_live->setChecked(m_app->liveReload());
    connect(m_live, &QCheckBox::toggled, this, [this](bool on) { m_app->setLiveReload(on); });
    QWidget *liveCol = column();
    auto *lv = static_cast<QVBoxLayout *>(liveCol->layout());
    lv->addWidget(m_live);
    if (m_app->liveReloadDisabledByFlag()) {
        m_live->setEnabled(false);
        lv->addWidget(noteLabel(tr("Turned off for this run by --no-watch.")));
    } else if (m_app->safeMode()) {
        lv->addWidget(noteLabel(tr("Safe mode does not watch files; this applies from the next launch.")));
    }
    form->addRow(tr("Live reload:"), liveCol);

    auto *more = new QHBoxLayout;
    more->setSpacing(6);
    auto *reference = new QPushButton(tr("Shell Language Reference"));
    connect(reference, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(
            QDir(m_app->userShellDir()).filePath(QStringLiteral("REFERENCE.md"))));
    });
    auto *reset = new QPushButton(tr("Reset to Default Layout…"));
    connect(reset, &QPushButton::clicked, this, &SettingsPage::confirmResetLayout);
    more->addWidget(reference);
    more->addWidget(reset);
    more->addStretch(1);
    form->addRow(QString(), more);

    m_refreshers.push_back([this] { refreshLayout(); });
    Q_UNUSED(shell);
    return page;
}

void SettingsPage::refreshLayout()
{
    ShellHost *shell = m_app->shell();
    const QString active = m_app->activeLayout();
    m_layout->clear();
    if (active.isEmpty()) {
        const QString other = m_app->safeMode() ? tr("Built-in default (safe mode)")
                                                : QFileInfo(shell->activePath()).fileName();
        m_layout->addItem(other, QString());
    }
    for (const QString &name : m_app->availableLayouts())
        m_layout->addItem(name, name);
    m_layout->setCurrentIndex(qMax(0, m_layout->findData(active.isEmpty() ? QString() : active)));

    const QString path = QDir::toNativeSeparators(shell->activePath());
    m_layoutPath->setText(m_layoutPath->fontMetrics().elidedText(path, Qt::ElideMiddle, 380));
    m_layoutPath->setToolTip(path);
    QString status;
    if (!shell->lastError().isEmpty())
        status = tr("Error: %1 The last working layout is still active.")
                     .arg(shell->lastError().section(QLatin1Char('\n'), 0, 0).left(200));
    else if (!shell->warnings().isEmpty())
        status = tr("“%1” loaded with %n warning(s); see the banner in the main window.", nullptr,
                    int(shell->warnings().size())).arg(shell->activeName());
    else
        status = tr("“%1” is active.").arg(shell->activeName());
    m_layoutStatus->setText(status);
}

void SettingsPage::confirmResetLayout()
{
    auto *box = new QMessageBox(QMessageBox::Warning, tr("Reset to Default Layout"),
                                tr("Replace your default.lua with the original default layout?"),
                                QMessageBox::NoButton, this);
    box->setInformativeText(tr("Your current copy is kept as default.lua.bak in the shell folder."));
    QPushButton *ok = box->addButton(tr("Reset"), QMessageBox::DestructiveRole);
    QPushButton *cancel = box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(cancel);
    box->setEscapeButton(cancel);
    box->setWindowModality(Qt::WindowModal);
    box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box, &QMessageBox::finished, this, [this, box, ok](int) {
        if (box->clickedButton() == ok)
            runCommand(QStringLiteral("shell.reset"));
    });
    box->open();
}

} // namespace folia
