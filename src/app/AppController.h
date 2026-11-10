// Integrator: wires storage, editor sessions, previews, commands and the
// programmable shell into one app; owns page lifecycle, the byte-capped
// document cache, autosave scheduling and shutdown flushing.
#pragma once

#include "app/Tabs.h"
#include "core/Types.h"
#include "editor/PageSession.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>

class QMainWindow;
class QStackedWidget;

namespace folia {

class AttachmentStore;
class CommandRegistry;
class DocumentImporter;
class MediaService;
class DataContext;
class EditorContext;
class EditorPane;
class PageSession;
class PreviewService;
class PaneSplitter;
class PdfViewerPage;
class SettingsPage;
class TabStrip;
class ShellHost;
class Store;
class WindowBufferTrimmer;

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
        qint64 mediaBudgetBytes = 0;   // 0: MediaService's default
        qint64 mediaCacheBytes = 0;    // cached pixels; 0: MediaService's default
        QString attachmentsDir;        // default: next to the workspace file
        // Read the start page while the window is built and show it with the
        // window's first paint (off: request it once the event loop runs).
        bool prefetchStartPage = true;
        // Release the window's spare buffers after this long without a paint
        // (WindowBufferTrimmer; macOS). 0: never.
        int idleBufferTrimMs = 10000;
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
    WindowBufferTrimmer *bufferTrimmer() const { return m_bufferTrimmer; }
    PreviewService *previews() const { return m_previews; }
    AttachmentStore *attachments() const { return m_attachments; }
    MediaService *media() const { return m_media; }
    PageSession *activeSession() const { return m_active; }
    PageId currentPage() const;

    // Shows `id` in the current tab (its previous page goes on that tab's
    // back history).
    void openPage(PageId id, bool focusEditor = true);
    PageId newPage(PageId parent, int index, const QString &title, bool focusTitle);
    // Note tabs (docs/ARCHITECTURE.md §5, "Tabs"): each tab is a page id
    // with its own back/forward history. Only the shown tab's page is
    // loaded; showing another tab opens its page through the document
    // cache. Indices are 0-based here (the tab.* commands take 1-based ones).
    const TabList &tabs() const { return m_tabs; }
    void activateTab(int index);
    // A new tab showing `id` after the current one; shown unless `background`.
    // When the current tab is empty (an empty workspace has one), a shown
    // page goes into it instead, as Chrome replaces its New Tab page.
    int openInNewTab(PageId id, bool background);
    // "+": a new Untitled page in a new tab (or in the current tab when it
    // is empty), its title ready to type.
    PageId newTab();
    // Closes a tab (never the last one). A page no other tab shows, with
    // nothing left to save, leaves the document cache at once.
    bool closeTab(int index);
    bool reopenClosedTab();
    void moveTab(int from, int to);
    // Upload (File ▸ Upload Files…, the top bar button, drops): PDFs go
    // into the current note (the first opens beside it); Markdown and text
    // files become new top-level pages, opened in tabs once saved
    // (docs/ATTACHMENTS.md §8). Unsupported files are reported, not read.
    void uploadFiles(const QStringList &paths);
    void chooseUploadFiles(); // the native file dialog (window-modal, no nested loop)
    static bool canUpload(const QString &path);
    DocumentImporter *documentImporter() const { return m_docImport; }
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
    // Settings (File ▸ Settings…, Mod+,): a page shown inside the main
    // window in place of the layout. Created when opened, destroyed when
    // closed; the layout's key bindings are off while it is showing.
    void showSettings();
    void closeSettings();
    bool settingsOpen() const;
    SettingsPage *settingsPage() const;
    // The PDF viewer (docs/ATTACHMENTS.md §4): a second tab beside the note,
    // in the editor slot's split (at most two tabs: the note and one PDF;
    // opening another PDF replaces it). Closing it destroys it, which closes
    // the document. pdfOpen() is true while the tab exists, even when
    // Settings covers it.
    bool showPdf(const QString &hash, const QString &name);
    void closePdf();
    bool pdfOpen() const;
    PdfViewerPage *pdfViewer() const;
    bool pdfHasFocus() const;
    // The split: which side the PDF is on and its share of the width are
    // workspace settings (split.side, split.ratio), written once per change.
    PaneSplitter *splitter() const { return m_split; }
    // The tab strip placed where the layout says ui.tabs{}.
    TabStrip *tabStrip() const { return m_tabStrip; }
    void swapPanes();
    void focusOtherPane();
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
    // The tab list changed: tabs added, closed, moved, or the active one.
    void tabsChanged();
    void pageActivated(folia::PageId id, qint64 micros, bool cached);
    void pagePainted(folia::PageId id, qint64 microsSinceRequest);
    void allSaved();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildWindow();
    // The page read during startup becomes the active page (once the window
    // has its geometry: from its show event).
    void showStartPage();
    void registerCommands();
    void seedWorkspace();
    void activate(PageSession *s, qint64 startedNs, bool cached, bool focusEditor);
    PageSession *createSession(PageId id);
    void touchCache(PageId id);
    void evictCache();
    void scheduleSave(PageSession *s);
    void updateStatus();
    void updateWindowTitle();
    void installUserPresets();
    void moveRelative(PageId id, int delta);
    void indent(PageId id);
    void outdent(PageId id);
    void confirmDelete(PageId id);
    void afterMove(PageId id, PageId oldParent, PageId newParent);
    QSet<PageId> heldPages() const;
    void tabsEdited();
    void restoreTabs(bool prefetched);
    // Shows the first root page in the current tab; `replace`: without
    // putting the tab's page on its back history.
    void openFirstPage(bool replace);
    // Closes tabs showing trashed pages; moves the current tab to `fallback`
    // (or the first page) if its own page is gone.
    void pruneTabs(PageId fallback);
    void evictIfUnused(PageId id);
    // The current tab has no page (an empty workspace): the editor shows
    // none, so the page it showed can leave the cache.
    void showNoPage();
    // Pushes the tab list and titles to the strip (it ignores no-op updates).
    void syncTabStrip();
    QString tabTitle(PageId id);
    void reportUploadErrors();
    PageId argId(const QVariantMap &args) const;

    Options m_opt;
    Store *m_store = nullptr;
    EditorContext *m_ectx = nullptr;
    PreviewService *m_previews = nullptr;
    AttachmentStore *m_attachments = nullptr;
    MediaService *m_media = nullptr;
    DataContext *m_data = nullptr;
    CommandRegistry *m_commands = nullptr;
    EditorPane *m_pane = nullptr;
    ShellHost *m_shell = nullptr;
    QMainWindow *m_window = nullptr;
    QWidget *m_shellContainer = nullptr;
    QStackedWidget *m_stack = nullptr; // the layout, or the Settings page
    PaneSplitter *m_split = nullptr;   // the editor slot: the note, and the PDF pane beside it
    TabStrip *m_tabStrip = nullptr;
    DocumentImporter *m_docImport = nullptr;
    bool m_uploadForeground = false; // the next imported page opens in a shown tab
    QStringList m_uploadErrors;      // reported together once the batch is done
    // Titles of the pages tabs show, kept here because the shared title
    // cache is cleared wholesale when it fills up.
    QHash<PageId, QString> m_tabTitles;
    QPointer<SettingsPage> m_settings;
    QPointer<PdfViewerPage> m_pdf;
    QTimer m_prefsTimer;         // coalesces settings writes
    WindowBufferTrimmer *m_bufferTrimmer = nullptr;
    PageSession *m_startSession = nullptr; // built before the window is shown
    bool m_themeDirty = false;
    bool m_tabsDirty = false;

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
    TabList m_tabs;
    quint32 m_bgOpenerUid = 0; // tab that background tabs were last opened from
    int m_bgOpened = 0;        // how many, so the next one goes after them
    // A closed tab's page that was still shown while the next tab's page
    // loaded: evicted once that page is shown (see closeTab).
    PageId m_evictWhenShown = 0;
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
