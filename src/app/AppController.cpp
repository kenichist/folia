#include "app/AppController.h"

#include "app/DocumentImport.h"
#include "app/Platform.h"
#include "import/DocumentKinds.h"
#include "app/Preferences.h"
#include "app/SettingsPage.h"
#include "app/WindowBufferTrimmer.h"
#include "core/Instrument.h"
#include "core/LucideIcons.h"
#include "editor/EditorContext.h"
#include "editor/EditorPane.h"
#include "editor/NoteEditor.h"
#include "editor/PageSession.h"
#include "editor/MediaService.h"
#include "app/PaneSplitter.h"
#include "app/PdfViewerPage.h"
#include "editor/PdfView.h"
#include "editor/PreviewService.h"
#include "storage/AttachmentStore.h"
#include "editor/VirtualLayout.h"
#include "shell/CommandRegistry.h"
#include "shell/DataContext.h"
#include "shell/ShellHost.h"
#include "shell/TabStrip.h"
#include "storage/Store.h"

#include <QAction>
#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QMimeData>
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
    // Files dropped anywhere that does not take them itself (the sidebar,
    // the top bar, the tab strip): uploaded like File ▸ Upload Files….
    std::function<bool(const QString &path)> canUpload;
    std::function<void(const QStringList &paths)> upload;

protected:
    QStringList uploadable(const QMimeData *m) const
    {
        QStringList out;
        if (m && m->hasUrls() && canUpload)
            for (const QUrl &u : m->urls())
                if (u.isLocalFile() && canUpload(u.toLocalFile())) // by suffix: no file I/O while dragging
                    out << u.toLocalFile();
        return out;
    }
    void dragEnterEvent(QDragEnterEvent *e) override
    {
        if (!uploadable(e->mimeData()).isEmpty())
            e->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent *e) override
    {
        if (!uploadable(e->mimeData()).isEmpty())
            e->acceptProposedAction();
    }
    void dropEvent(QDropEvent *e) override
    {
        const QStringList files = uploadable(e->mimeData());
        if (files.isEmpty() || !upload)
            return;
        e->acceptProposedAction();
        upload(files);
    }
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
    // Page sessions (and the VirtualLayouts their documents own) call into
    // the editor context, attachment store and media service as they go.
    // Those are children created before any session, so ~QObject would free
    // them first: delete every session now, including any already handed to
    // deleteLater.
    m_active = nullptr;
    m_cache.clear();
    m_lru.clear();
    qDeleteAll(findChildren<PageSession *>(Qt::FindDirectChildrenOnly));
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
        // Folia 0.2.0: default, keyboard, right-nav, REFERENCE.md
        "f0ed47d02a24cc7bb9230dd8a17c87b49f34dacb83ac55052316f8f7a0f9f229",
        "f69ceef1e654fb98e3d2cb73de2b39e150e51d189fcffdff3a4e641e9c455bf4",
        "596f4b01a811ddbb1694686317feaef12f61dcdca47bb8981da4033d81b231a1",
        "8f89b612dd54571ffbfafe8136ff84eb37c98e5240acad171ea4a00eb52d770d",
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
    // The page to open: read on the reader thread from now, while the window
    // and the layout are built on this one.
    const PageId wanted = m_opt.startPage > 0 ? m_opt.startPage
        : m_opt.restoreLastPage && prefs::flag(m_store->setting(prefs::kRestoreLastPage))
            ? m_store->setting(QStringLiteral("app.last_page")).toLongLong() : 0;
    const bool prefetch = m_opt.prefetchStartPage && !m_store->wasEmptyAtOpen();
    if (prefetch)
        m_store->prefetchStartPage(wanted);

    m_ectx = new EditorContext(this);
#ifdef FOLIA_WITH_MATH
    if (!m_opt.noMath && prefs::flag(m_store->setting(prefs::kMath))) {
        m_previews = new PreviewService(4 << 20, 1, this);
        m_ectx->previews = m_previews;
    }
#endif
    {
        // Attachments sit next to the workspace they belong to (the default
        // workspace: <data folder>/attachments), so two workspaces never
        // sweep each other's files.
        AttachmentStore::Options ao;
        const QFileInfo db(dbPath);
        ao.dir = !m_opt.attachmentsDir.isEmpty() ? m_opt.attachmentsDir
            : db.fileName() == QLatin1String("workspace.db")
                ? db.absolutePath() + QStringLiteral("/attachments")
                : db.absolutePath() + QLatin1Char('/') + db.completeBaseName() + QStringLiteral(".attachments");
        m_attachments = new AttachmentStore(m_store, ao, this);
        MediaService::Config mc;
        if (m_opt.mediaBudgetBytes > 0)
            mc.budgetBytes = m_opt.mediaBudgetBytes;
        if (m_opt.mediaCacheBytes > 0)
            mc.cacheMaxBytes = m_opt.mediaCacheBytes;
        m_media = new MediaService(mc, this);
        m_ectx->attachments = m_attachments;
        m_ectx->media = m_media;
    }
    m_data = new DataContext(m_store, this);
    m_docImport = new DocumentImporter(m_store, m_attachments, this);
    connect(m_docImport, &DocumentImporter::created, this, [this](PageId id, const QString &title) {
        m_data->noteTitle(id, title);
        // The first page of an upload is shown; the rest wait in background tabs.
        const bool background = !m_uploadForeground;
        m_uploadForeground = false;
        if (settingsOpen() && !background)
            closeSettings();
        openInNewTab(id, background);
    });
    connect(m_docImport, &DocumentImporter::failed, this, [this](const QString &file, const QString &error) {
        m_uploadErrors << tr("%1: %2").arg(file, error);
    });
    connect(m_docImport, &DocumentImporter::idle, this, &AppController::reportUploadErrors);
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

    if (m_opt.seedIfEmpty && m_store->wasEmptyAtOpen())
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

    restoreTabs(prefetch);
    return true;
}

// Reopens the saved tabs (or the start page, or the first root page). One
// read query checks them all and fetches their titles; only the shown tab's
// page is loaded.
void AppController::restoreTabs(bool prefetched)
{
    QVector<PageId> pages;
    int active = 0;
    if (m_opt.startPage > 0) {
        pages = {m_opt.startPage};
    } else if (m_opt.restoreLastPage && prefs::flag(m_store->setting(prefs::kRestoreLastPage))) {
        TabList::parse(m_store->setting(QStringLiteral("tabs.state")), &pages, &active);
        // The last page shown picks the active tab (it is written on every
        // page open, so it is also right after a crash or an older version).
        const PageId last = m_store->setting(QStringLiteral("app.last_page")).toLongLong();
        if (last > 0) {
            if (const int i = int(pages.indexOf(last)); i >= 0)
                active = i;
            else if (pages.isEmpty())
                pages = {last};
            else
                pages[active] = last;
        }
    }
    // The page read ahead during startup (Store::prefetchStartPage) is the
    // one the active tab shows, unless that page is gone: then the read-ahead
    // holds the first root page, which the checks below may not choose.
    PageContent startPage;
    const bool have = prefetched && m_store->takeStartPage(&startPage, 500);
    if (have && (pages.isEmpty() || startPage.id == pages.value(active))) {
        // Shown with the window's first paint: built here, made the active
        // page from the window's show event. Otherwise the window paints once
        // without a page and again when the page arrives.
        if (pages.isEmpty())
            pages = {startPage.id};
        m_tabs.reset(pages, active);
        tabsEdited();
        m_data->noteTitle(startPage.id, startPage.title);
        m_startSession = createSession(startPage.id);
        m_startSession->load(startPage);
        m_cache.insert(startPage.id, m_startSession);
        if (m_window->isVisible())
            showStartPage();
        else // from the window's show event; or, never shown, on the next turn
            QTimer::singleShot(0, this, &AppController::showStartPage);
        // The other tabs: titles for the strip, and those whose page is gone.
        if (pages.size() > 1) {
            m_store->pagesMeta(pages, this, [this](QVector<PageMeta> rows) {
                for (const PageMeta &m : std::as_const(rows))
                    m_data->noteTitle(m.id, m.title);
                emit tabsChanged();
            });
            pruneTabs(0);
        }
        return;
    }
    if (pages.isEmpty()) {
        openFirstPage(false);
        return;
    }
    m_store->pagesMeta(pages, this, [this, pages, active](QVector<PageMeta> rows) {
        QHash<PageId, QString> alive;
        for (const PageMeta &m : std::as_const(rows))
            alive.insert(m.id, m.title);
        QVector<PageId> kept;
        int keptActive = 0;
        for (int i = 0; i < pages.size(); ++i) {
            if (!alive.contains(pages[i]))
                continue;
            if (i <= active)
                keptActive = int(kept.size());
            kept << pages[i];
        }
        if (kept.isEmpty()) {
            openFirstPage(false);
            return;
        }
        for (auto it = alive.cbegin(); it != alive.cend(); ++it)
            m_data->noteTitle(it.key(), it.value());
        m_tabs.reset(kept, keptActive);
        tabsEdited();
        m_navigatingHistory = true;
        openPage(m_tabs.current().page);
        m_navigatingHistory = false;
    });
}

void AppController::showStartPage()
{
    PageSession *s = m_startSession;
    m_startSession = nullptr;
    if (!s || m_active || m_pendingOpen || m_tabs.current().page != s->pageId())
        return; // shown already, or another page was asked for meanwhile
    m_navigatingHistory = true; // restoring a tab is not a navigation inside it
    openPage(s->pageId());
    m_navigatingHistory = false;
}

bool AppController::eventFilter(QObject *watched, QEvent *event)
{
    // The window's children have their geometry when its show event arrives,
    // so the page's scroll position can be restored; nothing is painted yet.
    if (watched == m_window && event->type() == QEvent::Show && m_startSession)
        showStartPage();
    return QObject::eventFilter(watched, event);
}

void AppController::openFirstPage(bool replace)
{
    m_store->fetchChildren(0, 0, 1, this, [this, replace](QVector<PageMeta> rows, int) {
        if (rows.isEmpty())
            return;
        m_navigatingHistory = replace;
        openPage(rows.first().id);
        m_navigatingHistory = false;
    });
}

void AppController::buildWindow()
{
    auto *w = new MainWindow;
    m_window = w;
    w->setWindowTitle(QStringLiteral("Folia"));
    w->resize(1180, 780);
    w->canClose = [this] { return shutdown(true); };
    w->canUpload = &AppController::canUpload;
    w->upload = [this](const QStringList &paths) { uploadFiles(paths); };
    w->setAcceptDrops(true);
    w->installEventFilter(this);
    m_bufferTrimmer = new WindowBufferTrimmer(w, this);
    m_bufferTrimmer->setIdleMs(m_opt.idleBufferTrimMs);
    // The menu bar first: it creates the native window (about 45 ms), which
    // needs no font, while the system fonts are still being listed on a
    // worker (main.cpp). The first text widget below then finds them ready.
    QMenuBar *mb = w->menuBar();
    m_stack = new QStackedWidget(w);
    w->setCentralWidget(m_stack);
    m_shellContainer = new QWidget(m_stack);
    m_stack->addWidget(m_shellContainer);
    m_pane = new EditorPane(m_ectx);
    // The layout's editor slot holds the split: the note, and the PDF tab
    // beside it while one is open (one tab: no handle, nothing drawn).
    m_split = new PaneSplitter(m_pane, m_shellContainer);
    m_split->setSide(m_store->setting(QStringLiteral("split.side")) == QLatin1String("left") ? PaneSplitter::Side::Left
                                                                                            : PaneSplitter::Side::Right);
    bool ratioOk = false;
    const double ratio = m_store->setting(QStringLiteral("split.ratio")).toDouble(&ratioOk);
    m_split->setRatio(ratioOk ? ratio : 0.5);
    connect(m_split, &PaneSplitter::ratioCommitted, this, [this](double r) {
        m_store->setSetting(QStringLiteral("split.ratio"), QString::number(r, 'f', 3));
    });

    // The tab strip: one painted widget wherever the layout puts ui.tabs{}.
    m_tabStrip = new TabStrip(m_shellContainer);
    connect(m_tabStrip, &TabStrip::activated, this, &AppController::activateTab);
    connect(m_tabStrip, &TabStrip::closeRequested, this, &AppController::closeTab);
    connect(m_tabStrip, &TabStrip::moved, this, &AppController::moveTab);
    connect(m_tabStrip, &TabStrip::newRequested, this, &AppController::newTab);
    connect(m_tabStrip, &TabStrip::closeOthersRequested, this, [this](int keep) {
        activateTab(keep);
        for (int k = m_tabs.count() - 1; k >= 0; --k)
            if (k != m_tabs.activeIndex())
                closeTab(k); // background tabs: no page work
    });
    connect(m_tabStrip, &TabStrip::closeRightRequested, this, [this](int last) {
        if (m_tabs.activeIndex() > last)
            activateTab(last);
        for (int k = m_tabs.count() - 1; k > last; --k)
            closeTab(k);
    });
    connect(this, &AppController::tabsChanged, this, &AppController::syncTabStrip);
    connect(m_data, &DataContext::titleChanged, this, [this](PageId id, const QString &title) {
        auto it = m_tabTitles.find(id);
        if (it == m_tabTitles.end())
            return;
        *it = title;
        for (int i = 0; i < m_tabs.count(); ++i)
            if (m_tabs.at(i).page == id)
                m_tabStrip->setTitle(m_tabs.at(i).uid, title);
    });

    ShellHost::Deps deps;
    deps.editor = m_pane;
    deps.editorCtx = m_ectx;
    deps.commands = m_commands;
    deps.data = m_data;
    deps.window = w;
    deps.container = m_shellContainer;
    deps.editorSlot = m_split;
    deps.tabs = m_tabStrip;
    m_shell = new ShellHost(deps, this);
    connect(m_shell, &ShellHost::resetRequested, this, &AppController::resetShell);
    connect(m_pane->editor(), &NoteEditor::openPageRequested, this, [this](PageId id) { openPage(id); });
    connect(m_pane->editor(), &NoteEditor::openPdfRequested, this,
            [this](const QString &hash, const QString &name) { showPdf(hash, name); });
    connect(m_pane->editor(), &NoteEditor::documentsDropped, this, &AppController::uploadFiles);
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
        m_split->setLineColor(QColor(m_shell->namedColor(QStringLiteral("border"))));
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
    act(file, tr("New Tab"), QStringLiteral("tab.new"));
    act(file, tr("Close Tab"), QStringLiteral("tab.close"));
    act(file, tr("Reopen Closed Tab"), QStringLiteral("tab.reopen"));
    file->addSeparator();
    act(file, tr("Upload Files…"), QStringLiteral("file.upload"));
    file->addSeparator();
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
    act(edit, tr("Insert Image…"), QStringLiteral("image.insert"));
    act(edit, tr("Insert PDF…"), QStringLiteral("pdf.insert"));
    QMenu *go = mb->addMenu(tr("&Go"));
    act(go, tr("Back"), QStringLiteral("nav.back"));
    act(go, tr("Forward"), QStringLiteral("nav.forward"));
    act(go, tr("Parent Page"), QStringLiteral("nav.parent"));
    go->addSeparator();
    act(go, tr("Next Tab"), QStringLiteral("tab.next"));
    act(go, tr("Previous Tab"), QStringLiteral("tab.prev"));
    go->addSeparator();
    act(go, tr("Focus Title"), QStringLiteral("title.focus"));
    act(go, tr("Focus Editor"), QStringLiteral("editor.focus"));
    QMenu *view = mb->addMenu(tr("&View"));
    act(view, tr("Focus Other Pane"), QStringLiteral("pane.focus_other"));
    act(view, tr("Swap Panes"), QStringLiteral("pane.swap"));
    act(view, tr("Close PDF"), QStringLiteral("pdf.close"));
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
    // A PDF tab stays open under Settings (hidden: it renders nothing, and
    // its document closes after a while like any hidden viewer).
    if (!m_settings) {
        QElapsedTimer t;
        t.start();
        m_settings = new SettingsPage(this, m_stack);
        m_stack->addWidget(m_settings);
        instr::record("settings_create_us", t.nsecsElapsed() / 1000);
    }
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

// ----------------------------------------------------------------- PDF ----
bool AppController::showPdf(const QString &hash, const QString &name)
{
    const AttachmentInfo *info = m_attachments ? m_attachments->info(hash) : nullptr;
    if (!info || !info->isPdf() || info->pages <= 0)
        return false; // not known (yet), or not a PDF
    if (settingsOpen())
        closeSettings();
    if (m_pdf && m_pdf->hash() == hash) {
        m_pdf->setFocus(Qt::OtherFocusReason);
        return true;
    }
    QElapsedTimer t;
    t.start();
    if (m_active)
        m_pane->editor()->storeViewState();
    // At most two tabs: another PDF takes this one's place (its renders,
    // bitmaps and document are released before the new one starts).
    PdfViewerPage *old = m_pdf;
    m_pdf = new PdfViewerPage(this, *info, name.isEmpty() ? tr("Document.pdf") : name, nullptr);
    m_split->setSecondary(m_pdf);
    if (old) {
        old->release();
        old->deleteLater();
    }
    m_pdf->show();
    m_pdf->setFocus(Qt::OtherFocusReason);
    updateWindowTitle();
    instr::record("pdf_tab_open_us", t.nsecsElapsed() / 1000);
    return true;
}

void AppController::closePdf()
{
    if (!m_pdf)
        return;
    const bool hadFocus = pdfHasFocus();
    PdfViewerPage *page = m_pdf;
    m_pdf = nullptr;
    m_split->takeSecondary();
    // Now: its render requests, bitmaps and document. The widget goes on
    // the next turn, since this may run from its own key or button handler.
    page->release();
    page->deleteLater();
    updateWindowTitle();
    if (m_active && !settingsOpen() && (hadFocus || !QApplication::focusWidget()))
        m_pane->focusBody();
}

bool AppController::pdfOpen() const
{
    return m_pdf != nullptr;
}

bool AppController::pdfHasFocus() const
{
    const QWidget *f = QApplication::focusWidget();
    return m_pdf && f && (f == m_pdf || m_pdf->isAncestorOf(f));
}

void AppController::swapPanes()
{
    m_split->swapSides();
    m_store->setSetting(QStringLiteral("split.side"),
                        m_split->side() == PaneSplitter::Side::Left ? QStringLiteral("left") : QStringLiteral("right"));
}

void AppController::focusOtherPane()
{
    if (!m_pdf || settingsOpen())
        return;
    if (pdfHasFocus())
        m_pane->focusBody();
    else
        m_pdf->setFocus(Qt::OtherFocusReason);
}

PdfViewerPage *AppController::pdfViewer() const
{
    return m_pdf.data();
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
    if (m_shutDown)
        return;
    if (m_themeDirty) {
        m_themeDirty = false;
        m_store->setSetting(prefs::kThemeOverrides, prefs::serializeThemeOverrides(m_shell->themeOverrides()));
    }
    if (m_tabsDirty) {
        m_tabsDirty = false;
        m_store->setSetting(QStringLiteral("tabs.state"), m_tabs.serialize());
    }
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
        // Pinned: the active page, anything not yet durably committed, and
        // pages with images still being imported.
        if (!s || s == m_active || s->isUncommitted() || s->hasUnsnapshotted() || s->hasPendingImports())
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
    // A PDF tab stays open beside the note: reading and note taking go on.
    if (m_active && settingsOpen())
        closeSettings();
    // The current tab shows `id` from now on, like a browser's address bar
    // while the page loads; the page it leaves goes on its back history.
    if (m_tabs.current().page != id) {
        if (!m_navigatingHistory)
            m_tabs.pushHistory(m_tabs.current().page);
        m_tabs.current().page = id;
        tabsEdited();
    }
    if (m_active && m_active->pageId() == id) {
        m_pendingOpen = 0; // a slower load started before this must not land
        if (focusEditor)
            m_pane->focusBody();
        return;
    }
    m_openClock.start();
    m_openTarget = id;
    m_awaitPaint = true;
    const qint64 started = m_openClock.nsecsElapsed();
    if (m_active)
        m_pane->editor()->storeViewState();
    flushSaves(); // the page being left is snapshotted now, not after the timer
    if (PageSession *s = m_cache.value(id)) {
        m_pendingOpen = 0;
        activate(s, started, true, focusEditor);
        return;
    }
    m_pendingOpen = id;
    m_store->openPage(id, this, [this, id, focusEditor, started](PageContent pc) {
        if (m_pendingOpen != id)
            return; // superseded by a newer navigation
        m_pendingOpen = 0;
        if (!pc.found) {
            pruneTabs(0); // a tab pointed at a page that is gone
            return;
        }
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
    if (m_evictWhenShown)
        evictIfUnused(std::exchange(m_evictWhenShown, 0));
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
    m_pendingOpen = 0; // a load still in flight must not replace the new page
    if (!m_navigatingHistory)
        m_tabs.pushHistory(m_tabs.current().page);
    m_tabs.current().page = id;
    tabsEdited();
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

// ---------------------------------------------------------------- tabs ----
QString AppController::tabTitle(PageId id)
{
    if (PageSession *s = m_cache.value(id))
        return m_tabTitles[id] = s->title();
    if (auto it = m_tabTitles.find(id); it != m_tabTitles.end())
        return *it;
    const QString t = m_data->titleOf(id);
    m_tabTitles.insert(id, t);
    if (t.isEmpty()) {
        // Not in the shared cache (or really untitled): one lookup.
        m_store->pageMeta(id, this, [this](PageMeta m, bool found) {
            if (found && m_tabTitles.contains(m.id))
                m_data->noteTitle(m.id, m.title); // reaches the strip through titleChanged
        });
    }
    return t;
}

void AppController::syncTabStrip()
{
    if (!m_tabStrip)
        return;
    QVector<TabStrip::Item> items;
    items.reserve(m_tabs.count());
    QSet<PageId> shown;
    for (int i = 0; i < m_tabs.count(); ++i) {
        const NoteTab &t = m_tabs.at(i);
        shown.insert(t.page);
        items.push_back({t.uid, t.page > 0 ? tabTitle(t.page) : QString()});
    }
    for (auto it = m_tabTitles.begin(); it != m_tabTitles.end();)
        it = shown.contains(it.key()) ? std::next(it) : m_tabTitles.erase(it);
    m_tabStrip->setTabs(items, m_tabs.activeIndex());
}

void AppController::tabsEdited()
{
    m_tabsDirty = true;
    m_prefsTimer.start(); // one settings write for a burst of tab changes
    emit tabsChanged();
}

void AppController::activateTab(int index)
{
    if (index < 0 || index >= m_tabs.count())
        return;
    if (settingsOpen())
        closeSettings();
    if (index != m_tabs.activeIndex()) {
        m_tabs.setActive(index);
        m_bgOpenerUid = 0;
        tabsEdited();
    }
    const PageId page = m_tabs.current().page;
    if (page > 0) {
        m_navigatingHistory = true; // a tab switch is not a navigation inside the tab
        openPage(page);
        m_navigatingHistory = false;
    }
}

int AppController::openInNewTab(PageId id, bool background)
{
    if (id <= 0)
        return -1;
    if (!background && m_tabs.current().page <= 0) {
        openPage(id); // the empty tab shows it (an empty page is no history)
        return m_tabs.activeIndex();
    }
    // Background tabs opened one after another from the same tab line up in
    // the order they were opened, right of it (as in Chrome).
    const quint32 opener = m_tabs.current().uid;
    if (opener != m_bgOpenerUid) {
        m_bgOpenerUid = opener;
        m_bgOpened = 0;
    }
    const int at = m_tabs.insert(m_tabs.activeIndex() + 1 + m_bgOpened, id);
    if (at < 0) {
        m_data->notifyStatus(tr("At most %1 tabs can be open").arg(TabList::kMaxTabs));
        return -1;
    }
    if (background) {
        ++m_bgOpened;
        tabsEdited();
    } else {
        activateTab(at);
    }
    return at;
}

PageId AppController::newTab()
{
    if (m_tabs.current().page <= 0)
        return newPage(0, -1, QString(), true); // the empty tab takes it
    // "+" and Mod+T add the tab at the end, as in Chrome.
    const int at = m_tabs.insert(-1, 0);
    if (at < 0) {
        m_data->notifyStatus(tr("At most %1 tabs can be open").arg(TabList::kMaxTabs));
        return 0;
    }
    m_tabs.setActive(at);
    m_bgOpenerUid = 0;
    tabsEdited();
    return newPage(0, -1, QString(), true);
}

bool AppController::closeTab(int index)
{
    if (index < 0 || index >= m_tabs.count() || m_tabs.count() <= 1)
        return false;
    const PageId page = m_tabs.at(index).page;
    const bool wasActive = index == m_tabs.activeIndex();
    m_tabs.close(index);
    m_bgOpenerUid = 0;
    tabsEdited();
    if (wasActive) {
        if (m_tabs.current().page > 0) {
            m_navigatingHistory = true;
            openPage(m_tabs.current().page);
            m_navigatingHistory = false;
        } else {
            showNoPage();
        }
    }
    // While the next tab's page loads, the closed one is still shown (and
    // so kept): it goes once the other page is up, in activate().
    if (m_active && m_active->pageId() == page)
        m_evictWhenShown = page;
    else
        evictIfUnused(page);
    return true;
}

void AppController::showNoPage()
{
    m_pendingOpen = 0;
    if (!m_active)
        return;
    m_pane->editor()->storeViewState();
    flushSaves();
    m_active = nullptr;
    m_pane->setSession(nullptr);
    m_data->setCurrent(0, {});
    updateWindowTitle();
    updateStatus();
}

bool AppController::reopenClosedTab()
{
    const int at = m_tabs.reopenClosed();
    if (at < 0)
        return false;
    m_bgOpenerUid = 0;
    tabsEdited();
    m_navigatingHistory = true;
    openPage(m_tabs.current().page);
    m_navigatingHistory = false;
    return true;
}

void AppController::moveTab(int from, int to)
{
    if (from == to || from < 0 || to < 0 || from >= m_tabs.count() || to >= m_tabs.count())
        return;
    m_tabs.move(from, to);
    m_bgOpenerUid = 0;
    tabsEdited();
}

// A closed tab's page leaves the document cache right away when no other
// tab shows it and nothing of it is left to save: closing a large page frees
// its memory, instead of keeping it until the cache's LRU gets to it.
void AppController::evictIfUnused(PageId id)
{
    PageSession *s = m_cache.value(id);
    if (!s || s == m_active || m_tabs.showsPage(id) || s->isUncommitted() || s->hasUnsnapshotted()
        || s->hasPendingImports())
        return;
    m_cache.remove(id);
    m_lru.removeAll(id);
    s->deleteLater();
    instr::set(instr::DocCacheBytes, docCacheBytes());
    instr::set(instr::DocCacheEntries, m_cache.size());
}

void AppController::pruneTabs(PageId fallback)
{
    const QVector<PageId> refs = m_tabs.referencedPages();
    if (refs.isEmpty())
        return;
    m_store->pagesMeta(refs, this, [this, refs, fallback](QVector<PageMeta> rows) {
        QSet<PageId> alive;
        for (const PageMeta &m : std::as_const(rows))
            alive.insert(m.id);
        QSet<PageId> gone;
        for (PageId p : refs)
            if (!alive.contains(p))
                gone.insert(p);
        if (gone.isEmpty())
            return;
        // Pages a tab started referencing after the query was posted are not
        // in `refs`, so they are never dropped by mistake.
        m_tabs.dropPages(gone);
        tabsEdited();
        if (gone.contains(m_tabs.current().page)) {
            if (fallback > 0 && !gone.contains(fallback)) {
                m_navigatingHistory = true; // the page that went is not history
                openPage(fallback);
                m_navigatingHistory = false;
            } else {
                openFirstPage(true);
            }
        }
    });
}

// -------------------------------------------------------------- upload ----
bool AppController::canUpload(const QString &path)
{
    return import::isDocumentFile(path)
        || QFileInfo(path).suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0;
}

void AppController::chooseUploadFiles()
{
    auto *dlg = new QFileDialog(m_window, tr("Upload Files"),
                                m_store->setting(QStringLiteral("upload.dir"),
                                                 QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)),
                                tr("Documents (*.pdf *.docx *.md *.markdown *.txt);;PDF (*.pdf);;Word (*.docx);;"
                                   "Markdown and text (*.md *.markdown *.txt)"));
    dlg->setFileMode(QFileDialog::ExistingFiles);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QFileDialog::filesSelected, this, [this, dlg](const QStringList &files) {
        m_store->setSetting(QStringLiteral("upload.dir"), dlg->directory().absolutePath());
        uploadFiles(files);
    });
    dlg->open(); // a sheet on macOS; the panel itself runs in a system process
}

void AppController::uploadFiles(const QStringList &paths)
{
    QList<AttachmentStore::Source> pdfs;
    QStringList docs;
    for (const QString &p : paths) {
        const QFileInfo fi(p);
        if (import::isDocumentFile(p)) {
            docs << p;
        } else if (fi.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0) {
            AttachmentStore::Source s;
            s.path = p;
            s.name = fi.fileName();
            pdfs << s;
        } else {
            m_uploadErrors << tr("%1: not a PDF, Word, Markdown or text file").arg(fi.fileName());
        }
    }
    if (settingsOpen())
        closeSettings();
    if (!pdfs.isEmpty()) {
        // Into the current note at the caret, as Edit ▸ Insert PDF… does; the
        // first one opens beside it once it is stored.
        if (!m_active)
            newPage(0, -1, QString(), false);
        auto shown = std::make_shared<bool>(false);
        m_pane->editor()->importImages(pdfs, [this, shown](const AttachmentStore::ImportResult &r) {
            if (*shown || !r.info.isPdf())
                return;
            *shown = true;
            showPdf(r.info.hash, r.alt);
        });
    }
    if (!docs.isEmpty())
        m_uploadForeground = true;
    for (const QString &d : std::as_const(docs))
        m_docImport->import(d);
    if (docs.isEmpty())
        reportUploadErrors(); // unsupported files, now; otherwise with the batch
}

void AppController::reportUploadErrors()
{
    if (m_uploadErrors.isEmpty() || m_docImport->pending() > 0)
        return;
    auto *box = new QMessageBox(QMessageBox::Warning, tr("Upload Files"),
                                m_uploadErrors.size() == 1 ? tr("This file could not be imported.")
                                                           : tr("These files could not be imported."),
                                QMessageBox::Ok, m_window);
    box->setInformativeText(m_uploadErrors.join(QLatin1Char('\n')));
    box->setWindowModality(Qt::WindowModal);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->open(); // no nested event loop
    m_uploadErrors.clear();
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
    if (m_attachments && m_attachments->importsInFlight() > 0) {
        m_attachments->cancelJobs();     // a document import stops at its next step
        m_attachments->waitForImports(); // their references go into this flush
    }
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
        if (!s || s == m_active || s->isUncommitted() || s->hasUnsnapshotted() || s->hasPendingImports())
            continue;
        m_cache.remove(m_lru[i]);
        m_lru.removeAt(i);
        s->deleteLater();
    }
    if (m_previews)
        m_previews->trim(critical ? 0 : (1 << 20));
    if (m_media)
        m_media->memoryPressure(critical); // trims, then a lower budget for a cool-down
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
                m_store->trashPage(id, heldPages(), this, [this, id](bool ok, PageId parent) {
                    if (!ok)
                        return;
                    if (PageSession *ps = m_cache.value(parent))
                        ps->removePageLinks(id);
                    // The page and its whole subtree are trashed: tabs showing
                    // any of them close, and the current one (if it showed
                    // one) moves to the parent.
                    pruneTabs(parent);
                });
                return okResult();
            }, {}});
    c->add({QStringLiteral("link.edit"), tr("Insert or Edit Link"), {}, {}, [this](const QVariantMap &) {
                QString why;
                return m_pane->editor()->editLink(&why) ? okResult() : fail(why);
            }, {}});
    c->add({QStringLiteral("image.insert"), tr("Insert Image"), {}, {}, [this](const QVariantMap &) {
                return m_pane->editor()->chooseImages() ? okResult() : fail(tr("no image chosen"));
            }, {}});
    c->add({QStringLiteral("pdf.insert"), tr("Insert PDF"), {}, {}, [this](const QVariantMap &) {
                return m_pane->editor()->choosePdfs() ? okResult() : fail(tr("no PDF chosen"));
            }, {}});
    c->add({QStringLiteral("file.upload"), tr("Upload Files"), {}, {}, [this](const QVariantMap &) {
                // Deferred: a dialog must not open inside a Lua handler. No
                // path argument: layouts never name files to read.
                QTimer::singleShot(0, this, &AppController::chooseUploadFiles);
                return okResult();
            }, {}});
    c->add({QStringLiteral("pdf.close"), tr("Close PDF"), {}, {}, [this](const QVariantMap &) {
                if (!m_pdf)
                    return fail(tr("no PDF is open"));
                closePdf();
                return okResult();
            }, {}});
    c->add({QStringLiteral("pane.swap"), tr("Swap Panes"), {}, {}, [this](const QVariantMap &) {
                if (!m_pdf)
                    return fail(tr("only one tab is open"));
                swapPanes();
                return okResult();
            }, {}});
    c->add({QStringLiteral("pane.focus_other"), tr("Focus Other Pane"), {}, {}, [this](const QVariantMap &) {
                if (!m_pdf)
                    return fail(tr("only one tab is open"));
                if (settingsOpen())
                    return fail(tr("Settings are open"));
                focusOtherPane();
                return okResult();
            }, {}});
    c->add({QStringLiteral("page.reveal"), tr("Reveal Page in Tree"), {}, {arg("id", T::LongLong)}, [this](const QVariantMap &a) {
                emit m_data->revealRequested(argId(a));
                return okResult();
            }, {}});
    // Back and forward move through the current tab's own history.
    c->add({QStringLiteral("nav.back"), tr("Back"), {}, {}, [this](const QVariantMap &) {
                const PageId id = m_tabs.back();
                if (!id)
                    return fail(tr("nothing to go back to"));
                m_navigatingHistory = true;
                openPage(id);
                m_navigatingHistory = false;
                return okResult();
            }, {}});
    c->add({QStringLiteral("nav.forward"), tr("Forward"), {}, {}, [this](const QVariantMap &) {
                const PageId id = m_tabs.forward();
                if (!id)
                    return fail(tr("nothing to go forward to"));
                m_navigatingHistory = true;
                openPage(id);
                m_navigatingHistory = false;
                return okResult();
            }, {}});
    // Tabs. Indices are 1-based, as in Lua; -1 is the last tab. Commands with
    // nothing to do (closing the last tab, Mod+9 with one tab) succeed
    // quietly, so a key binding never raises an error banner for them.
    auto tabIndex = [this](const QVariantMap &a) {
        if (!a.contains(QStringLiteral("index")))
            return m_tabs.activeIndex();
        const int i = a.value(QStringLiteral("index")).toInt();
        return i < 0 ? m_tabs.count() + i : i - 1;
    };
    c->add({QStringLiteral("tab.new"), tr("New Tab"), {}, {}, [this](const QVariantMap &) {
                newTab();
                return okResult();
            }, {}});
    c->add({QStringLiteral("tab.open"), tr("Open in New Tab"), {}, {arg("id", T::LongLong, true), arg("background", T::Bool)},
            [this](const QVariantMap &a) {
                const PageId id = a.value(QStringLiteral("id")).toLongLong();
                if (id <= 0)
                    return fail(tr("no page"));
                if (settingsOpen())
                    closeSettings();
                openInNewTab(id, a.value(QStringLiteral("background")).toBool());
                return okResult();
            }, {}});
    c->add({QStringLiteral("tab.close"), tr("Close Tab"), {}, {arg("index", T::LongLong)}, [this, tabIndex](const QVariantMap &a) {
                if (settingsOpen())
                    closeSettings();
                closeTab(tabIndex(a));
                return okResult();
            }, {}});
    c->add({QStringLiteral("tab.select"), tr("Show Tab"), {}, {arg("index", T::LongLong, true)}, [this, tabIndex](const QVariantMap &a) {
                activateTab(tabIndex(a));
                return okResult();
            }, {}});
    c->add({QStringLiteral("tab.next"), tr("Next Tab"), {}, {}, [this](const QVariantMap &) {
                activateTab((m_tabs.activeIndex() + 1) % m_tabs.count());
                return okResult();
            }, {}});
    c->add({QStringLiteral("tab.prev"), tr("Previous Tab"), {}, {}, [this](const QVariantMap &) {
                activateTab((m_tabs.activeIndex() + m_tabs.count() - 1) % m_tabs.count());
                return okResult();
            }, {}});
    c->add({QStringLiteral("tab.reopen"), tr("Reopen Closed Tab"), {}, {}, [this](const QVariantMap &) {
                if (settingsOpen())
                    closeSettings();
                reopenClosedTab();
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
                if (pdfHasFocus())
                    return fail(tr("the PDF has focus"));
                if (m_pane->titleField()->hasFocus())
                    m_pane->titleField()->undo();
                else
                    m_pane->editor()->undo();
                return okResult();
            }, {}});
    c->add({QStringLiteral("editor.redo"), tr("Redo"), {}, {}, [this](const QVariantMap &) {
                if (settingsOpen())
                    return fail(tr("Settings are open"));
                if (pdfHasFocus())
                    return fail(tr("the PDF has focus"));
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
