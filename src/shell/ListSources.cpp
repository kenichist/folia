#include "shell/ListSources.h"

#include "core/Instrument.h"
#include "shell/DataContext.h"
#include "storage/Store.h"

#include <QElapsedTimer>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>

namespace folia {

PageListModel::PageListModel(DataContext *dc, QObject *parent) : QAbstractListModel(parent), m_dc(dc) {}

int PageListModel::rowOf(PageId id) const
{
    for (int i = 0, n = rowCount(); i < n; ++i)
        if (idAt(i) == id)
            return i;
    return -1;
}

// ================================================================ tree ====
TreeSource::TreeSource(DataContext *dc, QObject *parent) : PageListModel(dc, parent)
{
    const QString saved = dc->store()->setting(QStringLiteral("tree.expanded"));
    for (const QString &s : saved.split(QLatin1Char(','), Qt::SkipEmptyParts))
        m_expanded.insert(s.toLongLong());
    connect(dc, &DataContext::treeChanged, this, &TreeSource::refresh);
    connect(dc, &DataContext::titleChanged, this, [this](PageId id, const QString &t) {
        const int r = rowOf(id);
        if (r >= 0 && m_rows[r].title != t) {
            m_rows[r].title = t;
            emit dataChanged(index(r), index(r));
        }
    });
    connect(dc, &DataContext::revealRequested, this, &TreeSource::reveal);
    reload();
}

void TreeSource::configure(const QVariantMap &opt)
{
    const PageId root = opt.value(QStringLiteral("root")).toLongLong();
    if (root != m_root) {
        m_root = root;
        reload();
    }
}

void TreeSource::reload()
{
    beginResetModel();
    m_rows.clear();
    endResetModel();
    ++m_gen[m_root];
    fetch(m_root, 0, 0);
}

int TreeSource::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant TreeSource::data(const QModelIndex &idx, int role) const
{
    if (!idx.isValid() || idx.row() >= m_rows.size())
        return {};
    const Row &r = m_rows[idx.row()];
    switch (role) {
    case Qt::DisplayRole:
    case Qt::AccessibleTextRole:
        if (r.more) {
            // Lazy paging: asking for a placeholder's text means it is visible.
            const int row = idx.row();
            QTimer::singleShot(0, const_cast<TreeSource *>(this), [this, row] {
                const_cast<TreeSource *>(this)->loadMore(row);
            });
            return tr("Loading %n more…", nullptr, r.remaining);
        }
        return r.title.isEmpty() ? tr("Untitled") : r.title;
    case IdRole: return r.more ? 0 : r.id;
    case DepthRole: return r.depth;
    case ChildCountRole: return r.childCount;
    case ExpandedRole: return r.expanded;
    case MoreRole: return r.more;
    case ParentRole: return r.parent;
    default: return {};
    }
}

int TreeSource::rowOf(PageId id) const
{
    for (int i = 0; i < m_rows.size(); ++i)
        if (!m_rows[i].more && m_rows[i].id == id)
            return i;
    return -1;
}

int TreeSource::subtreeEnd(int row) const
{
    const int depth = m_rows[row].depth;
    int e = row + 1;
    while (e < m_rows.size() && m_rows[e].depth > depth)
        ++e;
    return e;
}

void TreeSource::fetch(PageId parent, int offset, int depth)
{
    const quint64 gen = m_gen.value(parent);
    QElapsedTimer total;
    total.start();
    m_dc->store()->fetchChildren(parent, offset, pageSize(), this,
        [this, parent, offset, depth, gen, total](QVector<PageMeta> rows, int count) {
            if (m_gen.value(parent) != gen)
                return; // collapsed or refreshed meanwhile
            QElapsedTimer main;
            main.start();
            insertChildren(parent, depth, offset, rows, count);
            const qint64 mainUs = main.nsecsElapsed() / 1000;
            instr::record("tree_expand_main_us", mainUs);
            instr::record("tree_expand_total_us", total.nsecsElapsed() / 1000);
            emit firstRowsSupplied(mainUs, total.nsecsElapsed() / 1000);
        });
}

void TreeSource::insertChildren(PageId parent, int depth, int offset, const QVector<PageMeta> &rows, int total)
{
    // Locate insertion point: after the parent row's loaded children (or the
    // "more" placeholder being replaced).
    int at = 0;
    if (parent == m_root) {
        at = m_rows.size();
        for (int i = 0; i < m_rows.size(); ++i)
            if (m_rows[i].more && m_rows[i].parent == parent) {
                at = i;
                break;
            }
    } else {
        const int pr = rowOf(parent);
        if (pr < 0 || !m_rows[pr].expanded)
            return;
        at = subtreeEnd(pr);
        for (int i = pr + 1; i < at; ++i)
            if (m_rows[i].more && m_rows[i].parent == parent) {
                at = i;
                break;
            }
    }
    // Drop an existing placeholder for this parent.
    if (at < m_rows.size() && m_rows[at].more && m_rows[at].parent == parent) {
        beginRemoveRows({}, at, at);
        m_rows.remove(at);
        endRemoveRows();
    }
    QVector<Row> add;
    add.reserve(rows.size() + 1);
    for (const PageMeta &m : rows) {
        Row r;
        r.id = m.id;
        r.parent = parent;
        r.depth = depth;
        r.title = m.title;
        r.childCount = m.childCount;
        r.expanded = m.childCount > 0 && m_expanded.contains(m.id);
        add.push_back(r);
        m_dc->noteTitle(m.id, m.title);
    }
    const int loaded = offset + rows.size();
    if (loaded < total) {
        Row more;
        more.more = true;
        more.parent = parent;
        more.depth = depth;
        more.nextOffset = loaded;
        more.remaining = total - loaded;
        add.push_back(more);
    }
    if (add.isEmpty())
        return;
    beginInsertRows({}, at, at + add.size() - 1);
    for (int i = 0; i < add.size(); ++i)
        m_rows.insert(at + i, add[i]);
    endInsertRows();
    // Re-expand remembered branches (async, one page each).
    for (const Row &r : std::as_const(add))
        if (!r.more && r.expanded) {
            ++m_gen[r.id];
            fetch(r.id, 0, r.depth + 1);
        }
    if (m_revealTarget) {
        const int tr = rowOf(m_revealTarget);
        if (tr >= 0) {
            m_revealTarget = 0;
            emit revealed(tr);
        } else if (!m_revealChain.isEmpty()) {
            const int nr = rowOf(m_revealChain.first());
            if (nr >= 0) {
                m_revealChain.removeFirst();
                if (!m_rows[nr].expanded)
                    expand(nr);
            }
        }
    }
}

void TreeSource::expand(int row)
{
    if (row < 0 || row >= m_rows.size())
        return;
    Row &r = m_rows[row];
    if (r.more || r.childCount == 0 || r.expanded)
        return;
    instr::Scope scope("tree_expand_request_us");
    r.expanded = true;
    m_expanded.insert(r.id);
    emit dataChanged(index(row), index(row));
    ++m_gen[r.id];
    fetch(r.id, 0, r.depth + 1);
    persistExpanded();
}

void TreeSource::collapse(int row)
{
    if (row < 0 || row >= m_rows.size())
        return;
    Row &r = m_rows[row];
    if (!r.expanded)
        return;
    r.expanded = false;
    m_expanded.remove(r.id);
    ++m_gen[r.id];
    const int e = subtreeEnd(row);
    if (e > row + 1) {
        beginRemoveRows({}, row + 1, e - 1);
        m_rows.remove(row + 1, e - row - 1);
        endRemoveRows();
    }
    emit dataChanged(index(row), index(row));
    persistExpanded();
}

void TreeSource::loadMore(int row)
{
    if (row < 0 || row >= m_rows.size() || !m_rows[row].more || m_rows[row].nextOffset < 0)
        return;
    Row &r = m_rows[row];
    const int off = r.nextOffset;
    r.nextOffset = -1; // in flight
    fetch(r.parent, off, r.depth);
}

void TreeSource::refresh(PageId parent)
{
    int pr = -1;
    if (parent != m_root) {
        pr = rowOf(parent);
        if (pr >= 0) {
            // child count may have changed; re-query this row's metadata
            m_dc->store()->pageMeta(parent, this, [this, parent](PageMeta m, bool found) {
                const int r = rowOf(parent);
                if (!found || r < 0)
                    return;
                const bool had = m_rows[r].childCount > 0;
                m_rows[r].childCount = m.childCount;
                m_rows[r].title = m.title;
                if (!had && m.childCount > 0 && m_expanded.contains(parent))
                    m_rows[r].expanded = true;
                emit dataChanged(index(r), index(r));
            });
        }
        if (pr < 0 || !m_rows[pr].expanded)
            return;
    }
    // Replace the parent's loaded children.
    int from = parent == m_root ? 0 : pr + 1;
    int to = parent == m_root ? m_rows.size() : subtreeEnd(pr);
    const int depth = parent == m_root ? 0 : m_rows[pr].depth + 1;
    if (to > from) {
        beginRemoveRows({}, from, to - 1);
        m_rows.remove(from, to - from);
        endRemoveRows();
    }
    ++m_gen[parent];
    fetch(parent, 0, depth);
}

void TreeSource::reveal(PageId id)
{
    if (rowOf(id) >= 0) {
        emit revealed(rowOf(id));
        return;
    }
    // Expand the ancestor chain (root..parent), one async page at a time.
    m_dc->store()->ancestors(id, this, [this, id](QVector<PageMeta> chain) {
        m_revealChain.clear();
        for (const PageMeta &m : chain)
            if (m.id != id)
                m_revealChain.push_back(m.id);
        m_revealTarget = id;
        while (!m_revealChain.isEmpty()) {
            const int r = rowOf(m_revealChain.first());
            if (r < 0)
                break;
            m_revealChain.removeFirst();
            if (!m_rows[r].expanded) {
                expand(r);
                return;
            }
        }
        const int tr = rowOf(id);
        if (tr >= 0) {
            m_revealTarget = 0;
            emit revealed(tr);
        }
    });
}

void TreeSource::persistExpanded()
{
    QStringList ids;
    ids.reserve(m_expanded.size());
    for (PageId id : std::as_const(m_expanded))
        ids << QString::number(id);
    if (ids.size() > 2000)
        ids = ids.mid(0, 2000);
    m_dc->store()->setSetting(QStringLiteral("tree.expanded"), ids.join(QLatin1Char(',')));
}

// ================================================================ flat ====
FlatSource::FlatSource(Kind k, DataContext *dc, QObject *parent) : PageListModel(dc, parent), m_kind(k)
{
    connect(dc, &DataContext::titleChanged, this, [this](PageId id, const QString &t) {
        for (int i = 0; i < m_rows.size(); ++i)
            if (m_rows[i].id == id && m_rows[i].title != t) {
                m_rows[i].title = t;
                emit dataChanged(index(i), index(i));
            }
    });
    if (k == Kind::Ancestors) {
        connect(dc, &DataContext::ancestorsChanged, this, &FlatSource::refresh);
    } else if (k == Kind::Children) {
        connect(dc, &DataContext::currentPageChanged, this, &FlatSource::refresh);
        connect(dc, &DataContext::treeChanged, this, [this](PageId p) {
            if (p == m_dc->currentPage())
                refresh();
        });
    } else if (k == Kind::Recent) {
        connect(dc, &DataContext::currentPageChanged, this, &FlatSource::refresh);
    }
    refresh();
}

QString FlatSource::kind() const
{
    switch (m_kind) {
    case Kind::Search: return QStringLiteral("search");
    case Kind::Recent: return QStringLiteral("recent");
    case Kind::Children: return QStringLiteral("children");
    case Kind::Ancestors: return QStringLiteral("ancestors");
    }
    return {};
}

void FlatSource::configure(const QVariantMap &opt)
{
    if (opt.contains(QStringLiteral("limit")))
        m_limit = qBound(1, opt.value(QStringLiteral("limit")).toInt(), 500);
    if (m_kind == Kind::Search && opt.contains(QStringLiteral("query")))
        setQuery(opt.value(QStringLiteral("query")).toString());
}

void FlatSource::setQuery(const QString &q)
{
    if (q == m_query && !m_rows.isEmpty())
        return;
    m_query = q;
    refresh();
}

void FlatSource::refresh()
{
    const quint64 gen = ++m_gen;
    Store *s = m_dc->store();
    auto done = [this, gen](QVector<PageMeta> rows) {
        if (gen == m_gen)
            setRows(std::move(rows), -1);
    };
    switch (m_kind) {
    case Kind::Search:
        if (m_query.trimmed().isEmpty())
            s->recentPages(m_limit, this, done);
        else {
            QElapsedTimer t;
            t.start();
            s->searchTitles(m_query.trimmed(), m_limit, this, [this, gen, t](QVector<PageMeta> rows) {
                instr::record("title_search_us", t.nsecsElapsed() / 1000);
                if (gen == m_gen)
                    setRows(std::move(rows), -1);
            });
        }
        break;
    case Kind::Recent:
        s->recentPages(m_limit, this, done);
        break;
    case Kind::Children:
        if (!m_dc->currentPage()) {
            setRows({}, 0);
            break;
        }
        s->fetchChildren(m_dc->currentPage(), 0, m_limit, this, [this, gen](QVector<PageMeta> rows, int total) {
            if (gen == m_gen)
                setRows(std::move(rows), total);
        });
        break;
    case Kind::Ancestors:
        setRows(m_dc->ancestors(), -1);
        break;
    }
}

void FlatSource::setRows(QVector<PageMeta> rows, int total)
{
    beginResetModel();
    m_rows = std::move(rows);
    m_total = total < 0 ? m_rows.size() : total;
    for (const PageMeta &m : std::as_const(m_rows))
        m_dc->noteTitle(m.id, m.title);
    endResetModel();
    emit reloaded();
}

int FlatSource::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant FlatSource::data(const QModelIndex &idx, int role) const
{
    if (!idx.isValid() || idx.row() >= m_rows.size())
        return {};
    const PageMeta &m = m_rows[idx.row()];
    switch (role) {
    case Qt::DisplayRole:
    case Qt::AccessibleTextRole:
        return m.title.isEmpty() ? tr("Untitled") : m.title;
    case IdRole: return m.id;
    case DepthRole: return 0;
    case ChildCountRole: return m.childCount;
    case ExpandedRole: return false;
    case MoreRole: return false;
    case ParentRole: return m.parent;
    default: return {};
    }
}

void FlatSource::loadMore(int) {}

// ============================================================ delegate ====
QRect PageRowDelegate::disclosureRect(const QRect &row, int depth) const
{
    const int x = row.left() + 6 + (tpl.indent ? depth * 16 : 0);
    return QRect(x, row.top(), 16, row.height());
}

QSize PageRowDelegate::sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &idx) const
{
    if (!tpl.horizontal)
        return QSize(opt.rect.width(), tpl.height);
    const QFontMetrics fm(tpl.font);
    const QString text = idx.data(Qt::DisplayRole).toString();
    const bool last = idx.row() == idx.model()->rowCount() - 1;
    const int sep = last ? 0 : fm.horizontalAdvance(tpl.separator);
    return QSize(qMin(260, fm.horizontalAdvance(text)) + 20 + sep, tpl.height);
}

void PageRowDelegate::paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const
{
    p->save();
    const bool selected = opt.state & QStyle::State_Selected;
    const bool hover = opt.state & QStyle::State_MouseOver;
    const QRect r = opt.rect;
    if (selected) {
        p->setRenderHint(QPainter::Antialiasing, true);
        p->setPen(Qt::NoPen);
        p->setBrush(tpl.selectedBg);
        p->drawRoundedRect(QRectF(r).adjusted(4, 1, -4, -1), 5, 5);
    } else if (hover && tpl.hoverBg.isValid()) {
        p->setRenderHint(QPainter::Antialiasing, true);
        p->setPen(Qt::NoPen);
        p->setBrush(tpl.hoverBg);
        p->drawRoundedRect(QRectF(r).adjusted(4, 1, -4, -1), 5, 5);
    }
    const int depth = idx.data(PageListModel::DepthRole).toInt();
    const bool more = idx.data(PageListModel::MoreRole).toBool();
    const int kids = idx.data(PageListModel::ChildCountRole).toInt();
    const bool expanded = idx.data(PageListModel::ExpandedRole).toBool();
    QColor fg = selected ? tpl.selectedText : tpl.text;
    int x = r.left() + 8;
    if (!tpl.horizontal) {
        x = r.left() + 6 + (tpl.indent ? depth * 16 : 0);
        if (tpl.disclosure) {
            if (kids > 0 && !more) {
                p->setRenderHint(QPainter::Antialiasing, true);
                QPainterPath tri;
                const QPointF c(x + 7, r.center().y() + 0.5);
                if (expanded) {
                    tri.moveTo(c + QPointF(-4, -2));
                    tri.lineTo(c + QPointF(4, -2));
                    tri.lineTo(c + QPointF(0, 3));
                } else {
                    tri.moveTo(c + QPointF(-2, -4));
                    tri.lineTo(c + QPointF(3, 0));
                    tri.lineTo(c + QPointF(-2, 4));
                }
                tri.closeSubpath();
                p->setPen(Qt::NoPen);
                p->setBrush(selected ? tpl.selectedText : tpl.dim);
                p->drawPath(tri);
            }
            x += 18;
        }
        if (tpl.icon && !more) {
            p->setRenderHint(QPainter::Antialiasing, true);
            p->setPen(QPen(selected ? tpl.selectedText : tpl.dim, 1.1));
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(QRectF(x, r.center().y() - 6, 9, 12), 1.5, 1.5);
            x += 16;
        }
    }
    QFont f = tpl.font;
    if (more)
        f.setItalic(true);
    p->setFont(f);
    p->setPen(more ? tpl.dim : fg);
    const QFontMetrics fm(f);
    QString text = idx.data(Qt::DisplayRole).toString();
    int right = r.right() - 8;
    if (tpl.detailCount && kids > 0 && !tpl.horizontal) {
        const QString d = QString::number(kids);
        p->setPen(tpl.dim);
        p->drawText(QRect(r.right() - 40, r.top(), 32, r.height()), Qt::AlignVCenter | Qt::AlignRight, d);
        p->setPen(more ? tpl.dim : fg);
        right -= 40;
    }
    if (tpl.horizontal) {
        const bool last = idx.row() == idx.model()->rowCount() - 1;
        const int sepW = last ? 0 : fm.horizontalAdvance(tpl.separator);
        const QRect tr(r.left() + 6, r.top(), r.width() - 12 - sepW, r.height());
        p->setPen(last ? tpl.text : tpl.dim);
        p->drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(text, Qt::ElideRight, tr.width()));
        if (!last) {
            p->setPen(tpl.dim);
            p->drawText(QRect(tr.right(), r.top(), sepW + 6, r.height()), Qt::AlignVCenter | Qt::AlignLeft, tpl.separator);
        }
    } else {
        const QRect tr(x, r.top(), qMax(10, right - x), r.height());
        p->drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(text, Qt::ElideRight, tr.width()));
    }
    p->restore();
}

} // namespace folia
