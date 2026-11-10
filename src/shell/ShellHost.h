// Programmable shell owner: evaluates a versioned Lua layout (the shell
// file), converts its view() result into a validated node tree and
// reconciles native widgets by stable identity. Lua runs only on load,
// on state changes requested by Lua handlers, on page changes and on
// explicitly bound user actions — never on typing, scrolling or painting.
#pragma once

#include "core/Types.h"

#include <QColor>
#include <QHash>
#include <QKeySequence>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QVariantMap>
#include <memory>
#include <vector>

class QFileSystemWatcher;
class QShortcut;
class QWidget;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace folia {

class CommandRegistry;
class DataContext;
class EditorContext;
class EditorPane;
class LuaRuntime;
class PageListModel;

struct UiNode {
    QString type;
    QString key;              // stable identity used for reconciliation
    QString id;               // explicit id (may be empty)
    QVariantMap props;        // scalars; style.* flattened
    QVariantMap source;       // list data source descriptor
    QVariantMap row;          // list row template
    QHash<QString, int> handlers; // event -> Lua registry ref
    QVector<UiNode> children;
};

class ShellHost : public QObject {
    Q_OBJECT
public:
    struct Deps {
        EditorPane *editor = nullptr;
        EditorContext *editorCtx = nullptr;
        CommandRegistry *commands = nullptr;
        DataContext *data = nullptr;
        QWidget *window = nullptr;   // shortcuts live here
        QWidget *container = nullptr; // shell root is placed inside
        // The widget placed where the layout says ui.editor{} (default: the
        // editor itself). The app passes the split that holds the editor and
        // the PDF tab; focus, IME and title handling still target `editor`.
        QWidget *editorSlot = nullptr;
        // The note tab strip (a TabStrip) placed where the layout says
        // ui.tabs{}. A layout without one gets it directly above the editor
        // (ui.tabs{ visible = false } hides it); never deleted. Null:
        // ui.tabs{} is an empty slot, and there is no fallback.
        QWidget *tabs = nullptr;
    };
    struct Limits {
        int maxNodes = 2000;
        int maxDepth = 40;
    };

    explicit ShellHost(const Deps &deps, QObject *parent = nullptr);
    ~ShellHost() override;

    // Load a shell file. On any error the previous valid layout stays active.
    bool loadFile(const QString &path, QString *error);
    bool loadSource(const QByteArray &code, const QString &name, QString *error);
    void setWatchFile(bool on);
    void setSafeModeNotice(const QString &notice);
    // The layout's key bindings (off while another page, such as Settings,
    // covers the layout; bindings created by a reload follow this too).
    void setShortcutsEnabled(bool on);
    bool shortcutsEnabled() const { return m_shortcutsEnabled; }

    QString activePath() const { return m_path; }
    QString activeName() const { return m_name; }
    QString lastError() const { return m_lastError; }
    QStringList warnings() const { return m_warnings; }
    bool hasLayout() const { return bool(m_rt); }
    int liveWidgetCount() const { return m_live.size(); }
    QWidget *widgetById(const QString &id) const;
    QVariantMap theme() const { return m_theme; } // effective: layout theme + overrides
    // A named style color of the current theme ("sidebar", "selection_bg"…), or empty.
    QString namedColor(const QString &name) const { return m_colors.value(name); }
    size_t luaMemory() const;

    // Settings overrides, layered over the layout's theme table (same shape:
    // mode, font, mono, editor, colors). The layout file is never modified;
    // an empty map means "use the layout's theme". Applied now and kept
    // across layout loads.
    void setThemeOverrides(const QVariantMap &overrides);
    QVariantMap themeOverrides() const { return m_overrides; }
    QVariantMap layoutTheme() const { return m_spec.theme; }
    // Nested tables are merged key by key; anything else is replaced.
    static QVariantMap mergeTheme(const QVariantMap &base, const QVariantMap &overrides);

    // Re-run view() now (normally coalesced to the next event-loop turn).
    bool renderNow(QString *error = nullptr);
    void scheduleRender();

signals:
    void loaded(const QString &name);
    void loadFailed(const QString &error);
    void resetRequested();
    void rendered(qint64 micros);

private:
    friend struct ShellApi;
    struct Binding {
        QKeySequence seq;
        QString keyText;
        QStringList commands;
        QVariantMap args;
        int fnRef = -1;
    };
    struct Spec {
        QString name;
        int version = 0;
        QVariantMap theme;
        int stateRef = -1;
        int viewRef = -1;
        int ctxRef = -1;
        std::vector<Binding> keys;
        QStringList focusOrder;
    };
    struct Live {
        QPointer<QWidget> w;
        QString type;
        PageListModel *model = nullptr;
        QString sourceKind;
        QString iconKey; // button icon name/color/size last applied
        bool wasVisible = false;
    };

    bool evaluate(LuaRuntime &rt, const QByteArray &code, const QString &name, Spec &spec, QString *err);
    bool runView(LuaRuntime &rt, Spec &spec, UiNode &root, std::vector<int> &refs, QString *err);
    bool toNode(LuaRuntime &rt, int idx, UiNode &n, int depth, int &count, const QString &path,
                std::vector<int> &refs, QString *err);
    bool validate(const UiNode &root, QString *err) const;
    void apply(const UiNode &root);
    QWidget *realize(const UiNode &n, QSet<QString> &seen, QWidget *parent);
    void updateCommon(QWidget *w, const UiNode &n);
    void applyTheme(const QVariantMap &theme);
    void applyBindings();
    void applyFocusOrder(const UiNode &root);
    void releaseRefs(std::vector<int> &refs);
    void releaseSpec(LuaRuntime &rt, Spec &spec);
    void pushCtx();
    void showBanner(const QString &text, bool error);
    void runBinding(const Binding &b);
    bool invokeHandler(int ref, const QVariantMap &arg, const QString &textArg, bool hasText);
    QString resolveColor(const QVariant &v, const QString &fallback = {}) const;
    void fileChanged();
    void maybeReloadPending();
    void applyPendingFocus();
    QWidget *slot() const; // m_d.editorSlot, or the editor

    Deps m_d;
    Limits m_lim;
    std::unique_ptr<LuaRuntime> m_rt;
    Spec m_spec;
    std::vector<int> m_renderRefs;
    QHash<QString, Live> m_live;
    QPointer<QWidget> m_rootWidget;
    QWidget *m_shellArea = nullptr;
    QWidget *m_banner = nullptr;
    QLabel *m_bannerText = nullptr;
    QPushButton *m_bannerReset = nullptr;
    QPointer<QWidget> m_editorParking;
    std::vector<QPointer<QShortcut>> m_shortcuts;
    QVariantMap m_theme;
    QVariantMap m_overrides;
    // Appearance Qt reports: the OS's while mode = "system", else the pinned one.
    Qt::ColorScheme m_systemScheme = Qt::ColorScheme::Unknown;
    bool m_settingScheme = false; // inside our own QStyleHints::setColorScheme()
    bool m_schemeRequested = false;
    Qt::ColorScheme m_requestedScheme = Qt::ColorScheme::Unknown; // Unknown = follow the OS
    bool m_shortcutsEnabled = true;
    QColor m_accent;
    QHash<QString, QString> m_colors;
    QString m_path;
    QString m_name;
    QString m_lastError;
    QByteArray m_loadedCode; // source of the active layout (reload dedupe)
    QString m_safeNotice;
    QStringList m_warnings;
    QFileSystemWatcher *m_watcher = nullptr;
    QTimer m_reloadTimer;
    QTimer m_renderTimer;
    bool m_reloadPending = false;
    bool m_inView = false;
    QStringList m_pendingFocus;
    QSet<QString> m_autofocusShown;
};

} // namespace folia
