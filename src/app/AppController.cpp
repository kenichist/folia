#include "app/AppController.h"

#include "app/Platform.h"
#include "app/Preferences.h"
#include "app/SettingsPage.h"
#include "core/Instrument.h"
#include "core/LucideIcons.h"
#include "editor/EditorContext.h"
#include "editor/EditorPane.h"
#include "editor/NoteEditor.h"
#include "editor/PageSession.h"
#include "editor/PreviewService.h"
#include "editor/VirtualLayout.h"
#include "shell/CommandRegistry.h"
#include "shell/DataContext.h"
#include "shell/ShellHost.h"
#include "storage/Store.h"

#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTextDocument>
#include <QUrl>

namespace folia {

namespace {

class MainWindow : public QMainWindow {
public:
    std::function<bool()> canClose;

protected:
    void closeEvent(QCloseEvent *e) override
    {
        if (canClose && !canClose()) {
            e->ignore();
            return;
        }
        e->accept();
    }
};

ArgSpec arg(const char *name, QMetaType::Type t, bool required = false)
{
    return ArgSpec{QString::fromLatin1(name), t, required};
}

CommandResult okResult()
{
    CommandResult r;
    r.ok = true;
    return r;
}

CommandResult fail(const QString &e)
{
    CommandResult r;
    r.error = e;
    return r;
}

} // namespace

AppController::AppController(const Options &opt, QObject *parent) : QObject(parent), m_opt(opt) {}

AppController::~AppController()
{
    if (!m_shutDown)
        shutdown(false);
    delete m_shell; // references widgets owned by the window
    m_shell = nullptr;
    delete m_window;
}

PageId AppController::currentPage() const
{
    return m_active ? m_active->pageId() : 0;
}

QString AppController::userShellDir() const
{
    return QDir(m_opt.homeDir).filePath(QStringLiteral("shell"));
}

QString AppController::builtinPresetPath(const QString &name) const
{
    const QString rel = name == QLatin1String("default") ? QStringLiteral("default/shell.lua")
                        : name == QLatin1String("keyboard") ? QStringLiteral("keyboard/shell.lua")
                                                            : QStringLiteral("examples/%1/shell.lua").arg(name);
    const QStringList roots = {
#ifdef Q_OS_MACOS
        QCoreApplication::applicationDirPath() + QStringLiteral("/../Resources/presets"),
#endif
        QCoreApplication::applicationDirPath() + QStringLiteral("/presets"),
        QStringLiteral(FOLIA_PRESET_SOURCE_DIR),
    };
    for (const QString &r : roots) {
        const QString p = QDir(r).filePath(rel);
        if (QFileInfo::exists(p))
            return QFileInfo(p).canonicalFilePath();
    }
    return QString();
}

QString AppController::presetPath(const QString &name) const
{
    return QDir(userShellDir()).filePath(name + QStringLiteral(".lua"));
}

void AppController::installUserPresets()
{
    // The editable copies are installed once. When Folia ships a newer
    // version of a file, a copy the user never edited (its contents still
    // match what was installed) is replaced; edited copies are left alone.
    static const QSet<QByteArray> kShippedBefore = {
        // Folia 0.1.0: default, keyboard, right-nav, REFERENCE.md
        "9ae2b2d91f30cbb98c934984338196229c23b364d8e2b99ed87b5f5777858539",
        "936462a873193beb9070e12f025f36b9feef4a6e6745ccb4fb22dc3b8e210215",
        "d90c1d7fb06d897307ba961c62c89d7b329bbd97f4d2386859bc9f56a010c2fa",
        "26d03f8283819b25826128c1307abb023fa4d613f18cf791d3a99ba59bcee08b",
        // Before Settings: default, REFERENCE.md
        "a3d1a7fbd23bc77951db4f828bd4093e11096bc02bf2769c83e32a09ed4742e9",
        "cc40ebbecffb15a668f30e1195f855ca4eb1972f3d19eca859446d8b983539f8",
        // Settings as a separate window: REFERENCE.md
        "89d527fa65c6bf6b10d4102a51b0d53b08d16222c9e2b4112f9438cf8c7c2fce",
        // Before Liquid Glass: default, REFERENCE.md
        "f0ed47d02a24cc7bb9230dd8a17c87b49f34dacb83ac55052316f8f7a0f9f229",
        "1ccdf9fcf65b8331a655f1c236560d3dda6e4632e0e81b84d6193ec3785317ca",
    };
    auto digest = [](const QString &path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return QByteArray();
        return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex();
    };
    auto install = [&](const QString &key, const QString &src, const QString &dst) {
        if (src.isEmpty() || !QFileInfo::exists(src))
            return;
        const QByteArray want = digest(src);
        const QString settingKey = QStringLiteral("shell.installed.") + key;
        if (QFileInfo::exists(dst)) {
            const QByteArray have = digest(dst);
            if (have == want)
                return;
            const QByteArray installed = m_store->setting(settingKey).toLatin1();
            if (have != installed && !kShippedBefore.contains(have))
                return; // edited by the user: keep it
            QFile::remove(dst);
        }
        if (QFile::copy(src, dst)) {
            QFile(dst).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup
                                      | QFileDevice::ReadOther);
            m_store->setSetting(settingKey, QString::fromLatin1(want));
        }
    };
    QDir().mkpath(userShellDir());
    for (const QString &name : {QStringLiteral("default"), QStringLiteral("keyboard"), QStringLiteral("right-nav")})
        install(name, builtinPresetPath(name), presetPath(name));
    const QString refSrc = QFileInfo(builtinPresetPath(QStringLiteral("default"))).absolutePath()
        + QStringLiteral("/../REFERENCE.md");
    install(QStringLiteral("REFERENCE.md"), refSrc, QDir(userShellDir()).filePath(QStringLiteral("REFERENCE.md")));
}

bool AppController::start(QString *error)
{
    if (m_opt.homeDir.isEmpty())
        m_opt.homeDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(m_opt.homeDir);
    const QString dbPath = m_opt.workspacePath.isEmpty()
        ? QDir(m_opt.homeDir).filePath(QStringLiteral("workspace.db")) : m_opt.workspacePath;

    m_store = new Store(this);
    Store::Options so;
    so.path = dbPath;
    so.faultInjectable = m_opt.faultVfs;
    if (!m_store->open(so, error))
        return false;

    m_ectx = new EditorContext(this);
#ifdef FOLIA_WITH_MATH
    if (!m_opt.noMath && prefs::flag(m_store->setting(prefs::kMath))) {
        m_previews = new PreviewService(4 << 20, 1, this);
        m_ectx->previews = m_previews;
    }
#endif
    m_data = new DataContext(m_store, this);
    m_ectx->titleOf = [this](PageId id) {
        if (auto it = m_cache.find(id); it != m_cache.end())
            return (*it)->title();
        return m_data->titleOf(id);
    };
    m_commands = new CommandRegistry(this);

    connect(m_store, &Store::committed, this, [this](PageId page, Revision rev, qint64 micros) {
        instr::record("commit_us", micros);
        if (auto it = m_cache.find(page); it != m_cache.end())
            (*it)->setCommitted(rev);
        // Every edit in a committed batch was dirty at most this long.
        auto &q = m_inflight[page];
        while (!q.isEmpty() && q.first().first <= rev) {
            instr::record("dirty_to_commit_us", q.first().second.nsecsElapsed() / 1000);
            q.removeFirst();
        }
        m_failing = false;
        updateStatus();
        evictCache();
    });
    connect(m_store, &Store::commitFailed, this, [this](PageId, Revision, const StorageError &e) {
        m_failing = true;
        m_failMessage = e.diskFull ? tr("Disk full — your edits are kept in memory and will be saved when space is available")
                                   : tr("Save failed (%1) — edits kept in memory, retrying").arg(e.message);
        updateStatus();
    });
    connect(m_store, &Store::writerRecovered, this, [this] {
        m_failing = false;
        updateStatus();
    });
    connect(m_store, &Store::treeChanged, m_data, &DataContext::notifyTreeChanged);

    m_prefsTimer.setSingleShot(true);
    m_prefsTimer.setInterval(300);
    connect(&m_prefsTimer, &QTimer::timeout, this, &AppController::flushPreferences);
    m_idleTimer.setSingleShot(true);
    m_idleTimer.setInterval(m_opt.autosaveIdleMs);
    m_deadlineTimer.setSingleShot(true);
    m_deadlineTimer.setInterval(m_opt.autosaveMaxDirtyMs);
    connect(&m_idleTimer, &QTimer::timeout, this, &AppController::flushSaves);
    connect(&m_deadlineTimer, &QTimer::timeout, this, &AppController::flushSaves);

    platform::onMemoryPressure([this](bool critical) { reclaimMemory(critical); });

    buildWindow();
    registerCommands();
    installUserPresets();

    if (m_opt.seedIfEmpty && m_store->pageCount() == 0)
        seedWorkspace();

    // Shell: explicit file > safe mode builtin > active preset.
    QString shellFile;
    if (m_opt.safeMode) {
        shellFile = builtinPresetPath(QStringLiteral("default"));
        m_shell->setSafeModeNotice(tr("Safe mode: using the built-in default layout. Your shell files are untouched; "
                                      "fix them and choose Shell ▸ Reload Layout."));
    } else if (!m_opt.shellPath.isEmpty()) {
        shellFile = m_opt.shellPath;
    } else {
        const QString preset = m_opt.preset.isEmpty()
            ? m_store->setting(QStringLiteral("shell.active"), QStringLiteral("default")) : m_opt.preset;
        shellFile = presetPath(preset);
        if (!QFileInfo::exists(shellFile))
            shellFile = presetPath(QStringLiteral("default"));
    }
    m_shell->setWatchFile(m_opt.watchShell && !m_opt.safeMode && liveReload());
    // Settings overrides sit on top of whichever layout loads.
    m_shell->setThemeOverrides(prefs::parseThemeOverrides(m_store->setting(prefs::kThemeOverrides)));
    QString shellErr;
    if (!loadShell(shellFile, &shellErr)) {
        // No valid layout yet: fall back to the built-in default so the editor
        // is always reachable; keep the error visible.
        QString e2;
        const QString builtin = builtinPresetPath(QStringLiteral("default"));
        if (builtin.isEmpty() || !m_shell->loadFile(builtin, &e2)) {
            if (error)
                *error = tr("No usable shell layout: %1 / %2").arg(shellErr, e2);
            return false;
        }
        m_shell->setSafeModeNotice(tr("Your layout %1 failed to load:\n%2\nUsing the built-in default until it is fixed "
                                      "(saved changes reload automatically).").arg(QFileInfo(shellFile).fileName(), shellErr.left(600)));
        m_shell->setWatchFile(false);
    }

    // Open the last page (or the first root page).
    const PageId last = m_opt.startPage > 0 ? m_opt.startPage
        : m_opt.restoreLastPage && prefs::flag(m_store->setting(prefs::kRestoreLastPage))
            ? m_store->setting(QStringLiteral("app.last_page")).toLongLong() : 0;
    if (last > 0) {
        m_store->pageMeta(last, this, [this](PageMeta m, bool found) {
            if (found)
                openPage(m.id);
            else
                m_store->fetchChildren(0, 0, 1, this, [this](QVector<PageMeta> rows, int) {
                    if (!rows.isEmpty())
                        openPage(rows.first().id);
                });
        });
    } else {
        m_store->fetchChildren(0, 0, 1, this, [this](QVector<PageMeta> rows, int) {
            if (!rows.isEmpty())
                openPage(rows.first().id);
        });
    }
    return true;
}

void AppController::buildWindow()
{
    auto *w = new MainWindow;
    m_window = w;
    w->setWindowTitle(QStringLiteral("Folia"));
    w->resize(1180, 780);
    w->canClose = [this] { return shutdown(true); };
    m_stack = new QStackedWidget(w);
    w->setCentralWidget(m_stack);
    m_shellContainer = new QWidget(m_stack);
    m_stack->addWidget(m_shellContainer);
    m_pane = new EditorPane(m_ectx, m_shellContainer);

    ShellHost::Deps deps;
    deps.editor = m_pane;
    deps.editorCtx = m_ectx;
    deps.commands = m_commands;
    deps.data = m_data;
    deps.window = w;
    deps.container = m_shellContainer;
    m_shell = new ShellHost(deps, this);
    connect(m_shell, &ShellHost::resetRequested, this, &AppController::resetShell);
    connect(m_pane->editor(), &NoteEditor::openPageRequested, this, [this](PageId id) { openPage(id); });
    connect(m_pane->editor(), &NoteEditor::openUrlRequested, this, [](const QString &href) {
        QUrl u(href, QUrl::TolerantMode);
        if (u.scheme().isEmpty())
            u = QUrl::fromUserInput(href);
        if (u.isValid())
            QDesktopServices::openUrl(u);
    });
    connect(m_ectx, &EditorContext::themeChanged, this, [this] {
        for (PageSession *s : std::as_const(m_cache))
            if (s->layout())
                s->layout()->relayoutAll();
    });
    connect(m_pane->editor(), &NoteEditor::painted, this, [this] {
        if (m_awaitPaint && m_active && m_active->pageId() == m_openTarget) {
            m_awaitPaint = false;
            const qint64 us = m_openClock.nsecsElapsed() / 1000;
            instr::record("open_to_paint_us", us);
            emit pagePainted(m_openTarget, us);
        }
    });

    // Native menus: always available, independent of the Lua layout.
    // Shortcuts are owned by the Lua layout (so users can rebind them); the
    // menu only keeps editing keys and the recovery keys reserved by the host.
    QMenuBar *mb = w->menuBar();
    auto act = [this](QMenu *menu, const QString &text, const QString &cmd, const QKeySequence &ks = {},
                      const QVariantMap &args = {}) {
        QAction *a = menu->addAction(text);
        if (!ks.isEmpty())
            a->setShortcut(ks);
        connect(a, &QAction::triggered, this, [this, cmd, args] {
            const CommandResult r = m_commands->dispatchCommand(cmd, args);
            if (!r.ok)
                qWarning() << r.error;
        });
        return a;
    };
    QMenu *file = mb->addMenu(tr("&File"));
    act(file, tr("New Page"), QStringLiteral("page.new"));
    act(file, tr("New Child Page"), QStringLiteral("page.new_child"));
    act(file, tr("New Sibling Page"), QStringLiteral("page.new_sibling"));
    file->addSeparator();
    act(file, tr("Move Page Up"), QStringLiteral("page.move_up"));
    act(file, tr("Move Page Down"), QStringLiteral("page.move_down"));
    act(file, tr("Indent Page"), QStringLiteral("page.indent"));
    act(file, tr("Outdent Page"), QStringLiteral("page.outdent"));
    file->addSeparator();
    act(file, tr("Delete Page…"), QStringLiteral("page.delete"), {}, {{QStringLiteral("confirm"), true}});
    file->addSeparator();
    // macOS moves this to the application menu (Folia ▸ Settings…).
    QAction *settings = file->addAction(tr("Settings…"));
    settings->setMenuRole(QAction::PreferencesRole);
    settings->setShortcut(QKeySequence(QStringLiteral("Ctrl+,"))); // Mod+, (reserved for layouts)
    settings->setShortcutContext(Qt::ApplicationShortcut);
    connect(settings, &QAction::triggered, this, &AppController::showSettings);
    QMenu *edit = mb->addMenu(tr("&Edit"));
    act(edit, tr("Undo"), QStringLiteral("editor.undo"), QKeySequence::Undo);
    act(edit, tr("Redo"), QStringLiteral("editor.redo"), QKeySequence::Redo);
    edit->addSeparator();
    act(edit, tr("Copy Code Block"), QStringLiteral("code.copy_block"));
    act(edit, tr("Insert or Edit Link…"), QStringLiteral("link.edit"));
    QMenu *go = mb->addMenu(tr("&Go"));
    act(go, tr("Back"), QStringLiteral("nav.back"));
    act(go, tr("Forward"), QStringLiteral("nav.forward"));
    act(go, tr("Parent Page"), QStringLiteral("nav.parent"));
    act(go, tr("Focus Title"), QStringLiteral("title.focus"));
    act(go, tr("Focus Editor"), QStringLiteral("editor.focus"));
    QMenu *shell = mb->addMenu(tr("&Shell"));
    act(shell, tr("Reload Layout"), QStringLiteral("shell.reload"), QKeySequence(QStringLiteral("Ctrl+Alt+R")));
    act(shell, tr("Reset to Default Layout"), QStringLiteral("shell.reset"), QKeySequence(QStringLiteral("Ctrl+Alt+Shift+R")));
    shell->addSeparator();
    for (const QString &p : {QStringLiteral("default"), QStringLiteral("keyboard"), QStringLiteral("right-nav")}) {
        QAction *a = shell->addAction(tr("Use “%1” Layout").arg(p));
        connect(a, &QAction::triggered, this, [this, p] {
            QVariantMap args;
            args.insert(QStringLiteral("name"), p);
            m_commands->dispatchCommand(QStringLiteral("shell.use"), args);
        });
    }
    shell->addSeparator();
    act(shell, tr("Edit Layout File"), QStringLiteral("shell.edit"));
    act(shell, tr("Open Shell Folder"), QStringLiteral("shell.open_folder"));
}

bool AppController::loadShell(const QString &path, QString *error)
{
    QString err;
    const bool ok = m_shell->loadFile(path, &err);
    if (error)
        *error = err;
    return ok;
}

void AppController::resetShell()
{
    // Restore the user's default preset from the built-in copy (keeping a
    // backup), then load it. Always reachable from the native Shell menu.
    const QString builtin = builtinPresetPath(QStringLiteral("default"));
    const QString user = presetPath(QStringLiteral("default"));
    if (QFileInfo::exists(user)) {
        const QString bak = user + QStringLiteral(".bak");
        QFile::remove(bak);
        QFile::rename(user, bak);
    }
    QFile::copy(builtin, user);
    QFile(user).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    m_store->setSetting(QStringLiteral("shell.active"), QStringLiteral("default"));
    m_shell->setSafeModeNotice(QString());
    m_shell->setWatchFile(m_opt.watchShell && liveReload());
    QString err;
    if (!m_shell->loadFile(user, &err))
        m_shell->loadFile(builtin, &err);
}

// ------------------------------------------------------------ settings ----
void AppController::showSettings()
{
    if (!m_settings) {
        QElapsedTimer t;
        t.start();
        m_settings = new SettingsPage(this, m_stack);
        m_stack->addWidget(m_settings);
        instr::record("settings_create_us", t.nsecsElapsed() / 1000);
    }
    if (const int h = m_shell->titlebarHeight(); h > 0)
        m_settings->setTitlebarInset(ShellHost::kTitlebarInset, h);
    else
        m_settings->setTitlebarInset(0, 0);
    if (m_stack->currentWidget() != m_settings) {
        if (m_active)
            m_pane->editor()->storeViewState();
        m_shell->setShortcutsEnabled(false); // Mod+N etc. would act on the hidden page
        m_stack->setCurrentWidget(m_settings);
        updateWindowTitle();
    }
    m_settings->setFocus(Qt::OtherFocusReason); // the section list (focus proxy)
}

void AppController::closeSettings()
{
    if (!m_settings)
        return;
    m_settings->applyPending();
    flushPreferences();
    SettingsPage *page = m_settings;
    m_settings = nullptr;
    m_stack->setCurrentWidget(m_shellContainer);
    m_stack->removeWidget(page);
    page->deleteLater(); // nothing of it stays while closed
    m_shell->setShortcutsEnabled(true);
    updateWindowTitle();
    if (m_active)
        m_pane->focusBody();
}

bool AppController::settingsOpen() const
{
    return m_settings && m_stack->currentWidget() == m_settings;
}

SettingsPage *AppController::settingsPage() const
{
    return m_settings.data();
}

void AppController::updateWindowTitle()
{
    if (settingsOpen())
        m_window->setWindowTitle(tr("Settings — Folia"));
    else if (m_active)
        m_window->setWindowTitle((m_active->title().isEmpty() ? tr("Untitled") : m_active->title())
                                 + QStringLiteral(" — Folia"));
    else
        m_window->setWindowTitle(QStringLiteral("Folia"));
}

void AppController::setThemeOverrides(const QVariantMap &overrides)
{
    m_shell->setThemeOverrides(overrides);
    m_themeDirty = true;
    m_prefsTimer.start(); // one write for a burst of changes
}

QVariantMap AppController::themeOverrides() const
{
    return m_shell->themeOverrides();
}

void AppController::flushPreferences()
{
    m_prefsTimer.stop();
    if (!m_themeDirty || m_shutDown)
        return;
    m_themeDirty = false;
    m_store->setSetting(prefs::kThemeOverrides, prefs::serializeThemeOverrides(m_shell->themeOverrides()));
}

bool AppController::liveReload() const
{
    return prefs::flag(m_store->setting(prefs::kLiveReload));
}

void AppController::setLiveReload(bool on)
{
    m_store->setSetting(prefs::kLiveReload, prefs::flagValue(on));
    // Safe mode keeps its built-in layout unwatched until the next launch.
    if (!m_opt.safeMode)
        m_shell->setWatchFile(on && m_opt.watchShell);
}

QStringList AppController::availableLayouts() const
{
    QStringList names;
    const QFileInfoList files = QDir(userShellDir()).entryInfoList({QStringLiteral("*.lua")}, QDir::Files, QDir::Name);
    for (const QFileInfo &fi : files)
        names << fi.completeBaseName();
    if (names.removeAll(QStringLiteral("default")))
        names.prepend(QStringLiteral("default"));
    return names;
}

QString AppController::activeLayout() const
{
    const QFileInfo fi(m_shell->activePath());
    if (fi.fileName().isEmpty() || QDir(fi.absolutePath()) != QDir(userShellDir()))
        return QString();
    return fi.completeBaseName();
}

// --------------------------------------------------------------- pages ----
PageSession *AppController::createSession(PageId id)
{
    auto *s = new PageSession(id, m_ectx, [this] { return m_store->allocateBlockId(); }, this);
    connect(s, &PageSession::revisionChanged, this, [this, s] { scheduleSave(s); });
    connect(s, &PageSession::committedChanged, this, &AppController::updateStatus);
    connect(s, &PageSession::titleChanged, this, [this, s](const QString &t) {
        m_data->noteTitle(s->pageId(), t);
        for (PageSession *o : std::as_const(m_cache))
            if (o != s)
                o->refreshLinkTo(s->pageId());
        if (s == m_active)
            updateWindowTitle();
        Q_UNUSED(t);
    });
    return s;
}

void AppController::touchCache(PageId id)
{
    m_lru.removeAll(id);
    m_lru.prepend(id);
}

qint64 AppController::docCacheBytes() const
{
    qint64 n = 0;
    for (PageSession *s : std::as_const(m_cache))
        n += s->estimatedBytes();
    return n;
}

void AppController::evictCache()
{
    qint64 bytes = docCacheBytes();
    for (int i = m_lru.size() - 1; i >= 0 && (bytes > m_opt.docCacheBytes || m_cache.size() > m_opt.docCacheEntries); --i) {
        const PageId id = m_lru[i];
        PageSession *s = m_cache.value(id);
        // Pinned: the active page, and anything not yet durably committed.
        if (!s || s == m_active || s->isUncommitted() || s->hasUnsnapshotted())
            continue;
        bytes -= s->estimatedBytes();
        m_cache.remove(id);
        m_lru.removeAt(i);
        s->deleteLater();
    }
    instr::set(instr::DocCacheBytes, bytes);
    instr::set(instr::DocCacheEntries, m_cache.size());
}

void AppController::openPage(PageId id, bool focusEditor)
{
    if (id <= 0)
        return;
    // Going to a page (menu, command, link) leaves Settings; the first page
    // opened at startup, which arrives asynchronously, does not.
    if (m_active && settingsOpen())
        closeSettings();
    if (m_active && m_active->pageId() == id) {
        if (focusEditor)
            m_pane->focusBody();
        return;
    }
    m_openClock.start();
    m_openTarget = id;
    m_awaitPaint = true;
    const qint64 started = m_openClock.nsecsElapsed();
    if (m_active && !m_navigatingHistory) {
        m_back.push_back(m_active->pageId());
        if (m_back.size() > 200)
            m_back.removeFirst();
        m_forward.clear();
    }
    if (m_active)
        m_pane->editor()->storeViewState();
    flushSaves(); // the page being left is snapshotted now, not after the timer
    if (PageSession *s = m_cache.value(id)) {
        activate(s, started, true, focusEditor);
        return;
    }
    m_pendingOpen = id;
    m_store->openPage(id, this, [this, id, focusEditor, started](PageContent pc) {
        if (m_pendingOpen != id)
            return; // superseded by a newer navigation
        m_pendingOpen = 0;
        if (!pc.found)
            return;
        PageSession *s = m_cache.value(id);
        if (!s) {
            s = createSession(id);
            s->load(pc);
            m_cache.insert(id, s);
        }
        activate(s, started, false, focusEditor);
    });
}

void AppController::activate(PageSession *s, qint64 startedNs, bool cached, bool focusEditor)
{
    m_active = s;
    touchCache(s->pageId());
    m_pane->setSession(s);
    m_data->noteTitle(s->pageId(), s->title());
    updateWindowTitle();
    if (focusEditor)
        m_pane->focusBody();
    const qint64 us = (m_openClock.nsecsElapsed() - startedNs) / 1000;
    instr::record(cached ? "open_cached_activate_us" : "open_uncached_activate_us", us);
    emit pageActivated(s->pageId(), us, cached);
    m_store->setSetting(QStringLiteral("app.last_page"), QString::number(s->pageId()));
    const PageId id = s->pageId();
    m_store->ancestors(id, this, [this, id](QVector<PageMeta> chain) {
        if (m_active && m_active->pageId() == id) {
            if (!chain.isEmpty())
                chain.last().title = m_active->title();
            m_data->setCurrent(id, chain);
        }
    });
    updateStatus();
    evictCache();
}

PageId AppController::newPage(PageId parent, int index, const QString &title, bool focusTitle)
{
    instr::Scope scope("page_new_us");
    if (settingsOpen())
        closeSettings();
    QElapsedTimer t;
    t.start();
    const bool parentHeld = parent != 0 && m_cache.contains(parent);
    const PageId id = m_store->createPage(parent, index, title, /*linkInParent=*/parent != 0 && !parentHeld);
    if (parentHeld) {
        PageSession *ps = m_cache.value(parent);
        if (ps == m_active)
            ps->insertPageLinkAfter(m_pane->editor()->textCursor().position(), id);
        else
            ps->appendPageLink(id);
    }
    m_data->noteTitle(id, title);
    if (m_active)
        m_pane->editor()->storeViewState();
    flushSaves();
    PageSession *s = createSession(id);
    s->initNew(parent, title);
    m_cache.insert(id, s);
    if (m_active && !m_navigatingHistory) {
        m_back.push_back(m_active->pageId());
        m_forward.clear();
    }
    m_openClock.start();
    m_openTarget = id;
    m_awaitPaint = true;
    activate(s, 0, true, !focusTitle);
    if (focusTitle)
        m_pane->focusTitle(true);
    instr::record("page_new_to_editable_title_us", t.nsecsElapsed() / 1000);
    scheduleSave(s);
    return id;
}

// ------------------------------------------------------------ autosave ----
void AppController::scheduleSave(PageSession *s)
{
    if (!m_dirtySince.contains(s->pageId())) {
        QElapsedTimer t;
        t.start();
        m_dirtySince.insert(s->pageId(), t);
    }
    if (m_autosave) {
        m_idleTimer.start(); // coalesce bursts
        if (!m_deadlineTimer.isActive())
            m_deadlineTimer.start(); // but never stay dirty longer than ~1 s
    }
    updateStatus();
}

void AppController::flushSaves()
{
    m_idleTimer.stop();
    m_deadlineTimer.stop();
    if (m_active)
        m_pane->editor()->storeViewState();
    for (PageSession *s : std::as_const(m_cache)) {
        if (!s->hasUnsnapshotted())
            continue;
        SaveBatch b = s->takeSaveBatch();
        if (b.rev == 0)
            continue;
        auto d = m_dirtySince.find(s->pageId());
        if (d != m_dirtySince.end()) {
            m_inflight[s->pageId()].push_back({b.rev, *d});
            m_dirtySince.erase(d);
        }
        m_store->commit(std::move(b));
    }
}

void AppController::updateStatus()
{
    QString st;
    if (m_failing)
        st = m_failMessage;
    else if (!m_active)
        st = QString();
    else if (m_active->committedRevision() >= m_active->revision() && !m_active->hasUnsnapshotted())
        st = tr("Saved");
    else
        st = tr("Saving…");
    m_data->notifyStatus(st);
    if (st == tr("Saved"))
        emit allSaved();
}

bool AppController::shutdown(bool interactive)
{
    if (m_shutDown)
        return true;
    flushPreferences();
    flushSaves();
    bool ok = m_store->flush(4000);
    while (!ok && interactive) {
        QMessageBox box(m_window);
        box.setIcon(QMessageBox::Warning);
        box.setText(tr("Some edits could not be saved."));
        box.setInformativeText(m_failMessage + tr("\n\nFree some disk space and retry, or keep Folia open so "
                                                  "your text stays recoverable."));
        QPushButton *retry = box.addButton(tr("Retry"), QMessageBox::AcceptRole);
        QPushButton *keep = box.addButton(tr("Keep Folia Open"), QMessageBox::RejectRole);
        box.addButton(tr("Quit and Lose Unsaved Edits"), QMessageBox::DestructiveRole);
        box.setDefaultButton(retry);
        box.exec();
        if (box.clickedButton() == static_cast<QAbstractButton *>(keep))
            return false;
        if (box.clickedButton() != static_cast<QAbstractButton *>(retry))
            break;
        ok = m_store->flush(4000);
    }
    m_shutDown = true;
    m_store->close();
    return true;
}

void AppController::reclaimMemory(bool critical)
{
    for (int i = m_lru.size() - 1; i >= 0; --i) {
        PageSession *s = m_cache.value(m_lru[i]);
        if (!s || s == m_active || s->isUncommitted() || s->hasUnsnapshotted())
            continue;
        m_cache.remove(m_lru[i]);
        m_lru.removeAt(i);
        s->deleteLater();
    }
    if (m_previews)
        m_previews->trim(critical ? 0 : (1 << 20));
    icons::clearCache();
    if (m_active && m_active->layout())
        m_active->layout()->setLayoutCap(critical ? 150 : 300);
    instr::set(instr::DocCacheEntries, m_cache.size());
}

QSet<PageId> AppController::heldPages() const
{
    QSet<PageId> s;
    for (auto it = m_cache.cbegin(); it != m_cache.cend(); ++it)
        s.insert(it.key());
    return s;
}

PageId AppController::argId(const QVariantMap &args) const
{
    const PageId id = args.value(QStringLiteral("id")).toLongLong();
    return id > 0 ? id : currentPage();
}

void AppController::afterMove(PageId id, PageId oldParent, PageId newParent)
{
    if (oldParent == newParent)
        return;
    if (PageSession *op = m_cache.value(oldParent))
        op->removePageLinks(id);
    if (PageSession *np = m_cache.value(newParent))
        np->appendPageLink(id);
    if (PageSession *s = m_cache.value(id))
        s->setParentId(newParent);
    if (m_active && m_active->pageId() == id)
        m_store->ancestors(id, this, [this, id](QVector<PageMeta> chain) {
            if (m_active && m_active->pageId() == id)
                m_data->setCurrent(id, chain);
        });
}

void AppController::moveRelative(PageId id, int delta)
{
    if (!id)
        return;
    flushSaves();
    m_store->siblingIndex(id, this, [this, id, delta](PageId parent, int index, int count) {
        const int target = index + delta;
        if (index < 0 || target < 0 || target >= count)
            return;
        m_store->movePage(id, parent, target, heldPages(), this, [](MoveResult, PageId) {});
    });
}

void AppController::indent(PageId id)
{
    if (!id)
        return;
    flushSaves();
    m_store->siblingIndex(id, this, [this, id](PageId parent, int index, int) {
        if (index <= 0)
            return;
        m_store->fetchChildren(parent, index - 1, 1, this, [this, id, parent](QVector<PageMeta> rows, int) {
            if (rows.isEmpty())
                return;
            const PageId newParent = rows.first().id;
            m_store->movePage(id, newParent, -1, heldPages(), this, [this, id, newParent](MoveResult r, PageId old) {
                if (r == MoveResult::Ok) {
                    afterMove(id, old, newParent);
                    QStringList ids = m_store->setting(QStringLiteral("tree.expanded")).split(QLatin1Char(','));
                    Q_UNUSED(ids);
                    emit m_data->revealRequested(id);
                }
            });
        });
        Q_UNUSED(parent);
    });
}

void AppController::outdent(PageId id)
{
    if (!id)
        return;
    flushSaves();
    m_store->pageMeta(id, this, [this, id](PageMeta m, bool found) {
        if (!found || m.parent == 0)
            return;
        const PageId parent = m.parent;
        m_store->pageMeta(parent, this, [this, id, parent](PageMeta pm, bool pf) {
            if (!pf)
                return;
            m_store->siblingIndex(parent, this, [this, id, pm](PageId gp, int pidx, int) {
                m_store->movePage(id, gp, pidx + 1, heldPages(), this, [this, id, gp](MoveResult r, PageId old) {
                    if (r == MoveResult::Ok) {
                        afterMove(id, old, gp);
                        emit m_data->revealRequested(id);
                    }
                });
                Q_UNUSED(pm);
            });
        });
    });
}

// Asks before deleting (asynchronously: commands may run inside a Lua
// handler, which must not spin a nested event loop).
void AppController::confirmDelete(PageId id)
{
    flushSaves();
    m_store->pageMeta(id, this, [this, id](PageMeta m, bool found) {
        if (!found)
            return;
        const QString title = m.title.trimmed().isEmpty() ? tr("Untitled") : m.title;
        auto *box = new QMessageBox(QMessageBox::Warning, tr("Delete Page"), tr("Delete “%1”?").arg(title),
                                    QMessageBox::NoButton, m_window);
        if (m.childCount == 1)
            box->setInformativeText(tr("Its subpage will be deleted too."));
        else if (m.childCount > 1)
            box->setInformativeText(tr("Its %1 subpages, and everything inside them, will be deleted too.").arg(m.childCount));
        QPushButton *del = box->addButton(tr("Delete"), QMessageBox::DestructiveRole);
        QPushButton *cancel = box->addButton(QMessageBox::Cancel);
        box->setDefaultButton(cancel);
        box->setEscapeButton(cancel);
        box->setWindowModality(Qt::WindowModal);
        box->setAttribute(Qt::WA_DeleteOnClose);
        connect(box, &QMessageBox::finished, this, [this, box, del, id](int) {
            if (box->clickedButton() == del) {
                QVariantMap args;
                args.insert(QStringLiteral("id"), qint64(id));
                m_commands->dispatchCommand(QStringLiteral("page.delete"), args);
            }
        });
        box->open();
    });
}

// ------------------------------------------------------------ commands ----
void AppController::registerCommands()
{
    using T = QMetaType;
    CommandRegistry *c = m_commands;
    c->add({QStringLiteral("page.open"), tr("Open Page"), {}, {arg("id", T::LongLong, true)}, [this](const QVariantMap &a) {
                openPage(a.value(QStringLiteral("id")).toLongLong());
                return okResult();
            }, {}});
    c->add({QStringLiteral("page.new"), tr("New Page"), {}, {arg("parent", T::LongLong), arg("title", T::QString)},
            [this](const QVariantMap &a) {
                newPage(a.value(QStringLiteral("parent")).toLongLong(), -1, a.value(QStringLiteral("title")).toString(), true);
                return okResult();
            }, {}});
    c->add({QStringLiteral("page.new_child"), tr("New Child Page"), {}, {arg("title", T::QString)},
            [this](const QVariantMap &a) {
                if (!currentPage())
                    return fail(tr("no page is open"));
                newPage(currentPage(), -1, a.value(QStringLiteral("title")).toString(), true);
                return okResult();
            }, {}});
    c->add({QStringLiteral("page.new_sibling"), tr("New Sibling Page"), {}, {}, [this](const QVariantMap &) {
                const PageId cur = currentPage();
                if (!cur)
                    return fail(tr("no page is open"));
                m_store->siblingIndex(cur, this, [this](PageId parent, int index, int) {
                    newPage(parent, index + 1, QString(), true);
                });
                return okResult();
            }, {}});
    c->add({QStringLiteral("page.move_up"), tr("Move Page Up"), {}, {arg("id", T::LongLong)}, [this](const QVariantMap &a) {
                moveRelative(argId(a), -1);
                return okResult();
            }, {}});
    c->add({QStringLiteral("page.move_down"), tr("Move Page Down"), {}, {arg("id", T::LongLong)}, [this](const QVariantMap &a) {
                moveRelative(argId(a), +1);
                return okResult();
            }, {}});
    c->add({QStringLiteral("page.indent"), tr("Indent Page"), {}, {arg("id", T::LongLong)}, [this](const QVariantMap &a) {
                indent(argId(a));
                return okResult();
            }, {}});
    c->add({QStringLiteral("page.outdent"), tr("Outdent Page"), {}, {arg("id", T::LongLong)}, [this](const QVariantMap &a) {
                outdent(argId(a));
                return okResult();
            }, {}});
    c->add({QStringLiteral("page.move"), tr("Move Page"), {},
            {arg("id", T::LongLong, true), arg("parent", T::LongLong, true), arg("index", T::LongLong)},
            [this](const QVariantMap &a) {
                const PageId id = a.value(QStringLiteral("id")).toLongLong();
                const PageId np = a.value(QStringLiteral("parent")).toLongLong();
                flushSaves();
                m_store->movePage(id, np, a.value(QStringLiteral("index"), -1).toInt(), heldPages(), this,
                                  [this, id, np](MoveResult r, PageId old) {
                                      if (r == MoveResult::Ok)
                                          afterMove(id, old, np);
                                      else if (r == MoveResult::Cycle)
                                          m_data->notifyStatus(tr("Cannot move a page inside itself"));
                                  });
                return okResult();
            }, {}});
    c->add({QStringLiteral("page.delete"), tr("Delete Page"), {}, {arg("id", T::LongLong), arg("confirm", T::Bool)},
            [this](const QVariantMap &a) {
                const PageId id = argId(a);
                if (!id)
                    return fail(tr("no page"));
                if (a.value(QStringLiteral("confirm")).toBool()) {
                    confirmDelete(id);
                    return okResult();
                }
                flushSaves();
                const bool wasActive = currentPage() == id;
                m_store->trashPage(id, heldPages(), this, [this, id, wasActive](bool ok, PageId parent) {
                    if (!ok)
                        return;
                    if (PageSession *ps = m_cache.value(parent))
                        ps->removePageLinks(id);
                    if (wasActive) {
                        if (parent)
                            openPage(parent);
                        else
                            m_store->fetchChildren(0, 0, 1, this, [this](QVector<PageMeta> rows, int) {
                                if (!rows.isEmpty())
                                    openPage(rows.first().id);
                            });
                    }
                });
                return okResult();
            }, {}});
    c->add({QStringLiteral("link.edit"), tr("Insert or Edit Link"), {}, {}, [this](const QVariantMap &) {
                QString why;
                return m_pane->editor()->editLink(&why) ? okResult() : fail(why);
            }, {}});
    c->add({QStringLiteral("page.reveal"), tr("Reveal Page in Tree"), {}, {arg("id", T::LongLong)}, [this](const QVariantMap &a) {
                emit m_data->revealRequested(argId(a));
                return okResult();
            }, {}});
    c->add({QStringLiteral("nav.back"), tr("Back"), {}, {}, [this](const QVariantMap &) {
                if (m_back.isEmpty())
                    return fail(tr("nothing to go back to"));
                const PageId id = m_back.takeLast();
                if (m_active)
                    m_forward.push_back(m_active->pageId());
                m_navigatingHistory = true;
                openPage(id);
                m_navigatingHistory = false;
                return okResult();
            }, {}});
    c->add({QStringLiteral("nav.forward"), tr("Forward"), {}, {}, [this](const QVariantMap &) {
                if (m_forward.isEmpty())
                    return fail(tr("nothing to go forward to"));
                const PageId id = m_forward.takeLast();
                if (m_active)
                    m_back.push_back(m_active->pageId());
                m_navigatingHistory = true;
                openPage(id);
                m_navigatingHistory = false;
                return okResult();
            }, {}});
    c->add({QStringLiteral("nav.parent"), tr("Open Parent Page"), {}, {}, [this](const QVariantMap &) {
                const auto &anc = m_data->ancestors();
                if (anc.size() < 2)
                    return fail(tr("this page has no parent"));
                openPage(anc[anc.size() - 2].id);
                return okResult();
            }, {}});
    c->add({QStringLiteral("title.focus"), tr("Focus Title"), {}, {}, [this](const QVariantMap &) {
                m_pane->focusTitle();
                return okResult();
            }, {}});
    c->add({QStringLiteral("editor.focus"), tr("Focus Editor"), {}, {}, [this](const QVariantMap &) {
                m_pane->focusBody();
                return okResult();
            }, {}});
    c->add({QStringLiteral("editor.undo"), tr("Undo"), {}, {}, [this](const QVariantMap &) {
                if (settingsOpen())
                    return fail(tr("Settings are open"));
                if (m_pane->titleField()->hasFocus())
                    m_pane->titleField()->undo();
                else
                    m_pane->editor()->undo();
                return okResult();
            }, {}});
    c->add({QStringLiteral("editor.redo"), tr("Redo"), {}, {}, [this](const QVariantMap &) {
                if (settingsOpen())
                    return fail(tr("Settings are open"));
                if (m_pane->titleField()->hasFocus())
                    m_pane->titleField()->redo();
                else
                    m_pane->editor()->redo();
                return okResult();
            }, {}});
    c->add({QStringLiteral("code.copy_block"), tr("Copy Code Block"), {}, {}, [this](const QVariantMap &) {
                return m_pane->editor()->copyCodeBlockAtCursor() ? okResult() : fail(tr("the caret is not in a code block"));
            }, {}});
    c->add({QStringLiteral("shell.reload"), tr("Reload Layout"), {}, {}, [this](const QVariantMap &) {
                // Deferred: may be invoked from a Lua handler of the state it replaces.
                QTimer::singleShot(0, this, [this] {
                    const QString p = m_shell->activePath();
                    if (!p.isEmpty())
                        loadShell(p);
                });
                return okResult();
            }, {}});
    c->add({QStringLiteral("shell.reset"), tr("Reset to Default Layout"), {}, {}, [this](const QVariantMap &) {
                QTimer::singleShot(0, this, &AppController::resetShell);
                return okResult();
            }, {}});
    c->add({QStringLiteral("shell.use"), tr("Use Layout"), {}, {arg("name", T::QString, true)}, [this](const QVariantMap &a) {
                const QString name = a.value(QStringLiteral("name")).toString();
                if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) || name.startsWith(QLatin1Char('.')))
                    return fail(tr("invalid layout name"));
                const QString path = presetPath(name);
                if (!QFileInfo::exists(path))
                    return fail(tr("no layout named %1 in %2").arg(name, userShellDir()));
                QTimer::singleShot(0, this, [this, name, path] {
                    m_shell->setSafeModeNotice(QString());
                    if (loadShell(path))
                        m_store->setSetting(QStringLiteral("shell.active"), name);
                });
                return okResult();
            }, {}});
    c->add({QStringLiteral("app.settings"), tr("Settings"), {}, {}, [this](const QVariantMap &) {
                // Deferred: opening a window from inside a Lua handler would move focus mid-call.
                QTimer::singleShot(0, this, &AppController::showSettings);
                return okResult();
            }, {}});
    c->add({QStringLiteral("shell.edit"), tr("Edit Layout File"), {}, {}, [this](const QVariantMap &) {
                QDesktopServices::openUrl(QUrl::fromLocalFile(m_shell->activePath()));
                return okResult();
            }, {}});
    c->add({QStringLiteral("shell.open_folder"), tr("Open Shell Folder"), {}, {}, [this](const QVariantMap &) {
                QDesktopServices::openUrl(QUrl::fromLocalFile(userShellDir()));
                return okResult();
            }, {}});
}

// --------------------------------------------------------------- seed -----
void AppController::seedWorkspace()
{
    auto block = [this](BlockKind k, const QString &text, const QString &lang = {}, PageId target = 0) {
        BlockRecord r;
        r.id = m_store->allocateBlockId();
        r.kind = k;
        r.text = text;
        r.lang = lang;
        r.target = target;
        return r;
    };
    auto save = [this](PageId page, const QString &title, const QVector<BlockRecord> &blocks) {
        SaveBatch b;
        b.page = page;
        b.rev = 1;
        b.hasTitle = true;
        b.title = title;
        b.upserts = blocks;
        b.hasOrder = true;
        for (const BlockRecord &r : blocks)
            b.order << r.id;
        m_store->commit(b);
    };
    const PageId welcome = m_store->createPage(0, -1, tr("Welcome to Folia"));
    const PageId mathPage = m_store->createPage(welcome, -1, tr("Math"));
    const PageId codePage = m_store->createPage(welcome, -1, tr("Code"));
    const PageId nested = m_store->createPage(mathPage, -1, tr("A nested research note"));
    save(welcome, tr("Welcome to Folia"), {
        block(BlockKind::Text, tr("Folia is a small, local notes app: nested pages, LaTeX math and code blocks.")),
        block(BlockKind::Text, tr("## Basics")),
        block(BlockKind::Text, tr("- Type anywhere. Everything autosaves locally within a second.")),
        block(BlockKind::Text, tr("- Inline math like $e^{i\\pi} + 1 = 0$ renders in place; put the caret on it to edit the source.")),
        block(BlockKind::Text, tr("- Start a line with ``` for code or $$ for a display equation, then press Enter.")),
        block(BlockKind::Text, tr("- Links: select text and press Mod+K, or paste an address onto it. Click a link for "
                                  "Open, Edit or Remove; Mod-click opens it, like [Lucide icons](https://lucide.dev/icons).")),
        block(BlockKind::Text, tr("- Settings (Mod+,) change the theme, fonts, text width and layout without editing any file.")),
        block(BlockKind::Text, tr("- The whole window layout is a Lua file: Shell ▸ Edit Layout File. Save it to see changes.")),
        block(BlockKind::Text, tr("## Child pages")),
        block(BlockKind::PageLink, {}, {}, mathPage),
        block(BlockKind::PageLink, {}, {}, codePage),
    });
    save(mathPage, tr("Math"), {
        block(BlockKind::Text, tr("Display equations render below their source; the source stays editable.")),
        block(BlockKind::Math, QStringLiteral("\\int_0^\\infty e^{-x^2}\\,dx = \\frac{\\sqrt{\\pi}}{2}")),
        block(BlockKind::Math, QStringLiteral("\\begin{aligned} f(x) &= (x+1)^2 \\\\ &= x^2 + 2x + 1 \\end{aligned}")),
        block(BlockKind::Math, QStringLiteral("A = \\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}"), QStringLiteral("single")),
        block(BlockKind::Text, tr("Inline: the roots are $x = \\frac{-b \\pm \\sqrt{b^2-4ac}}{2a}$.")),
        block(BlockKind::PageLink, {}, {}, nested),
    });
    save(codePage, tr("Code"), {
        block(BlockKind::Text, tr("Code blocks are highlighted lazily. Use the Copy button or Mod+Shift+C.")),
        block(BlockKind::Code, QStringLiteral("def fib(n: int) -> int:\n    \"\"\"Return the n-th Fibonacci number.\"\"\"\n"
                                              "    a, b = 0, 1\n    for _ in range(n):\n        a, b = b, a + b\n    return a"),
              QStringLiteral("python")),
        block(BlockKind::Code, QStringLiteral("#include <cstdio>\n\nint main() {\n    std::printf(\"hello\\n\"); // greet\n    return 0;\n}"),
              QStringLiteral("cpp")),
    });
    save(nested, tr("A nested research note"), {
        block(BlockKind::Text, tr("Breadcrumbs above show where this page lives. Mod+[ goes back.")),
        block(BlockKind::Text, tr("Energy: $E = mc^2$, and the Gaussian $\\frac{1}{\\sigma\\sqrt{2\\pi}} e^{-\\frac{(x-\\mu)^2}{2\\sigma^2}}$.")),
    });
    m_store->flush(3000);
}

} // namespace folia
