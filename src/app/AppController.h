// Integrator: wires storage, editor sessions, previews, commands and the
// programmable shell into one app; owns page lifecycle, the byte-capped
// document cache, autosave scheduling and shutdown flushing.
#pragma once

#include "core/Types.h"
#include "editor/PageSession.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>

class QMainWindow;

namespace folia {

class CommandRegistry;
class DataContext;
class EditorContext;
class EditorPane;
class PageSession;
class PreviewService;
class SettingsWindow;
class ShellHost;
class Store;

class AppController : public QObject {
    Q_OBJECT
public:
    struct Options {
        QString homeDir;        // workspace + shell folder root (default: AppDataLocation)
        QString workspacePath;  // overrides homeDir/workspace.db
        QString shellPath;      // load this shell file instead of the active preset
        QString preset;         // "default", "keyboard", "right-nav" ...
        bool safeMode = false;
        bool noMath = false;
        bool faultVfs = false;
        bool watchShell = true;
        bool seedIfEmpty = true;
        bool restoreLastPage = true;
        PageId startPage = 0;   // open this page at startup (overrides restore)
        int autosaveIdleMs = 300;
        int autosaveMaxDirtyMs = 900; // < 1 s maximum dirty interval incl. commit
        qint64 docCacheBytes = 48ll << 20;
        int docCacheEntries = 16;
    };

    explicit AppController(const Options &opt, QObject *parent = nullptr);
    ~AppController() override;

    bool start(QString *error);
    // Flush everything and close storage. Returns false when unsaved edits
    // could not be committed and the user chose to keep the app open.
    bool shutdown(bool interactive);

    QMainWindow *window() const { return m_window; }
    Store *store() const { return m_store; }
    EditorPane *editorPane() const { return m_pane; }
    ShellHost *shell() const { return m_shell; }
    CommandRegistry *commands() const { return m_commands; }
    DataContext *data() const { return m_data; }
    EditorContext *editorContext() const { return m_ectx; }
    PreviewService *previews() const { return m_previews; }
    PageSession *activeSession() const { return m_active; }
    PageId currentPage() const;

    void openPage(PageId id, bool focusEditor = true);
    PageId newPage(PageId parent, int index, const QString &title, bool focusTitle);
    void flushSaves();
    QString userShellDir() const;
    QString presetPath(const QString &name) const;
    QString builtinPresetPath(const QString &name) const;
    bool loadShell(const QString &path, QString *error = nullptr);
    void resetShell();
    void reclaimMemory(bool critical);
    qint64 docCacheBytes() const;
    int docCacheEntries() const { return m_cache.size(); }
    bool sessionCached(PageId id) const { return m_cache.contains(id); }
    // Settings window (File ▸ Settings…, Mod+,): created on the first open,
    // hidden (not destroyed) when closed, dropped under critical memory pressure.
    void showSettings();
    SettingsWindow *settingsWindow() const;
    // Appearance overrides layered over the layout theme: applied now,
    // written to the workspace shortly after (coalesced).
    void setThemeOverrides(const QVariantMap &overrides);
    QVariantMap themeOverrides() const;
    void flushPreferences();
    bool liveReload() const;
    void setLiveReload(bool on);
    bool liveReloadDisabledByFlag() const { return !m_opt.watchShell; }
    bool safeMode() const { return m_opt.safeMode; }
    bool mathDisabledByFlag() const { return m_opt.noMath; }
    QString homeDir() const { return m_opt.homeDir; }
    QStringList availableLayouts() const; // *.lua in the shell folder, "default" first
    QString activeLayout() const;         // its name, or empty for a file outside the shell folder

    // Bench/recovery harness only: take over save timing.
    void setAutosaveEnabled(bool on) { m_autosave = on; if (!on) { m_idleTimer.stop(); m_deadlineTimer.stop(); } }

signals:
    void pageActivated(folia::PageId id, qint64 micros, bool cached);
    void pagePainted(folia::PageId id, qint64 microsSinceRequest);
    void allSaved();

private:
    void buildWindow();
    void registerCommands();
    void seedWorkspace();
    void activate(PageSession *s, qint64 startedNs, bool cached, bool focusEditor);
    PageSession *createSession(PageId id);
    void touchCache(PageId id);
    void evictCache();
    void scheduleSave(PageSession *s);
    void updateStatus();
    void installUserPresets();
    void moveRelative(PageId id, int delta);
    void indent(PageId id);
    void outdent(PageId id);
    void confirmDelete(PageId id);
    void afterMove(PageId id, PageId oldParent, PageId newParent);
    QSet<PageId> heldPages() const;
    PageId argId(const QVariantMap &args) const;

    Options m_opt;
    Store *m_store = nullptr;
    EditorContext *m_ectx = nullptr;
    PreviewService *m_previews = nullptr;
    DataContext *m_data = nullptr;
    CommandRegistry *m_commands = nullptr;
    EditorPane *m_pane = nullptr;
    ShellHost *m_shell = nullptr;
    QMainWindow *m_window = nullptr;
    QWidget *m_shellContainer = nullptr;
    QPointer<SettingsWindow> m_settings;
    QTimer m_prefsTimer;         // coalesces settings writes
    bool m_themeDirty = false;

    QHash<PageId, PageSession *> m_cache;
    QList<PageId> m_lru; // front = most recent
    QPointer<PageSession> m_active;
    PageId m_pendingOpen = 0;
    QTimer m_idleTimer;
    QTimer m_deadlineTimer;
    // Oldest not-yet-snapshotted edit per page, and per in-flight batch
    // revision the age of its oldest edit (for the max-dirty-interval probe).
    QHash<PageId, QElapsedTimer> m_dirtySince;
    QHash<PageId, QList<QPair<Revision, QElapsedTimer>>> m_inflight;
    QList<PageId> m_back, m_forward;
    bool m_navigatingHistory = false;
    QElapsedTimer m_openClock;
    PageId m_openTarget = 0;
    bool m_awaitPaint = false;
    bool m_failing = false;
    QString m_failMessage;
    bool m_shutDown = false;
    bool m_autosave = true;
};

} // namespace folia
