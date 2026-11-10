#include "shell/ShellHost.h"

#include "app/Platform.h"
#include "core/Instrument.h"
#include "core/LucideIcons.h"
#include "editor/EditorContext.h"
#include "editor/EditorPane.h"
#include "editor/NoteEditor.h"
#include "shell/CommandRegistry.h"
#include "shell/DataContext.h"
#include "shell/ListSources.h"
#include "shell/LuaRuntime.h"
#include "shell/TabStrip.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <QApplication>
#include <QBoxLayout>
#include <QCursor>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFrame>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QStyleHints>
#include <QToolTip>

namespace folia {

namespace {

const char *kPrelude = R"LUA(
ui = {}
local function mk(t) return function(p) p = p or {}; p.type = t; return p end end
for _, t in ipairs({"row", "column", "overlay", "text", "button", "input", "list", "editor", "tabs", "spacer", "divider"}) do
  ui[t] = mk(t)
end
function ui.component(name, fn)
  if type(name) ~= "string" or type(fn) ~= "function" then error("ui.component(name, fn) expects a name and a function", 2) end
  if ui[name] ~= nil then error("ui.component: '" .. name .. "' is already defined", 2) end
  ui[name] = function(props) return fn(props or {}) end
  return ui[name]
end
data = {}
for _, k in ipairs({"tree", "search", "recent", "children", "ancestors"}) do
  data[k] = function(o) o = o or {}; o.kind = k; return o end
end
)LUA";

const QSet<QString> &knownTypes()
{
    static const QSet<QString> s = {QStringLiteral("row"), QStringLiteral("column"), QStringLiteral("overlay"),
                                    QStringLiteral("text"), QStringLiteral("button"), QStringLiteral("input"),
                                    QStringLiteral("list"), QStringLiteral("editor"), QStringLiteral("tabs"),
                                    QStringLiteral("spacer"), QStringLiteral("divider")};
    return s;
}

QString platformName()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#elif defined(Q_OS_WIN)
    return QStringLiteral("windows");
#else
    return QStringLiteral("linux");
#endif
}

// Raw field read: host code never triggers user metamethods (which could
// raise errors outside a protected call).
int rawField(lua_State *L, int idx, const char *k)
{
    idx = lua_absindex(L, idx);
    lua_pushstring(L, k);
    return lua_rawget(L, idx);
}

// Lua value -> QVariant (scalars, and bounded nested tables).
QVariant toVariant(lua_State *L, int idx, int depth = 0)
{
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN: return bool(lua_toboolean(L, idx));
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx))
            return qint64(lua_tointeger(L, idx));
        return lua_tonumber(L, idx);
    case LUA_TSTRING: {
        size_t len = 0;
        const char *s = lua_tolstring(L, idx, &len);
        return QString::fromUtf8(s, qsizetype(qMin<size_t>(len, 1 << 20)));
    }
    case LUA_TTABLE: {
        if (depth > 8)
            return {};
        const lua_Unsigned n = lua_rawlen(L, idx);
        if (n > 0) {
            QVariantList l;
            for (lua_Unsigned i = 1; i <= n && i <= 2000; ++i) {
                lua_rawgeti(L, idx, lua_Integer(i));
                l << toVariant(L, -1, depth + 1);
                lua_pop(L, 1);
            }
            return l;
        }
        QVariantMap m;
        lua_pushnil(L);
        int count = 0;
        while (lua_next(L, idx) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING && ++count <= 2000)
                m.insert(QString::fromUtf8(lua_tostring(L, -2)), toVariant(L, -1, depth + 1));
            lua_pop(L, 1);
        }
        return m;
    }
    default:
        return {};
    }
}

void pushVariant(lua_State *L, const QVariant &v)
{
    switch (v.typeId()) {
    case QMetaType::Bool: lua_pushboolean(L, v.toBool()); break;
    case QMetaType::Int:
    case QMetaType::LongLong:
    case QMetaType::UInt:
    case QMetaType::ULongLong: lua_pushinteger(L, lua_Integer(v.toLongLong())); break;
    case QMetaType::Double: lua_pushnumber(L, v.toDouble()); break;
    case QMetaType::QString: {
        const QByteArray u = v.toString().toUtf8();
        lua_pushlstring(L, u.constData(), size_t(u.size()));
        break;
    }
    case QMetaType::QVariantMap: {
        const QVariantMap m = v.toMap();
        lua_createtable(L, 0, int(m.size()));
        for (auto it = m.cbegin(); it != m.cend(); ++it) {
            pushVariant(L, it.value());
            lua_setfield(L, -2, it.key().toUtf8().constData());
        }
        break;
    }
    case QMetaType::QVariantList: {
        const QVariantList l = v.toList();
        lua_createtable(L, int(l.size()), 0);
        for (int i = 0; i < l.size(); ++i) {
            pushVariant(L, l[i]);
            lua_rawseti(L, -2, i + 1);
        }
        break;
    }
    default: lua_pushnil(L); break;
    }
}

// Logical key names -> platform sequence. "Mod" is the platform's primary
// modifier (Cmd on macOS, Ctrl elsewhere); "Ctrl" is the physical Control key.
QKeySequence parseKeys(const QString &text, QString *err)
{
    QStringList parts = text.split(QLatin1Char('+'));
    // Allow a literal "+" key ("Mod++").
    if (text.endsWith(QLatin1String("++"))) {
        parts.removeLast();
        parts.last() = QStringLiteral("+");
    }
    if (parts.isEmpty() || parts.last().trimmed().isEmpty()) {
        *err = QStringLiteral("empty key in '%1'").arg(text);
        return {};
    }
    QStringList mods;
    for (int i = 0; i < parts.size() - 1; ++i) {
        const QString m = parts[i].trimmed().toLower();
#ifdef Q_OS_MACOS
        if (m == QLatin1String("mod") || m == QLatin1String("cmd") || m == QLatin1String("super"))
            mods << QStringLiteral("Ctrl"); // Qt: Ctrl == Command on macOS
        else if (m == QLatin1String("ctrl") || m == QLatin1String("control"))
            mods << QStringLiteral("Meta");
#else
        if (m == QLatin1String("mod") || m == QLatin1String("ctrl") || m == QLatin1String("control"))
            mods << QStringLiteral("Ctrl");
        else if (m == QLatin1String("cmd") || m == QLatin1String("super"))
            mods << QStringLiteral("Meta");
#endif
        else if (m == QLatin1String("alt") || m == QLatin1String("option") || m == QLatin1String("opt"))
            mods << QStringLiteral("Alt");
        else if (m == QLatin1String("shift"))
            mods << QStringLiteral("Shift");
        else {
            *err = QStringLiteral("unknown modifier '%1' in '%2'").arg(parts[i], text);
            return {};
        }
    }
    mods.removeDuplicates();
    const QString seqText = (mods.isEmpty() ? QString() : mods.join(QLatin1Char('+')) + QLatin1Char('+'))
        + parts.last().trimmed();
    QKeySequence seq = QKeySequence::fromString(seqText, QKeySequence::PortableText);
    if (seq.isEmpty() || seq[0].key() == Qt::Key_unknown) {
        *err = QStringLiteral("unknown key '%1'").arg(text);
        return {};
    }
    return seq;
}

const QList<QKeySequence> &reservedKeys()
{
    static const QList<QKeySequence> r = {
        QKeySequence(QStringLiteral("Ctrl+Z")), QKeySequence(QStringLiteral("Ctrl+Shift+Z")),
        QKeySequence(QStringLiteral("Ctrl+X")), QKeySequence(QStringLiteral("Ctrl+C")),
        QKeySequence(QStringLiteral("Ctrl+V")), QKeySequence(QStringLiteral("Ctrl+A")),
        QKeySequence(QStringLiteral("Ctrl+Q")), QKeySequence(QStringLiteral("Ctrl+Alt+Shift+R")),
        QKeySequence(QStringLiteral("Ctrl+Alt+R")), QKeySequence(QStringLiteral("Ctrl+,")), // Settings
    };
    return r;
}

// ------------------------------------------------------------ widgets -----
class ShellBox : public QWidget {
public:
    ShellBox(bool horizontal, QWidget *parent) : QWidget(parent)
    {
        m_layout = new QBoxLayout(horizontal ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom, this);
        m_layout->setContentsMargins(0, 0, 0, 0);
        m_layout->setSpacing(0);
        setAttribute(Qt::WA_StyledBackground, true);
    }
    void setDirection(bool horizontal)
    {
        m_layout->setDirection(horizontal ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
    }
    void setChildren(const QVector<QPair<QWidget *, int>> &kids)
    {
        bool same = m_layout->count() == kids.size();
        for (int i = 0; same && i < kids.size(); ++i)
            same = m_layout->itemAt(i)->widget() == kids[i].first && m_layout->stretch(i) == kids[i].second;
        if (same)
            return;
        while (m_layout->count())
            delete m_layout->takeAt(0);
        for (const auto &k : kids)
            m_layout->addWidget(k.first, k.second);
    }
    QBoxLayout *box() { return m_layout; }

private:
    QBoxLayout *m_layout;
};

class ShellOverlay : public QWidget {
public:
    explicit ShellOverlay(QWidget *parent) : QWidget(parent) { setAttribute(Qt::WA_StyledBackground, true); }
    struct Pop { QPointer<QWidget> w; int width; int top; };
    void setChildren(QWidget *base, const QVector<Pop> &pops)
    {
        m_base = base;
        m_pops = pops;
        if (base && base->parentWidget() != this)
            base->setParent(this);
        for (const Pop &p : pops)
            if (p.w && p.w->parentWidget() != this)
                p.w->setParent(this);
        relayout();
    }
    void relayout()
    {
        if (m_base) {
            m_base->setGeometry(rect());
            m_base->lower();
        }
        for (const Pop &p : std::as_const(m_pops)) {
            if (!p.w)
                continue;
            const int w = qMin(p.width > 0 ? p.width : 560, width() - 32);
            const int hint = p.w->sizeHint().height();
            const int h = qMin(qMax(hint, 80), int(height() * 0.7));
            p.w->setGeometry((width() - w) / 2, p.top, w, h);
            p.w->raise();
        }
    }

protected:
    void resizeEvent(QResizeEvent *) override { relayout(); }

private:
    QPointer<QWidget> m_base;
    QVector<Pop> m_pops;
};

class ShellInput : public QLineEdit {
public:
    using QLineEdit::QLineEdit;
    std::function<bool(QKeyEvent *)> onKey;

protected:
    void keyPressEvent(QKeyEvent *e) override
    {
        if (onKey && onKey(e))
            return;
        QLineEdit::keyPressEvent(e);
    }
};

class ShellList : public QListView {
public:
    explicit ShellList(QWidget *parent) : QListView(parent)
    {
        setFrameShape(QFrame::NoFrame);
        setUniformItemSizes(true);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        setMouseTracking(true);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        delegate = new PageRowDelegate(this);
        setItemDelegate(delegate);
    }
    PageRowDelegate *delegate;
    std::function<void(int)> onActivate;
    std::function<void(const QString &, PageId)> onCommand;
    std::function<void(int row, int action)> onRowAction;
    // Mod-click or middle-click on a row (and Mod+Enter): open it in a new
    // tab, in the background unless Shift is held too.
    std::function<void(int row, bool background)> onOpenInTab;
    bool clickActivates = true;

    // Hover actions: the row under the mouse shows its action buttons. Only
    // the rows whose state changed are repainted.
    int actionAt(const QPoint &pos, int *row) const
    {
        const PageRowDelegate::Template &tpl = delegate->tpl;
        if (tpl.actions.isEmpty() || tpl.horizontal || !model())
            return -1;
        const QModelIndex idx = indexAt(pos);
        if (!idx.isValid() || idx.data(PageListModel::MoreRole).toBool())
            return -1;
        const QRect vr = visualRect(idx);
        for (int i = 0; i < int(tpl.actions.size()); ++i) {
            if (delegate->actionRect(vr, i).contains(pos)) {
                *row = idx.row();
                return i;
            }
        }
        return -1;
    }
    void setHover(int row, int action)
    {
        if (row == delegate->hoverRow && action == delegate->hoverAction)
            return;
        const int old = delegate->hoverRow;
        delegate->hoverRow = row;
        delegate->hoverAction = action;
        updateRow(old);
        if (row != old)
            updateRow(row);
        const Qt::CursorShape shape = action >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor;
        if (viewport()->cursor().shape() != shape)
            viewport()->setCursor(shape);
    }
    void refreshHover()
    {
        if (delegate->tpl.actions.isEmpty())
            return;
        const QPoint pos = viewport()->mapFromGlobal(QCursor::pos());
        if (!viewport()->rect().contains(pos) || !underMouse()) {
            setHover(-1, -1);
            return;
        }
        const QModelIndex idx = indexAt(pos);
        int row = -1;
        const int a = actionAt(pos, &row);
        setHover(idx.isValid() ? idx.row() : -1, a);
    }
    void updateRow(int r)
    {
        if (r >= 0 && model() && r < model()->rowCount())
            viewport()->update(visualRect(model()->index(r, 0)));
    }

    PageListModel *pageModel() const { return static_cast<PageListModel *>(model()); }
    int current() const { return currentIndex().isValid() ? currentIndex().row() : -1; }
    void setCurrentRow(int r)
    {
        if (!model() || r < 0 || r >= model()->rowCount())
            return;
        setCurrentIndex(model()->index(r, 0));
        scrollTo(model()->index(r, 0));
    }
    bool navigate(QKeyEvent *e)
    {
        PageListModel *m = pageModel();
        if (!m)
            return false;
        const int n = m->rowCount();
        int r = current();
        const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
        const bool reorder = mods == (Qt::ControlModifier | Qt::AltModifier);
        if (reorder && r >= 0 && onCommand) {
            const PageId id = m->idAt(r);
            switch (e->key()) {
            case Qt::Key_Up: onCommand(QStringLiteral("page.move_up"), id); return true;
            case Qt::Key_Down: onCommand(QStringLiteral("page.move_down"), id); return true;
            case Qt::Key_Right: onCommand(QStringLiteral("page.indent"), id); return true;
            case Qt::Key_Left: onCommand(QStringLiteral("page.outdent"), id); return true;
            default: break;
            }
        }
        if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && (mods & Qt::ControlModifier) && r >= 0
            && onOpenInTab && !m->data(m->index(r), PageListModel::MoreRole).toBool()) {
            onOpenInTab(r, !(mods & Qt::ShiftModifier));
            return true;
        }
        switch (e->key()) {
        case Qt::Key_Down: setCurrentRow(qMin(n - 1, r + 1)); return true;
        case Qt::Key_Up: setCurrentRow(qMax(0, r - 1)); return true;
        case Qt::Key_PageDown: setCurrentRow(qMin(n - 1, r + 10)); return true;
        case Qt::Key_PageUp: setCurrentRow(qMax(0, r - 10)); return true;
        case Qt::Key_Home: setCurrentRow(0); return true;
        case Qt::Key_End: setCurrentRow(n - 1); return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (r >= 0 && onActivate) {
                if (m->data(m->index(r), PageListModel::MoreRole).toBool())
                    m->loadMore(r);
                else
                    onActivate(r);
            }
            return true;
        case Qt::Key_Right:
            if (m->isTree() && r >= 0) {
                if (m->data(m->index(r), PageListModel::ExpandedRole).toBool())
                    setCurrentRow(qMin(n - 1, r + 1));
                else
                    m->expand(r);
                return true;
            }
            return false;
        case Qt::Key_Left:
            if (m->isTree() && r >= 0) {
                if (m->data(m->index(r), PageListModel::ExpandedRole).toBool()) {
                    m->collapse(r);
                } else {
                    const PageId parent = m->data(m->index(r), PageListModel::ParentRole).toLongLong();
                    const int pr = parent ? m->rowOf(parent) : -1;
                    if (pr >= 0)
                        setCurrentRow(pr);
                }
                return true;
            }
            return false;
        default:
            return false;
        }
    }

protected:
    void keyPressEvent(QKeyEvent *e) override
    {
        if (navigate(e))
            return;
        QListView::keyPressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        QListView::mouseMoveEvent(e);
        if (delegate->tpl.actions.isEmpty() || e->buttons() != Qt::NoButton)
            return;
        const QPoint pos = e->position().toPoint();
        const QModelIndex idx = indexAt(pos);
        int row = -1;
        const int a = actionAt(pos, &row);
        setHover(idx.isValid() ? idx.row() : -1, a);
    }
    void leaveEvent(QEvent *e) override
    {
        setHover(-1, -1);
        QListView::leaveEvent(e);
    }
    void scrollContentsBy(int dx, int dy) override
    {
        QListView::scrollContentsBy(dx, dy);
        if (delegate->hoverRow >= 0)
            refreshHover();
    }
    void rowsInserted(const QModelIndex &parent, int start, int end) override
    {
        QListView::rowsInserted(parent, start, end);
        if (delegate->hoverRow >= 0)
            refreshHover();
    }
    void rowsAboutToBeRemoved(const QModelIndex &parent, int start, int end) override
    {
        setHover(-1, -1); // rows shift; the next mouse move restores it
        QListView::rowsAboutToBeRemoved(parent, start, end);
    }
    bool viewportEvent(QEvent *e) override
    {
        if (e->type() == QEvent::ToolTip && !delegate->tpl.actions.isEmpty()) {
            auto *he = static_cast<QHelpEvent *>(e);
            int row = -1;
            const int a = actionAt(he->pos(), &row);
            if (a >= 0) {
                const QRect ar = delegate->actionRect(visualRect(model()->index(row, 0)), a);
                QToolTip::showText(he->globalPos(), delegate->tpl.actions[a].tooltip, viewport(), ar);
                return true;
            }
        }
        return QListView::viewportEvent(e);
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        int row = -1;
        const int a = e->button() == Qt::LeftButton ? actionAt(e->position().toPoint(), &row) : -1;
        if (a >= 0) {
            m_pressAction = a;
            m_pressRow = row;
            e->accept(); // an action click neither selects nor opens the row
            return;
        }
        m_pressAction = -1;
        m_tabPressRow = -1;
        if (onOpenInTab && (e->button() == Qt::MiddleButton
                            || (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier)))) {
            const QModelIndex idx = indexAt(e->position().toPoint());
            if (idx.isValid() && !idx.data(PageListModel::MoreRole).toBool()) {
                m_tabPressRow = idx.row();
                e->accept(); // neither selects nor opens the row here
                return;
            }
        }
        QListView::mousePressEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (m_pressAction >= 0) {
            int row = -1;
            const int a = actionAt(e->position().toPoint(), &row);
            const bool same = a == m_pressAction && row == m_pressRow;
            m_pressAction = -1;
            if (same && e->button() == Qt::LeftButton && onRowAction)
                onRowAction(row, a);
            return;
        }
        if (m_tabPressRow >= 0) {
            const QModelIndex idx = indexAt(e->position().toPoint());
            const int row = m_tabPressRow;
            m_tabPressRow = -1;
            if (idx.isValid() && idx.row() == row && onOpenInTab)
                onOpenInTab(row, !(e->modifiers() & Qt::ShiftModifier));
            e->accept();
            return;
        }
        const QModelIndex idx = indexAt(e->position().toPoint());
        QListView::mouseReleaseEvent(e);
        if (!idx.isValid() || e->button() != Qt::LeftButton)
            return;
        PageListModel *m = pageModel();
        if (m->data(idx, PageListModel::MoreRole).toBool()) {
            m->loadMore(idx.row());
            return;
        }
        if (m->isTree() && delegate->tpl.disclosure) {
            const QRect dr = delegate->disclosureRect(visualRect(idx), idx.data(PageListModel::DepthRole).toInt());
            if (dr.contains(e->position().toPoint()) && idx.data(PageListModel::ChildCountRole).toInt() > 0) {
                if (idx.data(PageListModel::ExpandedRole).toBool())
                    m->collapse(idx.row());
                else
                    m->expand(idx.row());
                return;
            }
        }
        if (clickActivates && onActivate)
            onActivate(idx.row());
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        int row = -1;
        if (actionAt(e->position().toPoint(), &row) >= 0) {
            mousePressEvent(e); // a fast second click is another click on the action
            return;
        }
        const QModelIndex idx = indexAt(e->position().toPoint());
        if (idx.isValid() && !clickActivates && onActivate)
            onActivate(idx.row());
        else
            QListView::mouseDoubleClickEvent(e);
    }

private:
    int m_pressAction = -1;
    int m_tabPressRow = -1;
    int m_pressRow = -1;
};

QString cssKey(const QString &key)
{
    QString s = QStringLiteral("n_");
    for (QChar c : key)
        s += (c.isLetterOrNumber() ? c : QLatin1Char('_'));
    return s;
}

} // namespace

// ============================================================ Lua API =====
struct ShellApi {
    static ShellHost *host(lua_State *L)
    {
        return static_cast<ShellHost *>(lua_touserdata(L, lua_upvalueindex(1)));
    }
    static void budget(lua_State *L)
    {
        if (!LuaRuntime::of(L)->consumeHostCall())
            luaL_error(L, "host call budget exceeded");
    }
    static int set(lua_State *L)
    {
        budget(L);
        ShellHost *h = host(L);
        if (h->m_inView)
            return luaL_error(L, "ctx.set is not allowed inside view(); change state from handlers");
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_rawgeti(L, LUA_REGISTRYINDEX, h->m_spec.stateRef);
        lua_pushnil(L);
        while (lua_next(L, 1) != 0) {
            lua_pushvalue(L, -2);
            lua_insert(L, -2);
            lua_settable(L, -4);
        }
        lua_pop(L, 1);
        h->scheduleRender();
        return 0;
    }
    static int toggle(lua_State *L)
    {
        budget(L);
        ShellHost *h = host(L);
        const char *k = luaL_checkstring(L, 1);
        if (h->m_inView)
            return luaL_error(L, "ctx.toggle is not allowed inside view()");
        lua_rawgeti(L, LUA_REGISTRYINDEX, h->m_spec.stateRef);
        lua_getfield(L, -1, k); // inside a protected call (ctx.toggle)
        const bool v = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_pushboolean(L, !v);
        lua_setfield(L, -2, k);
        lua_pop(L, 1);
        h->scheduleRender();
        return 0;
    }
    static int run(lua_State *L)
    {
        budget(L);
        ShellHost *h = host(L);
        if (h->m_inView)
            return luaL_error(L, "ctx.run is not allowed inside view()");
        const QString name = QString::fromUtf8(luaL_checkstring(L, 1));
        QVariantMap args;
        if (lua_istable(L, 2))
            args = toVariant(L, 2).toMap();
        const CommandResult r = h->m_d.commands->dispatchCommand(name, args);
        lua_pushboolean(L, r.ok);
        if (r.ok)
            return 1;
        const QByteArray e = r.error.toUtf8();
        lua_pushlstring(L, e.constData(), size_t(e.size()));
        return 2;
    }
    static int focus(lua_State *L)
    {
        budget(L);
        ShellHost *h = host(L);
        if (h->m_inView)
            return luaL_error(L, "ctx.focus is not allowed inside view(); use autofocus = true");
        h->m_pendingFocus << QString::fromUtf8(luaL_checkstring(L, 1));
        // Applied after the handler returns (and after any re-render it
        // caused) — never by forcing a render of its own.
        QTimer::singleShot(0, h, [h] { h->applyPendingFocus(); });
        return 0;
    }
    static int commands(lua_State *L)
    {
        budget(L);
        ShellHost *h = host(L);
        const QStringList names = h->m_d.commands->names();
        lua_createtable(L, int(names.size()), 0);
        for (int i = 0; i < names.size(); ++i) {
            const QByteArray n = names[i].toUtf8();
            lua_pushlstring(L, n.constData(), size_t(n.size()));
            lua_rawseti(L, -2, i + 1);
        }
        return 1;
    }
};

// ============================================================ host ========
ShellHost::ShellHost(const Deps &deps, QObject *parent) : QObject(parent), m_d(deps)
{
    auto *outer = new QVBoxLayout(m_d.container);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    m_banner = new QFrame(m_d.container);
    m_banner->setObjectName(QStringLiteral("folia-shell-banner"));
    auto *bl = new QHBoxLayout(m_banner);
    bl->setContentsMargins(12, 6, 8, 6);
    m_bannerText = new QLabel(m_banner);
    m_bannerText->setWordWrap(true);
    m_bannerText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_bannerReset = new QPushButton(tr("Reset to default layout"), m_banner);
    m_bannerReset->setFocusPolicy(Qt::TabFocus);
    bl->addWidget(m_bannerText, 1);
    bl->addWidget(m_bannerReset);
    auto *dismiss = new QPushButton(tr("Dismiss"), m_banner);
    bl->addWidget(dismiss);
    connect(dismiss, &QPushButton::clicked, m_banner, &QWidget::hide);
    connect(m_bannerReset, &QPushButton::clicked, this, &ShellHost::resetRequested);
    m_banner->hide();
    m_shellArea = new QWidget(m_d.container);
    auto *al = new QVBoxLayout(m_shellArea);
    al->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(m_banner);
    outer->addWidget(m_shellArea, 1);
    m_editorParking = new QWidget(m_d.container);
    m_editorParking->hide();
    slot()->setParent(m_editorParking);
    if (m_d.tabs)
        m_d.tabs->setParent(m_editorParking); // until a layout places ui.tabs{}

    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(120);
    connect(&m_reloadTimer, &QTimer::timeout, this, &ShellHost::fileChanged);
    m_renderTimer.setSingleShot(true);
    m_renderTimer.setInterval(0);
    connect(&m_renderTimer, &QTimer::timeout, this, [this] { renderNow(); });
    connect(m_d.editor->editor(), &NoteEditor::preeditChanged, this, [this](bool active) {
        if (!active)
            maybeReloadPending();
    });
    connect(m_d.data, &DataContext::currentPageChanged, this, &ShellHost::scheduleRender);
    connect(m_d.data, &DataContext::ancestorsChanged, this, &ShellHost::scheduleRender);
    // OS light/dark switch: a theme with mode = "system" follows it, and every
    // layout re-renders so ctx.prefers.dark and named style colors are current.
    m_systemScheme = QGuiApplication::styleHints()->colorScheme();
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme scheme) {
        if (scheme == m_systemScheme)
            return;
        m_systemScheme = scheme;
        if (!m_rt || m_settingScheme)
            return; // our own setColorScheme() from applyTheme()
        const QVariantMap theme = mergeTheme(m_spec.theme, m_overrides);
        if (theme.value(QStringLiteral("mode")).toString() == QLatin1String("system"))
            applyTheme(theme);
        scheduleRender();
    });
}

ShellHost::~ShellHost()
{
    // The window (and the widgets/shortcuts parented to it) may already be
    // gone at this point; only touch what is still alive.
    for (const QPointer<QShortcut> &s : m_shortcuts)
        delete s.data();
    m_shortcuts.clear();
}

size_t ShellHost::luaMemory() const
{
    return m_rt ? m_rt->memoryUsed() : 0;
}

QWidget *ShellHost::slot() const
{
    if (m_d.editorSlot)
        return m_d.editorSlot;
    return m_d.editor;
}

QWidget *ShellHost::widgetById(const QString &id) const
{
    if (id == QLatin1String("editor"))
        return m_d.editor;
    auto it = m_live.find(id);
    return it == m_live.end() ? nullptr : it->w.data();
}

void ShellHost::setSafeModeNotice(const QString &notice)
{
    m_safeNotice = notice;
    if (!notice.isEmpty())
        showBanner(notice, false);
}

void ShellHost::showBanner(const QString &text, bool error)
{
    m_bannerText->setText(text);
    m_banner->setStyleSheet(error ? QStringLiteral("#folia-shell-banner{background:#fdecea;color:#7a1c12;}")
                                  : QStringLiteral("#folia-shell-banner{background:#fff6d6;color:#5a4500;}"));
    m_bannerText->setStyleSheet(error ? QStringLiteral("color:#7a1c12;") : QStringLiteral("color:#5a4500;"));
    m_banner->show();
}

bool ShellHost::loadFile(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        const QString e = tr("Cannot read shell file %1").arg(path);
        if (error)
            *error = e;
        m_lastError = e;
        showBanner(e + tr(" — keeping the current layout."), true);
        emit loadFailed(e);
        return false;
    }
    if (f.size() > (1 << 20)) {
        const QString e = tr("Shell file is larger than 1 MiB");
        if (error)
            *error = e;
        showBanner(e, true);
        return false;
    }
    const QByteArray code = f.readAll();
    const bool ok = loadSource(code, path, error);
    if (ok) {
        m_path = path;
        if (m_watcher) {
            const QStringList files = m_watcher->files();
            if (!files.isEmpty())
                m_watcher->removePaths(files);
            m_watcher->addPath(path);
            m_watcher->addPath(QFileInfo(path).absolutePath());
        }
    } else if (m_path.isEmpty()) {
        m_path = path; // still watch it so a fix is picked up
        if (m_watcher) {
            m_watcher->addPath(path);
            m_watcher->addPath(QFileInfo(path).absolutePath());
        }
    }
    return ok;
}

bool ShellHost::loadSource(const QByteArray &code, const QString &name, QString *error)
{
    QElapsedTimer t;
    t.start();
    auto rt = std::make_unique<LuaRuntime>();
    Spec spec;
    QString err;
    UiNode root;
    std::vector<int> refs;
    QStringList oldWarnings = m_warnings;
    m_warnings.clear();
    bool ok = rt->valid() && evaluate(*rt, code, name, spec, &err);
    // view() must be evaluated against the new spec's state/ctx.
    std::unique_ptr<LuaRuntime> prevRt;
    Spec prevSpec;
    std::vector<int> prevRefs;
    if (ok) {
        prevRt = std::move(m_rt);
        prevSpec = m_spec;
        prevRefs.swap(m_renderRefs);
        m_rt = std::move(rt);
        m_spec = spec;
        ok = runView(*m_rt, m_spec, root, refs, &err) && validate(root, &err);
        if (!ok) {
            // Roll back to the previous runtime untouched.
            rt = std::move(m_rt);
            m_rt = std::move(prevRt);
            m_spec = prevSpec;
            m_renderRefs.swap(prevRefs);
        }
    }
    if (!ok) {
        m_lastError = err;
        m_warnings = oldWarnings;
        const QString shown = tr("Shell layout error in %1:\n%2\nThe last valid layout is still active.")
                                  .arg(QFileInfo(name).fileName(), err.left(800));
        showBanner(shown, true);
        if (error)
            *error = err;
        emit loadFailed(err);
        return false;
    }
    m_loadedCode = code;
    // Success: tear down the old runtime's widgets/bindings, apply the new.
    for (const QPointer<QShortcut> &s : m_shortcuts)
        delete s.data();
    m_shortcuts.clear();
    m_name = spec.name;
    m_lastError.clear();
    applyTheme(mergeTheme(m_spec.theme, m_overrides));
    apply(root);
    m_renderRefs = std::move(refs);
    applyBindings();
    prevRt.reset(); // closes the old Lua state (and all its refs)
    if (m_safeNotice.isEmpty() && m_warnings.isEmpty())
        m_banner->hide();
    else if (!m_warnings.isEmpty())
        showBanner(tr("Shell loaded with warnings:\n") + m_warnings.join(QLatin1Char('\n')), false);
    instr::record("shell_load_us", t.nsecsElapsed() / 1000);
    emit loaded(m_name);
    return true;
}

bool ShellHost::evaluate(LuaRuntime &rt, const QByteArray &code, const QString &name, Spec &spec, QString *err)
{
    lua_State *L = rt.L();
    if (!rt.run(kPrelude, QStringLiteral("prelude"), err))
        return false;
    lua_pop(L, 1);
    if (!rt.run(code, QFileInfo(name).fileName(), err))
        return false;
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        *err = QStringLiteral("the shell file must return a table (got %1)").arg(QString::fromUtf8(luaL_typename(L, -1)));
        return false;
    }
    const int top = lua_gettop(L);
    rawField(L, top, "version");
    spec.version = int(lua_tointeger(L, -1));
    lua_pop(L, 1);
    if (spec.version != 1) {
        lua_settop(L, top - 1);
        *err = QStringLiteral("unsupported shell version %1 (this Folia supports version = 1)").arg(spec.version);
        return false;
    }
    rawField(L, top, "name");
    spec.name = lua_isstring(L, -1) ? QString::fromUtf8(lua_tostring(L, -1)) : QFileInfo(name).baseName();
    lua_pop(L, 1);
    rawField(L, top, "theme");
    if (lua_istable(L, -1))
        spec.theme = toVariant(L, -1).toMap();
    lua_pop(L, 1);
    rawField(L, top, "state");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    spec.stateRef = luaL_ref(L, LUA_REGISTRYINDEX);
    rawField(L, top, "view");
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, top - 1);
        *err = QStringLiteral("the shell table needs a view = function(state, ctx) ... end");
        return false;
    }
    spec.viewRef = luaL_ref(L, LUA_REGISTRYINDEX);
    rawField(L, top, "focus_order");
    if (lua_istable(L, -1)) {
        const QVariantList l = toVariant(L, -1).toList();
        for (const QVariant &v : l)
            spec.focusOrder << v.toString();
    }
    lua_pop(L, 1);

    // ctx table with host functions.
    lua_newtable(L);
    struct Fn { const char *name; lua_CFunction fn; };
    const Fn fns[] = {{"set", &ShellApi::set}, {"toggle", &ShellApi::toggle}, {"run", &ShellApi::run},
                      {"focus", &ShellApi::focus}, {"commands", &ShellApi::commands}};
    for (const Fn &f : fns) {
        lua_pushstring(L, f.name);
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, f.fn, 1);
        lua_rawset(L, -3);
    }
    spec.ctxRef = luaL_ref(L, LUA_REGISTRYINDEX);

    // Key bindings.
    rawField(L, top, "keys");
    if (lua_istable(L, -1)) {
        const int kt = lua_gettop(L);
        QHash<QString, QString> seen;
        lua_pushnil(L);
        while (lua_next(L, kt) != 0) {
            if (lua_type(L, -2) != LUA_TSTRING) {
                lua_pop(L, 1);
                continue;
            }
            const QString keyText = QString::fromUtf8(lua_tostring(L, -2));
            QString kerr;
            Binding b;
            b.keyText = keyText;
            b.seq = parseKeys(keyText, &kerr);
            if (b.seq.isEmpty()) {
                m_warnings << QStringLiteral("keys['%1']: %2").arg(keyText, kerr);
                lua_pop(L, 1);
                continue;
            }
            const QString norm = b.seq.toString(QKeySequence::PortableText);
            bool reserved = false;
            for (const QKeySequence &r : reservedKeys())
                if (r.matches(b.seq) == QKeySequence::ExactMatch)
                    reserved = true;
            if (reserved) {
                m_warnings << QStringLiteral("keys['%1'] is reserved for editing/recovery and was ignored").arg(keyText);
                lua_pop(L, 1);
                continue;
            }
            if (seen.contains(norm)) {
                m_warnings << QStringLiteral("keys['%1'] conflicts with keys['%2'] (same shortcut); keeping '%2'")
                                  .arg(keyText, seen.value(norm));
                lua_pop(L, 1);
                continue;
            }
            const int vt = lua_type(L, -1);
            bool valid = true;
            if (vt == LUA_TFUNCTION) {
                lua_pushvalue(L, -1);
                b.fnRef = luaL_ref(L, LUA_REGISTRYINDEX);
            } else if (vt == LUA_TSTRING) {
                b.commands << QString::fromUtf8(lua_tostring(L, -1));
            } else if (vt == LUA_TTABLE) {
                const QVariant v = toVariant(L, -1);
                if (v.typeId() == QMetaType::QVariantList) {
                    for (const QVariant &c : v.toList())
                        b.commands << c.toString();
                } else {
                    const QVariantMap m = v.toMap();
                    b.commands << m.value(QStringLiteral("command")).toString();
                    b.args = m.value(QStringLiteral("args")).toMap();
                }
            } else {
                valid = false;
            }
            for (const QString &c : std::as_const(b.commands)) {
                const CommandResult cr = m_d.commands->validate(c, b.commands.size() == 1 ? b.args : QVariantMap());
                if (!m_d.commands->has(c)) {
                    m_warnings << QStringLiteral("keys['%1']: %2").arg(keyText, cr.error);
                    valid = false;
                }
            }
            if (valid) {
                seen.insert(norm, keyText);
                spec.keys.push_back(b);
            } else if (b.fnRef >= 0) {
                luaL_unref(L, LUA_REGISTRYINDEX, b.fnRef);
            }
            lua_pop(L, 1);
        }
    }
    lua_settop(L, top - 1);
    return true;
}

void ShellHost::pushCtx()
{
    // Raw writes only: the ctx table is visible to shell code, which may have
    // given it a metatable; host code must never run user metamethods
    // outside a protected call.
    lua_State *L = m_rt->L();
    auto rawSet = [L](const char *k) { // value on top, table below it
        lua_pushstring(L, k);
        lua_insert(L, -2);
        lua_rawset(L, -3);
    };
    lua_rawgeti(L, LUA_REGISTRYINDEX, m_spec.ctxRef);
    lua_createtable(L, 0, 4);
    const PageId cur = m_d.data->currentPage();
    lua_pushinteger(L, lua_Integer(cur));
    rawSet("id");
    const QByteArray title = m_d.data->titleOf(cur).toUtf8();
    lua_pushlstring(L, title.constData(), size_t(title.size()));
    rawSet("title");
    const auto &anc = m_d.data->ancestors();
    lua_pushinteger(L, lua_Integer(anc.size() >= 2 ? anc[anc.size() - 2].id : 0));
    rawSet("parent");
    lua_pushinteger(L, lua_Integer(qMax(0, int(anc.size()) - 1)));
    rawSet("depth");
    rawSet("page");
    const QByteArray plat = platformName().toUtf8();
    lua_pushstring(L, plat.constData());
    rawSet("platform");
    lua_createtable(L, 0, 3);
    lua_pushboolean(L, m_systemScheme == Qt::ColorScheme::Dark);
    rawSet("dark");
    // System accessibility preferences (env vars force them on for tests).
    lua_pushboolean(L, platform::reducedMotion() || qEnvironmentVariableIsSet("FOLIA_REDUCED_MOTION"));
    rawSet("reduced_motion");
    lua_pushboolean(L, platform::reducedTransparency() || qEnvironmentVariableIsSet("FOLIA_REDUCED_TRANSPARENCY"));
    rawSet("reduced_transparency");
    rawSet("prefers");
    lua_rawgeti(L, LUA_REGISTRYINDEX, m_spec.stateRef);
    rawSet("state");
}

bool ShellHost::runView(LuaRuntime &rt, Spec &spec, UiNode &root, std::vector<int> &refs, QString *err)
{
    lua_State *L = rt.L();
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, spec.viewRef);
    lua_rawgeti(L, LUA_REGISTRYINDEX, spec.stateRef);
    pushCtx();
    m_inView = true;
    const bool ok = rt.call(2, 1, err);
    m_inView = false;
    if (!ok) {
        lua_settop(L, top);
        return false;
    }
    int count = 0;
    const bool conv = toNode(rt, -1, root, 0, count, QStringLiteral("root"), refs, err);
    lua_settop(L, top);
    if (!conv) {
        for (int r : refs)
            luaL_unref(L, LUA_REGISTRYINDEX, r);
        refs.clear();
        return false;
    }
    instr::add(instr::LuaRenders);
    return true;
}

bool ShellHost::toNode(LuaRuntime &rt, int idx, UiNode &n, int depth, int &count, const QString &path,
                       std::vector<int> &refs, QString *err)
{
    lua_State *L = rt.L();
    idx = lua_absindex(L, idx);
    if (depth > m_lim.maxDepth) {
        *err = QStringLiteral("%1: layout nested deeper than %2 levels").arg(path).arg(m_lim.maxDepth);
        return false;
    }
    if (++count > m_lim.maxNodes) {
        *err = QStringLiteral("layout has more than %1 nodes").arg(m_lim.maxNodes);
        return false;
    }
    if (!lua_istable(L, idx)) {
        *err = QStringLiteral("%1: expected a ui node table, got %2").arg(path, QString::fromUtf8(luaL_typename(L, idx)));
        return false;
    }
    rawField(L, idx, "type");
    n.type = QString::fromUtf8(lua_tostring(L, -1));
    lua_pop(L, 1);
    if (!knownTypes().contains(n.type)) {
        *err = QStringLiteral("%1: unknown node type '%2' (use ui.row, ui.column, ui.overlay, ui.text, ui.button, "
                              "ui.input, ui.list, ui.editor, ui.tabs, ui.spacer, ui.divider)").arg(path, n.type);
        return false;
    }
    // Scalars, style, handlers, source/row descriptors.
    lua_Unsigned maxIndex = 0; // largest positive integer key (children)
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        if (lua_isinteger(L, -2)) {
            const lua_Integer k = lua_tointeger(L, -2);
            if (k > 0 && lua_Unsigned(k) > maxIndex)
                maxIndex = lua_Unsigned(k);
        } else if (lua_type(L, -2) == LUA_TSTRING) {
            const QString k = QString::fromUtf8(lua_tostring(L, -2));
            const int vt = lua_type(L, -1);
            if (k.startsWith(QLatin1String("on_")) && vt == LUA_TFUNCTION) {
                lua_pushvalue(L, -1);
                const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
                refs.push_back(ref);
                n.handlers.insert(k, ref);
            } else if (k == QLatin1String("style") && vt == LUA_TTABLE) {
                const QVariantMap s = toVariant(L, -1).toMap();
                for (auto it = s.cbegin(); it != s.cend(); ++it)
                    n.props.insert(QStringLiteral("style.") + it.key(), it.value());
            } else if (k == QLatin1String("source") && vt == LUA_TTABLE) {
                n.source = toVariant(L, -1).toMap();
            } else if (k == QLatin1String("row") && vt == LUA_TTABLE) {
                n.row = toVariant(L, -1).toMap();
            } else if (vt == LUA_TSTRING || vt == LUA_TNUMBER || vt == LUA_TBOOLEAN) {
                n.props.insert(k, toVariant(L, -1));
            } else if (vt == LUA_TFUNCTION) {
                lua_pop(L, 2);
                *err = QStringLiteral("%1: '%2' is a function but only on_* fields may be handlers").arg(path, k);
                return false;
            }
        }
        lua_pop(L, 1);
    }
    n.id = n.props.value(QStringLiteral("id")).toString();
    // `cond and node or nil` leaves holes, for which the length operator is
    // undefined (Lua 5.5 may stop at the first nil): walk to the largest key.
    const lua_Unsigned len = qMax(lua_rawlen(L, idx), maxIndex);
    for (lua_Unsigned i = 1; i <= len; ++i) {
        lua_rawgeti(L, idx, lua_Integer(i));
        if (lua_isnil(L, -1) || (lua_isboolean(L, -1) && !lua_toboolean(L, -1))) {
            lua_pop(L, 1); // nil/false children are skipped (conditional layouts)
            continue;
        }
        UiNode c;
        const QString cpath = path + QStringLiteral("/") + QString::number(i);
        if (!toNode(rt, -1, c, depth + 1, count, cpath, refs, err)) {
            lua_pop(L, 1);
            return false;
        }
        lua_pop(L, 1);
        n.children.push_back(std::move(c));
    }
    return true;
}

bool ShellHost::validate(const UiNode &root, QString *err) const
{
    int editors = 0;
    int tabStrips = 0;
    QSet<QString> ids;
    QString problem;
    std::function<void(const UiNode &, const QString &)> walk = [&](const UiNode &n, const QString &parentKey) {
        if (!problem.isEmpty())
            return;
        if (n.type == QLatin1String("editor"))
            ++editors;
        if (n.type == QLatin1String("tabs") && ++tabStrips > 1)
            problem = QStringLiteral("the layout has more than one ui.tabs{} (at most one is allowed)");
        if (!n.id.isEmpty()) {
            if (ids.contains(n.id))
                problem = QStringLiteral("duplicate id '%1'").arg(n.id);
            if ((n.id == QLatin1String("editor") || n.id == QLatin1String("title")) && n.type != QLatin1String("editor"))
                problem = QStringLiteral("id '%1' is reserved for the editor").arg(n.id);
            if (n.id == QLatin1String("tabs") && n.type != QLatin1String("tabs"))
                problem = QStringLiteral("id 'tabs' is reserved for ui.tabs{}");
            ids.insert(n.id);
        }
        if (n.type == QLatin1String("button")) {
            const QString icon = n.props.value(QStringLiteral("icon")).toString();
            if (!icon.isEmpty() && !icons::has(icon))
                problem = QStringLiteral("button '%1': unknown icon '%2' (available: %3)")
                              .arg(n.id.isEmpty() ? n.key : n.id, icon, icons::names().join(QStringLiteral(", ")));
        }
        if (n.type == QLatin1String("list")) {
            for (const QVariant &v : n.row.value(QStringLiteral("actions")).toList()) {
                const QVariantMap a = v.toMap();
                const QString icon = a.value(QStringLiteral("icon")).toString();
                const QString cmd = a.value(QStringLiteral("command")).toString();
                QVariantMap args = a.value(QStringLiteral("args")).toMap();
                args.insert(a.value(QStringLiteral("id_arg"), QStringLiteral("id")).toString(), qint64(1));
                if (!icons::has(icon))
                    problem = QStringLiteral("list '%1': row action needs a known icon, got '%2' (available: %3)")
                                  .arg(n.id, icon, icons::names().join(QStringLiteral(", ")));
                else if (const CommandResult r = m_d.commands->validate(cmd, args); !r.ok)
                    problem = QStringLiteral("list '%1': row action '%2': %3").arg(n.id, cmd, r.error);
            }
            const QString k = n.source.value(QStringLiteral("kind")).toString();
            static const QSet<QString> kinds = {QStringLiteral("tree"), QStringLiteral("search"), QStringLiteral("recent"),
                                                QStringLiteral("children"), QStringLiteral("ancestors")};
            if (!kinds.contains(k))
                problem = QStringLiteral("list '%1' needs source = data.tree{} / data.search{} / data.recent{} / "
                                         "data.children{} / data.ancestors{}").arg(n.id);
        }
        const bool container = n.type == QLatin1String("row") || n.type == QLatin1String("column")
            || n.type == QLatin1String("overlay");
        if (!container && !n.children.isEmpty())
            problem = QStringLiteral("'%1' nodes cannot have children").arg(n.type);
        Q_UNUSED(parentKey);
        for (const UiNode &c : n.children)
            walk(c, n.key);
    };
    walk(root, QString());
    if (problem.isEmpty() && editors != 1)
        problem = editors == 0 ? QStringLiteral("the layout has no ui.editor{} slot (exactly one is required)")
                               : QStringLiteral("the layout has %1 ui.editor{} slots (exactly one is allowed)").arg(editors);
    if (!problem.isEmpty()) {
        *err = problem;
        return false;
    }
    return true;
}

// --------------------------------------------------------- reconcile -----
static void assignKeys(UiNode &n, const QString &parentKey, int index)
{
    if (n.type == QLatin1String("editor"))
        n.key = QStringLiteral("editor");
    else if (n.type == QLatin1String("tabs"))
        n.key = QStringLiteral("tabs");
    else if (!n.id.isEmpty())
        n.key = n.id;
    else
        n.key = parentKey + QLatin1Char('/') + n.type + QString::number(index);
    for (int i = 0; i < n.children.size(); ++i)
        assignKeys(n.children[i], n.key, i);
}

namespace {

bool hasNodeOfType(const UiNode &n, const QString &type)
{
    if (n.type == type)
        return true;
    for (const UiNode &c : n.children)
        if (hasNodeOfType(c, type))
            return true;
    return false;
}

// Wraps the editor node in a column with ui.tabs{} above it. The column
// takes the editor's place: its size props move over, and it shows and
// hides with it.
bool wrapEditorWithTabs(UiNode &n)
{
    if (n.type != QLatin1String("editor")) {
        for (UiNode &c : n.children)
            if (wrapEditorWithTabs(c))
                return true;
        return false;
    }
    UiNode column;
    column.type = QStringLiteral("column");
    column.props.insert(QStringLiteral("style.grow"), 1); // as the editor grows by default
    static const char *const kSize[] = {"grow", "style.grow", "width", "style.width", "min_width", "style.min_width",
                                        "max_width", "style.max_width", "height", "style.height"};
    for (const char *k : kSize) {
        const QString key = QString::fromLatin1(k);
        if (n.props.contains(key))
            column.props.insert(key, n.props.take(key));
    }
    if (n.props.contains(QStringLiteral("visible")))
        column.props.insert(QStringLiteral("visible"), n.props.value(QStringLiteral("visible")));
    UiNode strip;
    strip.type = QStringLiteral("tabs");
    column.children.push_back(std::move(strip));
    column.children.push_back(std::move(n));
    n = std::move(column);
    return true;
}

} // namespace

void ShellHost::apply(const UiNode &rootIn)
{
    QElapsedTimer t;
    t.start();
    UiNode root = rootIn;
    // A layout without ui.tabs{} (an edited copy from before tabs, say)
    // still shows the note tabs: the strip goes directly above the editor.
    // A layout that wants none says ui.tabs{ visible = false }.
    if (m_d.tabs && !hasNodeOfType(root, QStringLiteral("tabs")))
        wrapEditorWithTabs(root);
    assignKeys(root, QStringLiteral("root"), 0);
    QPointer<QWidget> focused = QApplication::focusWidget();
    // The slot holds the editor and, in a split, the PDF tab: whichever had
    // focus keeps it if a reload re-parents the slot.
    const bool slotHadFocus = focused && (focused == slot() || slot()->isAncestorOf(focused));
    QSet<QString> seen;
    QWidget *w = realize(root, seen, m_shellArea);
    if (m_rootWidget != w) {
        auto *lay = static_cast<QVBoxLayout *>(m_shellArea->layout());
        while (lay->count())
            delete lay->takeAt(0);
        lay->addWidget(w);
        m_rootWidget = w;
    }
    // Remove widgets that are no longer in the layout.
    for (auto it = m_live.begin(); it != m_live.end();) {
        if (!seen.contains(it.key())) {
            if (it->w && it->w != slot() && it->w != m_d.tabs) {
                it->w->hide();
                it->w->deleteLater();
            }
            it = m_live.erase(it);
        } else {
            ++it;
        }
    }
    if (!seen.contains(QStringLiteral("editor")))
        slot()->setParent(m_editorParking);
    if (m_d.tabs && !seen.contains(QStringLiteral("tabs")) && m_d.tabs->parentWidget() != m_editorParking)
        m_d.tabs->setParent(m_editorParking); // parked, never deleted
    applyFocusOrder(root);
    if (slotHadFocus && !QApplication::focusWidget()) {
        if (focused && focused->isVisible() && !m_d.editor->isAncestorOf(focused))
            focused->setFocus(Qt::OtherFocusReason);
        else if (m_d.editor->isVisible())
            m_d.editor->editor()->setFocus(Qt::OtherFocusReason);
    }
    applyPendingFocus();
    // A changed style sheet above re-polishes the editor, and Qt's style-sheet
    // style then restores the palettes it saw at the previous polish (the old
    // theme: a white body and dark title in dark mode). Re-assert the current
    // ones; unchanged palettes and fonts make this a no-op.
    m_d.editor->applyTheme();
    instr::record("shell_reconcile_us", t.nsecsElapsed() / 1000);
}

void ShellHost::applyPendingFocus()
{
    if (m_renderTimer.isActive())
        return; // a render is pending: it applies the focus once widgets exist
    const QStringList focusReq = m_pendingFocus;
    m_pendingFocus.clear();
    for (const QString &id : focusReq) {
        if (id == QLatin1String("editor")) {
            m_d.editor->focusBody();
        } else if (id == QLatin1String("title")) {
            m_d.editor->focusTitle();
        } else if (QWidget *fw = widgetById(id)) {
            if (fw->isVisible())
                fw->setFocus(Qt::OtherFocusReason);
        }
    }
}

QString ShellHost::resolveColor(const QVariant &v, const QString &fallback) const
{
    QString s = v.toString().trimmed();
    if (s.isEmpty())
        s = fallback;
    if (s.isEmpty())
        return {};
    if (s.startsWith(QLatin1Char('#')) || s.startsWith(QLatin1String("rgb")))
        return s;
    return m_colors.value(s, s);
}

void ShellHost::updateCommon(QWidget *w, const UiNode &n)
{
    const QVariantMap &p = n.props;
    const QString obj = cssKey(n.key);
    w->setObjectName(obj);
    QStringList css;
    auto sv = [&](const char *k) { return p.value(QStringLiteral("style.") + QLatin1String(k)); };
    const QString bg = resolveColor(sv("bg"));
    const QString fg = resolveColor(sv("fg"));
    if (!bg.isEmpty())
        css << QStringLiteral("background-color:%1;").arg(bg);
    if (!fg.isEmpty())
        css << QStringLiteral("color:%1;").arg(fg);
    if (sv("radius").isValid())
        css << QStringLiteral("border-radius:%1px;").arg(sv("radius").toInt());
    if (sv("border").isValid())
        css << QStringLiteral("border:1px solid %1;").arg(resolveColor(sv("border")));
    else if (n.type == QLatin1String("input") || n.type == QLatin1String("list") || n.type == QLatin1String("button"))
        css << QStringLiteral("border:none;");
    if (sv("padding").isValid() && n.type != QLatin1String("row") && n.type != QLatin1String("column"))
        css << QStringLiteral("padding:%1px;").arg(sv("padding").toInt());
    const QString sel = n.type == QLatin1String("list") ? QStringLiteral("QListView#") : QStringLiteral("#");
    QString sheet = css.isEmpty() ? QString() : sel + obj + QStringLiteral("{") + css.join(QString()) + QStringLiteral("}");
    if (n.type == QLatin1String("button")) {
        // Flat buttons get a soft hover/pressed background (theme "hover_bg").
        const QString hover = m_colors.value(QStringLiteral("hover_bg"));
        if (!sv("radius").isValid())
            sheet += QStringLiteral("#%1{border-radius:6px;}").arg(obj);
        if (!hover.isEmpty())
            sheet += QStringLiteral("#%1:hover{background-color:%2;}#%1:pressed{background-color:%3;}")
                         .arg(obj, hover, resolveColor(QStringLiteral("border")));
    }
    // QWidget::setStyleSheet has no same-string check: it re-polishes the
    // widget and every descendant (style-change events, relayout, repaint)
    // even for an identical sheet. Containers reach the editor, so an
    // unchanged render used to repaint the whole window.
    if (n.type != QLatin1String("tabs") && w->styleSheet() != sheet)
        w->setStyleSheet(sheet);
    QFont f = m_d.editorCtx->theme().body;
    f.setPointSizeF(qMax(9.0, f.pointSizeF() - 2));
    if (sv("font").isValid())
        f.setFamily(sv("font").toString());
    if (sv("size").isValid())
        f.setPointSizeF(sv("size").toDouble());
    if (sv("weight").isValid()) {
        const QString wt = sv("weight").toString();
        f.setWeight(wt == QLatin1String("bold") ? QFont::Bold : wt == QLatin1String("medium") ? QFont::Medium
                    : wt == QLatin1String("light") ? QFont::Light : QFont::Weight(qBound(100, sv("weight").toInt(), 900)));
    }
    if (n.type != QLatin1String("editor"))
        w->setFont(f);
    if (sv("width").isValid())
        w->setFixedWidth(sv("width").toInt());
    else {
        w->setMinimumWidth(sv("min_width").isValid() ? sv("min_width").toInt() : 0);
        w->setMaximumWidth(sv("max_width").isValid() ? sv("max_width").toInt() : QWIDGETSIZE_MAX);
    }
    if (n.type == QLatin1String("tabs")) {
        // The strip sizes itself (style.height is part of its appearance).
    } else if (sv("height").isValid())
        w->setFixedHeight(sv("height").toInt());
    else if (n.type != QLatin1String("editor")) {
        w->setMinimumHeight(0);
        w->setMaximumHeight(QWIDGETSIZE_MAX);
    }
    if (p.contains(QStringLiteral("tooltip")))
        w->setToolTip(p.value(QStringLiteral("tooltip")).toString());
    const QString a11y = p.value(QStringLiteral("label"), p.value(QStringLiteral("text"))).toString();
    if (!a11y.isEmpty())
        w->setAccessibleName(a11y);
    if (p.contains(QStringLiteral("focusable")))
        w->setFocusPolicy(p.value(QStringLiteral("focusable")).toBool() ? Qt::StrongFocus : Qt::NoFocus);
}

QWidget *ShellHost::realize(const UiNode &n, QSet<QString> &seen, QWidget *parent)
{
    seen.insert(n.key);
    Live &lv = m_live[n.key]; // valid only until children are realized (rehash)
    const bool reuse = lv.w && lv.type == n.type;
    if (!reuse && lv.w && lv.w != slot() && lv.w != m_d.tabs)
        lv.w->deleteLater();
    lv.type = n.type;
    const QVariantMap &p = n.props;
    const bool visible = p.value(QStringLiteral("visible"), true).toBool();
    QWidget *w = reuse ? lv.w.data() : nullptr;
    instr::add(reuse ? instr::WidgetsReused : instr::WidgetsCreated);

    if (n.type == QLatin1String("editor")) {
        w = slot();
    } else if (n.type == QLatin1String("tabs")) {
        if (auto *strip = qobject_cast<TabStrip *>(m_d.tabs)) {
            w = strip;
            // Native colors and sizes, no style sheet: compared before any
            // repaint, so an unchanged render leaves the strip alone.
            auto sv = [&](const char *k) { return p.value(QStringLiteral("style.") + QLatin1String(k)); };
            auto color = [&](const char *k, const char *fallback) {
                return QColor(resolveColor(sv(k), QString::fromLatin1(fallback)));
            };
            TabStrip::Appearance a;
            a.bg = color("bg", "sidebar");
            a.activeBg = color("active_bg", "background");
            a.text = color("fg", "text");
            a.dim = color("dim", "dim");
            a.hoverBg = color("hover_bg", "hover_bg");
            a.border = color("border", "border");
            a.minTabWidth = qBound(40, p.value(QStringLiteral("tab_min_width"), 72).toInt(), 400);
            a.maxTabWidth = qBound(a.minTabWidth, p.value(QStringLiteral("tab_max_width"), 220).toInt(), 600);
            a.height = qBound(24, sv("height").isValid() ? sv("height").toInt() : 36, 64);
            strip->setAppearance(a);
            strip->setAutoHide(p.value(QStringLiteral("auto_hide"), false).toBool());
        } else if (!w) {
            w = new QWidget(parent); // a host without a strip (tests): an empty slot
            w->setFixedHeight(0);
        }
    } else if (n.type == QLatin1String("row") || n.type == QLatin1String("column")) {
        const bool horiz = n.type == QLatin1String("row");
        auto *box = static_cast<ShellBox *>(w);
        if (!box)
            w = box = new ShellBox(horiz, parent);
        box->setDirection(horiz);
        const int pad = p.value(QStringLiteral("style.padding"), 0).toInt();
        box->box()->setContentsMargins(pad, pad, pad, pad);
        box->box()->setSpacing(p.value(QStringLiteral("style.gap"), p.value(QStringLiteral("gap"), 0)).toInt());
        QVector<QPair<QWidget *, int>> kids;
        for (const UiNode &c : n.children) {
            QWidget *cw = realize(c, seen, box);
            if (cw->parentWidget() != box)
                cw->setParent(box);
            int grow = c.props.value(QStringLiteral("style.grow"), c.props.value(QStringLiteral("grow"), -1)).toInt();
            if (grow < 0)
                grow = (c.type == QLatin1String("editor") || c.type == QLatin1String("spacer")) ? 1 : 0;
            kids.push_back({cw, grow});
        }
        box->setChildren(kids);
    } else if (n.type == QLatin1String("overlay")) {
        auto *ov = static_cast<ShellOverlay *>(w);
        if (!ov)
            w = ov = new ShellOverlay(parent);
        QWidget *base = nullptr;
        QVector<ShellOverlay::Pop> pops;
        for (int i = 0; i < n.children.size(); ++i) {
            const UiNode &c = n.children[i];
            QWidget *cw = realize(c, seen, ov);
            if (i == 0)
                base = cw;
            else
                pops.push_back({cw, c.props.value(QStringLiteral("style.width"), 560).toInt(),
                                c.props.value(QStringLiteral("style.top"), 72).toInt()});
        }
        ov->setChildren(base, pops);
    } else if (n.type == QLatin1String("text")) {
        auto *l = static_cast<QLabel *>(w);
        if (!l)
            w = l = new QLabel(parent);
        const QString bind = p.value(QStringLiteral("bind")).toString();
        QObject::disconnect(m_d.data, nullptr, l, nullptr);
        auto refresh = [this, l, bind, p] {
            if (bind == QLatin1String("status"))
                l->setText(m_d.data->status());
            else if (bind == QLatin1String("title"))
                l->setText(m_d.data->titleOf(m_d.data->currentPage()));
            else if (bind == QLatin1String("path")) {
                QStringList t;
                for (const PageMeta &m : m_d.data->ancestors())
                    t << m.title;
                l->setText(t.join(QStringLiteral(" / ")));
            } else
                l->setText(p.value(QStringLiteral("text")).toString());
        };
        refresh();
        if (bind == QLatin1String("status"))
            connect(m_d.data, &DataContext::statusChanged, l, refresh);
        else if (bind == QLatin1String("title") || bind == QLatin1String("path")) {
            connect(m_d.data, &DataContext::ancestorsChanged, l, refresh);
            connect(m_d.data, &DataContext::titleChanged, l, refresh);
        }
        l->setWordWrap(p.value(QStringLiteral("wrap"), false).toBool());
        const QString align = p.value(QStringLiteral("style.align")).toString();
        l->setAlignment((align == QLatin1String("center") ? Qt::AlignHCenter
                         : align == QLatin1String("right") ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
        l->setTextInteractionFlags(Qt::NoTextInteraction);
    } else if (n.type == QLatin1String("button")) {
        auto *b = static_cast<QPushButton *>(w);
        if (!b)
            w = b = new QPushButton(parent);
        b->setText(p.value(QStringLiteral("text")).toString());
        const QString icon = p.value(QStringLiteral("icon")).toString();
        // Lucide icon tinted with the button's text color. QAbstractButton::
        // setIcon always updates and relayouts, so rebuild only on change
        // (a leaf: the Live reference is still valid here).
        QString iconKey;
        if (!reuse)
            lv.iconKey.clear(); // a fresh widget has no icon yet
        if (!icon.isEmpty()) {
            const QColor fg(resolveColor(p.value(QStringLiteral("style.fg")), QStringLiteral("text")));
            const int sz = p.value(QStringLiteral("style.icon_size"), 16).toInt();
            iconKey = icon + QLatin1Char('/') + fg.name(QColor::HexArgb) + QLatin1Char('/') + QString::number(sz);
            if (lv.iconKey != iconKey) {
                b->setIcon(icons::icon(icon, fg));
                b->setIconSize(QSize(sz, sz));
            }
        } else if (!lv.iconKey.isEmpty() || !reuse) {
            b->setIcon(QIcon());
        }
        lv.iconKey = iconKey;
        if (b->text().isEmpty() && icon.isEmpty())
            b->setText(p.value(QStringLiteral("label")).toString());
        b->setFlat(p.value(QStringLiteral("flat"), true).toBool());
        b->setCursor(Qt::PointingHandCursor);
        b->disconnect(this);
        const int ref = n.handlers.value(QStringLiteral("on_click"), -1);
        const QString cmd = p.value(QStringLiteral("command")).toString();
        connect(b, &QPushButton::clicked, this, [this, ref, cmd] {
            if (ref >= 0)
                invokeHandler(ref, {}, {}, false);
            else if (!cmd.isEmpty()) {
                const CommandResult r = m_d.commands->dispatchCommand(cmd, {});
                if (!r.ok)
                    showBanner(r.error, true);
            }
        });
    } else if (n.type == QLatin1String("input")) {
        auto *in = static_cast<ShellInput *>(w);
        const bool fresh = !in;
        if (!in)
            w = in = new ShellInput(parent);
        in->setPlaceholderText(p.value(QStringLiteral("placeholder")).toString());
        if (fresh && p.contains(QStringLiteral("value")))
            in->setText(p.value(QStringLiteral("value")).toString());
        in->disconnect(this);
        const int onChange = n.handlers.value(QStringLiteral("on_change"), -1);
        const int onSubmit = n.handlers.value(QStringLiteral("on_submit"), -1);
        const int onCancel = n.handlers.value(QStringLiteral("on_cancel"), -1);
        const QString listId = p.value(QStringLiteral("list")).toString();
        connect(in, &QLineEdit::textEdited, this, [this, onChange, listId](const QString &t) {
            if (!listId.isEmpty()) {
                if (auto *lw = dynamic_cast<ShellList *>(widgetById(listId)))
                    if (auto *fs = dynamic_cast<FlatSource *>(lw->pageModel()))
                        fs->setQuery(t); // native: no Lua round-trip per keystroke
            }
            if (onChange >= 0)
                invokeHandler(onChange, {}, t, true);
        });
        in->onKey = [this, onSubmit, onCancel, listId, in](QKeyEvent *e) {
            if (e->key() == Qt::Key_Escape && onCancel >= 0) {
                invokeHandler(onCancel, {}, in->text(), true);
                return true;
            }
            auto *lw = listId.isEmpty() ? nullptr : dynamic_cast<ShellList *>(widgetById(listId));
            if (lw) {
                const int k = e->key();
                if (k == Qt::Key_Up || k == Qt::Key_Down || k == Qt::Key_PageUp || k == Qt::Key_PageDown
                    || k == Qt::Key_Return || k == Qt::Key_Enter)
                    return lw->navigate(e);
            }
            if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && onSubmit >= 0) {
                invokeHandler(onSubmit, {}, in->text(), true);
                return true;
            }
            return false;
        };
    } else if (n.type == QLatin1String("list")) {
        auto *lw = static_cast<ShellList *>(w);
        if (!lw)
            w = lw = new ShellList(parent);
        const QString kind = n.source.value(QStringLiteral("kind")).toString();
        if (!lv.model || lv.sourceKind != kind) {
            PageListModel *old = lv.model;
            if (kind == QLatin1String("tree"))
                lv.model = new TreeSource(m_d.data, lw);
            else if (kind == QLatin1String("search"))
                lv.model = new FlatSource(FlatSource::Kind::Search, m_d.data, lw);
            else if (kind == QLatin1String("recent"))
                lv.model = new FlatSource(FlatSource::Kind::Recent, m_d.data, lw);
            else if (kind == QLatin1String("children"))
                lv.model = new FlatSource(FlatSource::Kind::Children, m_d.data, lw);
            else
                lv.model = new FlatSource(FlatSource::Kind::Ancestors, m_d.data, lw);
            lv.sourceKind = kind;
            lw->setModel(lv.model);
            delete old;
            const bool selectFirst = p.value(QStringLiteral("select_first"), kind == QLatin1String("search")).toBool();
            const bool follow = p.value(QStringLiteral("follow_current"), kind == QLatin1String("tree")).toBool();
            PageListModel *model = lv.model;
            connect(model, &PageListModel::reloaded, lw, [lw, selectFirst] {
                if (selectFirst)
                    lw->setCurrentRow(0);
            });
            connect(model, &PageListModel::revealed, lw, [lw](int row) { lw->setCurrentRow(row); });
            if (follow) {
                auto sync = [this, lw, model] {
                    const PageId cur = m_d.data->currentPage();
                    const int r = model->rowOf(cur);
                    if (r >= 0)
                        lw->setCurrentRow(r);
                    else if (auto *tree = dynamic_cast<TreeSource *>(model))
                        tree->reveal(cur);
                };
                connect(m_d.data, &DataContext::currentPageChanged, lw, sync);
                connect(model, &QAbstractItemModel::rowsInserted, lw, [this, lw, model] {
                    const int r = model->rowOf(m_d.data->currentPage());
                    if (r >= 0 && lw->current() != r)
                        lw->setCurrentRow(r);
                });
            }
        }
        lv.model->configure(n.source);
        const bool horizontal = p.value(QStringLiteral("orientation")).toString() == QLatin1String("horizontal");
        lw->setFlow(horizontal ? QListView::LeftToRight : QListView::TopToBottom);
        lw->setWrapping(false);
        lw->setUniformItemSizes(!horizontal);
        lw->setVerticalScrollBarPolicy(horizontal ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
        PageRowDelegate::Template &tpl = lw->delegate->tpl;
        const QVariantMap &row = n.row;
        tpl.height = row.value(QStringLiteral("height"), horizontal ? 28 : 26).toInt();
        tpl.indent = row.value(QStringLiteral("indent"), true).toBool();
        tpl.disclosure = row.value(QStringLiteral("disclosure"), kind == QLatin1String("tree")).toBool();
        tpl.icon = row.value(QStringLiteral("icon"), false).toBool();
        tpl.detailCount = row.value(QStringLiteral("detail")).toString() == QLatin1String("count");
        tpl.horizontal = horizontal;
        tpl.separator = row.value(QStringLiteral("separator"), QStringLiteral("  /")).toString();
        tpl.text = QColor(resolveColor(p.value(QStringLiteral("style.fg")), QStringLiteral("text")));
        tpl.dim = QColor(resolveColor(row.value(QStringLiteral("dim")), QStringLiteral("dim")));
        tpl.selectedBg = QColor(resolveColor(row.value(QStringLiteral("selected_bg")), QStringLiteral("selection_bg")));
        tpl.selectedText = QColor(resolveColor(row.value(QStringLiteral("selected_fg")), QStringLiteral("selection_text")));
        tpl.hoverBg = QColor(resolveColor(row.value(QStringLiteral("hover_bg")), QStringLiteral("hover_bg")));
        tpl.font = lw->font();
        tpl.actions.clear();
        for (const QVariant &v : row.value(QStringLiteral("actions")).toList()) {
            const QVariantMap a = v.toMap();
            tpl.actions.push_back({a.value(QStringLiteral("icon")).toString(), a.value(QStringLiteral("command")).toString(),
                                   a.value(QStringLiteral("id_arg"), QStringLiteral("id")).toString(),
                                   a.value(QStringLiteral("args")).toMap(), a.value(QStringLiteral("tooltip")).toString()});
        }
        if (tpl.actions.isEmpty())
            lw->setHover(-1, -1);
        lw->onRowAction = [this, lw](int r, int a) {
            const PageRowDelegate::Template &t = lw->delegate->tpl;
            if (!lw->pageModel() || a < 0 || a >= int(t.actions.size()))
                return;
            const PageRowDelegate::Action act = t.actions[a]; // the handler may re-render the shell
            QVariantMap args = act.args;
            args.insert(act.idArg, qint64(lw->pageModel()->idAt(r)));
            const CommandResult res = m_d.commands->dispatchCommand(act.command, args);
            if (!res.ok)
                showBanner(res.error, true);
        };
        lw->clickActivates = p.value(QStringLiteral("click_activates"), true).toBool();
        const int onActivate = n.handlers.value(QStringLiteral("on_activate"), -1);
        const QString activateCmd = p.value(QStringLiteral("activate"), QStringLiteral("page.open")).toString();
        PageListModel *model = lv.model;
        lw->onActivate = [this, onActivate, activateCmd, model](int r) {
            const QModelIndex idx = model->index(r);
            QVariantMap item;
            item.insert(QStringLiteral("id"), idx.data(PageListModel::IdRole).toLongLong());
            item.insert(QStringLiteral("title"), idx.data(Qt::DisplayRole).toString());
            item.insert(QStringLiteral("depth"), idx.data(PageListModel::DepthRole).toInt());
            item.insert(QStringLiteral("row"), r + 1);
            if (onActivate >= 0) {
                invokeHandler(onActivate, item, {}, false);
            } else {
                QVariantMap args;
                args.insert(QStringLiteral("id"), item.value(QStringLiteral("id")));
                const CommandResult res = m_d.commands->dispatchCommand(activateCmd, args);
                if (!res.ok)
                    showBanner(res.error, true);
            }
        };
        // New tabs: lists that open pages with a command do it with tab.open;
        // lists with an on_activate handler get item.new_tab (and
        // item.background) and decide themselves.
        if (onActivate >= 0 || activateCmd == QLatin1String("page.open")) {
            lw->onOpenInTab = [this, onActivate, model](int r, bool background) {
                const QModelIndex idx = model->index(r);
                const qint64 id = idx.data(PageListModel::IdRole).toLongLong();
                if (id <= 0)
                    return;
                if (onActivate >= 0) {
                    QVariantMap item;
                    item.insert(QStringLiteral("id"), id);
                    item.insert(QStringLiteral("title"), idx.data(Qt::DisplayRole).toString());
                    item.insert(QStringLiteral("depth"), idx.data(PageListModel::DepthRole).toInt());
                    item.insert(QStringLiteral("row"), r + 1);
                    item.insert(QStringLiteral("new_tab"), true);
                    item.insert(QStringLiteral("background"), background);
                    invokeHandler(onActivate, item, {}, false);
                    return;
                }
                QVariantMap args;
                args.insert(QStringLiteral("id"), id);
                args.insert(QStringLiteral("background"), background);
                const CommandResult res = m_d.commands->dispatchCommand(QStringLiteral("tab.open"), args);
                if (!res.ok)
                    showBanner(res.error, true);
            };
        } else {
            lw->onOpenInTab = nullptr;
        }
        lw->onCommand = [this](const QString &cmd, PageId id) {
            QVariantMap args;
            args.insert(QStringLiteral("id"), id);
            m_d.commands->dispatchCommand(cmd, args);
        };
        if (horizontal) {
            const int h = tpl.height + 2;
            lw->setFixedHeight(h);
        }
        lw->viewport()->update();
    } else if (n.type == QLatin1String("spacer")) {
        if (!w) {
            w = new QWidget(parent);
            w->setAttribute(Qt::WA_StyledBackground, true);
        }
        w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    } else if (n.type == QLatin1String("divider")) {
        auto *f = static_cast<QFrame *>(w);
        if (!f)
            w = f = new QFrame(parent);
        f->setFrameShape(QFrame::NoFrame);
        f->setAttribute(Qt::WA_StyledBackground, true);
        const bool vertical = p.value(QStringLiteral("vertical"), false).toBool();
        if (vertical)
            f->setFixedWidth(1);
        else
            f->setFixedHeight(1);
        if (!p.contains(QStringLiteral("style.bg"))) {
            const QString sheet = QStringLiteral("background-color:%1;")
                                      .arg(m_colors.value(QStringLiteral("border"), QStringLiteral("#d8d8dc")));
            if (f->styleSheet() != sheet)
                f->setStyleSheet(sheet);
        }
    }
    // Children were realized above and may have rehashed m_live: never keep
    // the reference taken at the top across that recursion.
    Live &done = m_live[n.key];
    done.w = w;
    done.type = n.type;
    if (n.type != QLatin1String("divider") || p.contains(QStringLiteral("style.bg")))
        updateCommon(w, n);
    else
        w->setObjectName(cssKey(n.key));
    if (n.type == QLatin1String("editor") || (n.type == QLatin1String("tabs") && w == m_d.tabs)) {
        if (w->parentWidget() != parent)
            w->setParent(parent);
    }
    if (auto *strip = n.type == QLatin1String("tabs") ? qobject_cast<TabStrip *>(w) : nullptr)
        strip->setLayoutVisible(visible); // with auto_hide, also needs 2+ tabs
    else
        w->setVisible(visible);
    // autofocus when the node becomes visible
    if (p.value(QStringLiteral("autofocus"), false).toBool()) {
        if (visible && !done.wasVisible)
            m_pendingFocus << (n.id.isEmpty() ? n.key : n.id);
    }
    done.wasVisible = visible;
    return w;
}

void ShellHost::applyFocusOrder(const UiNode &)
{
    QWidget *prev = nullptr;
    for (const QString &id : std::as_const(m_spec.focusOrder)) {
        QWidget *w = id == QLatin1String("editor") ? static_cast<QWidget *>(m_d.editor->editor())
                     : id == QLatin1String("title") ? static_cast<QWidget *>(m_d.editor->titleField())
                                                    : widgetById(id);
        if (!w)
            continue;
        if (prev)
            QWidget::setTabOrder(prev, w);
        prev = w;
    }
}

QVariantMap ShellHost::mergeTheme(const QVariantMap &base, const QVariantMap &overrides)
{
    QVariantMap out = base;
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it) {
        const QVariant cur = out.value(it.key());
        if (it.value().typeId() == QMetaType::QVariantMap && cur.typeId() == QMetaType::QVariantMap)
            out.insert(it.key(), mergeTheme(cur.toMap(), it.value().toMap()));
        else
            out.insert(it.key(), it.value());
    }
    return out;
}

void ShellHost::setThemeOverrides(const QVariantMap &overrides)
{
    if (overrides == m_overrides)
        return;
    m_overrides = overrides;
    if (!m_rt)
        return; // applied when a layout loads
    instr::Scope scope("theme_apply_us");
    applyTheme(mergeTheme(m_spec.theme, m_overrides));
    scheduleRender(); // named style colors ("sidebar", "panel"…) follow the new theme
}

void ShellHost::applyTheme(const QVariantMap &theme)
{
    m_theme = theme;
    const QString mode = theme.value(QStringLiteral("mode"), QStringLiteral("light")).toString();
    // Native controls, menus and dialogs follow the app's color scheme, not
    // the palette: pin it to the mode shown (light/dark), or follow the OS.
    // Otherwise a light layout on a dark Mac gets dark spin boxes, combo
    // boxes and check marks drawn on light colors (and vice versa).
    QElapsedTimer phase;
    phase.start();
    // Only when it changes: re-sending even the same scheme makes macOS
    // re-theme every native control (~17 ms measured per apply).
    {
        QStyleHints *hints = QGuiApplication::styleHints();
        const Qt::ColorScheme want = mode == QLatin1String("dark") ? Qt::ColorScheme::Dark
            : mode == QLatin1String("system")                      ? Qt::ColorScheme::Unknown
                                                                   : Qt::ColorScheme::Light;
        if (!m_schemeRequested || want != m_requestedScheme) {
            m_schemeRequested = true;
            m_requestedScheme = want;
            m_settingScheme = true;
            if (want == Qt::ColorScheme::Unknown)
                hints->unsetColorScheme();
            else
                hints->setColorScheme(want);
            m_settingScheme = false;
            if (want == Qt::ColorScheme::Unknown && hints->colorScheme() != Qt::ColorScheme::Unknown)
                m_systemScheme = hints->colorScheme(); // the OS's again, now that nothing is pinned
        }
    }
    instr::record("theme_scheme_us", phase.nsecsElapsed() / 1000);
    const bool dark = mode == QLatin1String("dark")
        || (mode == QLatin1String("system") && m_systemScheme == Qt::ColorScheme::Dark);
    EditorTheme t = dark ? EditorTheme::dark() : EditorTheme::light();
    const QVariantMap font = theme.value(QStringLiteral("font")).toMap();
    if (font.contains(QStringLiteral("family")))
        t.body.setFamily(font.value(QStringLiteral("family")).toString());
    if (font.contains(QStringLiteral("size")))
        t.body.setPointSizeF(qBound(8.0, font.value(QStringLiteral("size")).toDouble(), 40.0));
    const QVariantMap mono = theme.value(QStringLiteral("mono")).toMap();
    if (mono.contains(QStringLiteral("family")))
        t.mono.setFamily(mono.value(QStringLiteral("family")).toString());
    if (mono.contains(QStringLiteral("size")))
        t.mono.setPointSizeF(qBound(8.0, mono.value(QStringLiteral("size")).toDouble(), 40.0));
    // A code font with programming ligatures (Fira Code, JetBrains Mono):
    // shape all code text, so they form (EditorContext::codeFont).
    t.monoLigatures = mono.value(QStringLiteral("ligatures")).toBool();
    const QVariantMap ed = theme.value(QStringLiteral("editor")).toMap();
    if (ed.contains(QStringLiteral("max_width")))
        t.maxColumnWidth = qBound(0.0, ed.value(QStringLiteral("max_width")).toDouble(), 4000.0);
    if (ed.contains(QStringLiteral("line_height")))
        t.lineSpacing = qBound(1.0, ed.value(QStringLiteral("line_height")).toDouble(), 2.5);
    if (ed.contains(QStringLiteral("paragraph_spacing")))
        t.paragraphSpacing = qBound(0.0, ed.value(QStringLiteral("paragraph_spacing")).toDouble(), 48.0);
    if (ed.contains(QStringLiteral("margin")))
        t.sideMargin = qBound(4.0, ed.value(QStringLiteral("margin")).toDouble(), 400.0);
    const QVariantMap colors = theme.value(QStringLiteral("colors")).toMap();
    auto col = [&](const char *k, QColor &target) {
        const QString v = colors.value(QLatin1String(k)).toString();
        if (!v.isEmpty() && QColor::isValidColorName(v))
            target = QColor::fromString(v);
    };
    col("background", t.background);
    col("text", t.text);
    col("dim", t.dim);
    col("accent", t.accent);
    col("code_bg", t.codeBackground);
    col("inline_code_bg", t.inlineCodeBackground);
    col("math", t.mathSource);
    col("error", t.mathError);
    col("link_bg", t.linkBackground);
    col("selection", t.selection);
    // Named colors available to styles ("bg = 'sidebar'").
    m_colors.clear();
    m_colors.insert(QStringLiteral("background"), t.background.name());
    m_colors.insert(QStringLiteral("text"), t.text.name());
    m_colors.insert(QStringLiteral("dim"), t.dim.name());
    m_colors.insert(QStringLiteral("accent"), t.accent.name());
    m_colors.insert(QStringLiteral("selection"), t.selection.name());
    QColor sb = dark ? QColor(38, 38, 42) : QColor(246, 246, 248);
    m_colors.insert(QStringLiteral("sidebar"), sb.name());
    m_colors.insert(QStringLiteral("sidebar_text"), t.text.name());
    m_colors.insert(QStringLiteral("border"), (dark ? QColor(60, 60, 66) : QColor(222, 222, 226)).name());
    QColor selBg = t.accent;
    selBg.setAlpha(dark ? 90 : 46);
    m_colors.insert(QStringLiteral("selection_bg"), selBg.name(QColor::HexArgb));
    m_colors.insert(QStringLiteral("selection_text"), t.text.name());
    QColor hov = t.text;
    hov.setAlpha(14);
    m_colors.insert(QStringLiteral("hover_bg"), hov.name(QColor::HexArgb));
    m_colors.insert(QStringLiteral("panel"), (dark ? QColor(44, 44, 48) : QColor(255, 255, 255)).name());
    for (auto it = colors.cbegin(); it != colors.cend(); ++it)
        if (QColor::isValidColorName(it.value().toString()))
            m_colors.insert(it.key(), it.value().toString());
    // Native material request (optional; opaque fallback is always valid).
    const QString material = theme.value(QStringLiteral("material"), QStringLiteral("opaque")).toString();
    if (material != QLatin1String("opaque") && m_warnings.filter(QStringLiteral("theme.material")).isEmpty())
        m_warnings << QStringLiteral("theme.material = '%1' requested: no native material adapter in this build; "
                                     "using the opaque fallback").arg(material);
    phase.restart();
    m_d.editorCtx->setTheme(t);
    instr::record("theme_editor_us", phase.nsecsElapsed() / 1000);
    QPalette pal = m_d.container->palette();
    pal.setColor(QPalette::Window, t.background);
    pal.setColor(QPalette::WindowText, t.text);
    pal.setColor(QPalette::Base, t.background);
    pal.setColor(QPalette::Text, t.text);
    pal.setColor(QPalette::ButtonText, t.text);
    pal.setColor(QPalette::PlaceholderText, t.dim);
    pal.setColor(QPalette::Highlight, t.selection);
    pal.setColor(QPalette::Button, t.background);
    pal.setColor(QPalette::AlternateBase, t.codeBackground);
    pal.setColor(QPalette::HighlightedText, t.text);
    // App-wide, so every native control (title field, lists, scroll bars)
    // follows the layout's theme instead of the system appearance.
    phase.restart();
    QApplication::setPalette(pal);
    m_d.container->setPalette(pal);
    m_d.container->setAutoFillBackground(true);
    instr::record("theme_palette_us", phase.nsecsElapsed() / 1000);
}

void ShellHost::applyBindings()
{
    for (const QPointer<QShortcut> &s : m_shortcuts)
        delete s.data();
    m_shortcuts.clear();
    for (const Binding &b : m_spec.keys) {
        auto *sc = new QShortcut(b.seq, m_d.window);
        sc->setContext(Qt::WindowShortcut);
        sc->setAutoRepeat(false);
        sc->setEnabled(m_shortcutsEnabled);
        const Binding copy = b;
        connect(sc, &QShortcut::activated, this, [this, copy] { runBinding(copy); });
        connect(sc, &QShortcut::activatedAmbiguously, this, [this, copy] {
            showBanner(tr("Shortcut %1 is ambiguous").arg(copy.keyText), false);
        });
        m_shortcuts.push_back(sc);
    }
}

void ShellHost::setShortcutsEnabled(bool on)
{
    m_shortcutsEnabled = on;
    for (const QPointer<QShortcut> &s : m_shortcuts)
        if (s)
            s->setEnabled(on);
}

void ShellHost::runBinding(const Binding &b)
{
    if (b.fnRef >= 0) {
        invokeHandler(b.fnRef, {}, {}, false);
        return;
    }
    for (const QString &c : b.commands) {
        const CommandResult r = m_d.commands->dispatchCommand(c, b.commands.size() == 1 ? b.args : QVariantMap());
        if (!r.ok) {
            showBanner(r.error, true);
            break;
        }
    }
}

bool ShellHost::invokeHandler(int ref, const QVariantMap &arg, const QString &textArg, bool hasText)
{
    if (!m_rt)
        return false;
    lua_State *L = m_rt->L();
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    pushCtx();
    int nargs = 1;
    if (hasText) {
        const QByteArray u = textArg.toUtf8();
        lua_pushlstring(L, u.constData(), size_t(u.size()));
        ++nargs;
    } else if (!arg.isEmpty()) {
        pushVariant(L, arg);
        ++nargs;
    }
    QString err;
    const bool ok = m_rt->call(nargs, 0, &err);
    lua_settop(L, top);
    if (!ok)
        showBanner(tr("Shell handler error: %1").arg(err.left(600)), true);
    return ok;
}

void ShellHost::scheduleRender()
{
    if (m_rt)
        m_renderTimer.start();
}

bool ShellHost::renderNow(QString *error)
{
    if (!m_rt)
        return false;
    QElapsedTimer t;
    t.start();
    UiNode root;
    std::vector<int> refs;
    QString err;
    if (!runView(*m_rt, m_spec, root, refs, &err) || !validate(root, &err)) {
        releaseRefs(refs);
        m_lastError = err;
        showBanner(tr("Shell render error: %1\nThe last valid layout is still active.").arg(err.left(800)), true);
        if (error)
            *error = err;
        return false;
    }
    apply(root);
    releaseRefs(m_renderRefs);
    m_renderRefs = std::move(refs);
    const qint64 us = t.nsecsElapsed() / 1000;
    instr::record("shell_render_us", us);
    emit rendered(us);
    return true;
}

void ShellHost::releaseRefs(std::vector<int> &refs)
{
    if (m_rt)
        for (int r : refs)
            luaL_unref(m_rt->L(), LUA_REGISTRYINDEX, r);
    refs.clear();
}

void ShellHost::releaseSpec(LuaRuntime &, Spec &) {}

void ShellHost::setWatchFile(bool on)
{
    if (on && !m_watcher) {
        m_watcher = new QFileSystemWatcher(this);
        connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_reloadTimer.start(); });
        connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_reloadTimer.start(); });
        if (!m_path.isEmpty()) {
            m_watcher->addPath(m_path);
            m_watcher->addPath(QFileInfo(m_path).absolutePath());
        }
    } else if (!on && m_watcher) {
        delete m_watcher;
        m_watcher = nullptr;
    }
}

void ShellHost::fileChanged()
{
    if (m_path.isEmpty())
        return;
    // Editors often save by replace: re-arm the file watch.
    if (m_watcher && !m_watcher->files().contains(m_path) && QFileInfo::exists(m_path))
        m_watcher->addPath(m_path);
    m_reloadPending = true;
    maybeReloadPending();
}

void ShellHost::maybeReloadPending()
{
    if (!m_reloadPending)
        return;
    if (m_d.editor->editor()->hasPreedit())
        return; // never re-parent the editor mid-composition; retry on commit
    m_reloadPending = false;
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QByteArray code = f.readAll();
    if (code == m_loadedCode && hasLayout() && m_lastError.isEmpty())
        return; // unchanged content (editors often touch files on save)
    QString err;
    loadSource(code, m_path, &err);
}

} // namespace folia
