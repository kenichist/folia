// Native, paged, read-only page data sources for the shell's virtualized
// list primitive. Rows are fetched from the storage reader thread in pages;
// nothing loads all descendants and no source ever calls Lua.
#pragma once

#include "core/Types.h"

#include <QAbstractListModel>
#include <QHash>
#include <QSet>
#include <QStyledItemDelegate>
#include <QVariantMap>
#include <QVector>

namespace folia {

class DataContext;

class PageListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { IdRole = Qt::UserRole + 1, DepthRole, ChildCountRole, ExpandedRole, MoreRole, ParentRole };

    explicit PageListModel(DataContext *dc, QObject *parent = nullptr);
    virtual QString kind() const = 0;
    virtual void configure(const QVariantMap &) {}
    virtual bool isTree() const { return false; }
    virtual void expand(int) {}
    virtual void collapse(int) {}
    virtual void loadMore(int) {}
    virtual int rowOf(PageId id) const;
    PageId idAt(int row) const { return data(index(row), IdRole).toLongLong(); }

signals:
    void revealed(int row);
    void reloaded();

protected:
    DataContext *m_dc;
};

// Flattened, lazily expanded page outline.
class TreeSource : public PageListModel {
    Q_OBJECT
public:
    explicit TreeSource(DataContext *dc, QObject *parent = nullptr);
    QString kind() const override { return QStringLiteral("tree"); }
    bool isTree() const override { return true; }
    void configure(const QVariantMap &opt) override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &idx, int role) const override;
    void expand(int row) override;
    void collapse(int row) override;
    void loadMore(int row) override;
    int rowOf(PageId id) const override;
    void reveal(PageId id);
    void reload();
    QSet<PageId> expandedSet() const { return m_expanded; }

    static int pageSize() { return 200; }

signals:
    void expandedChanged();
    void firstRowsSupplied(qint64 mainThreadMicros, qint64 totalMicros);

private:
    struct Row {
        PageId id = 0;
        PageId parent = 0;
        int depth = 0;
        QString title;
        int childCount = 0;
        bool expanded = false;
        bool more = false;   // "show more" placeholder for `parent`
        int nextOffset = 0;
        int remaining = 0;
    };
    void fetch(PageId parent, int offset, int depth);
    void insertChildren(PageId parent, int depth, int offset, const QVector<PageMeta> &rows, int total);
    int subtreeEnd(int row) const;
    void refresh(PageId parent);
    void persistExpanded();

    QVector<Row> m_rows;
    QSet<PageId> m_expanded;
    QHash<PageId, quint64> m_gen;
    QVector<PageId> m_revealChain;
    PageId m_revealTarget = 0;
    PageId m_root = 0;
    bool m_rootOnly = false;
};

// Flat list sources.
class FlatSource : public PageListModel {
    Q_OBJECT
public:
    enum class Kind { Search, Recent, Children, Ancestors };
    FlatSource(Kind k, DataContext *dc, QObject *parent = nullptr);
    QString kind() const override;
    void configure(const QVariantMap &opt) override;
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &idx, int role) const override;
    void loadMore(int row) override;
    void setQuery(const QString &q);
    QString query() const { return m_query; }
    void refresh();

private:
    void setRows(QVector<PageMeta> rows, int total);
    Kind m_kind;
    QVector<PageMeta> m_rows;
    int m_total = 0;
    int m_limit = 50;
    QString m_query;
    quint64 m_gen = 0;
};

// Paints rows from a declarative template (no per-row Lua).
class PageRowDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    // A button shown on the hovered row only (e.g. "add a page inside",
    // "delete"), painted here and hit-tested by the list: no widgets per row.
    struct Action {
        QString icon;        // Lucide name
        QString command;
        QString idArg;       // argument that receives the row's page id
        QVariantMap args;    // fixed extra arguments
        QString tooltip;
    };
    struct Template {
        int height = 26;
        qreal radius = 5;   // selection / hover pill corners
        bool indent = true;
        bool disclosure = true;
        bool icon = false;
        bool detailCount = false;
        bool horizontal = false;
        QString separator;
        QColor text, dim, selectedBg, selectedText, hoverBg;
        QFont font;
        QVector<Action> actions;
    };
    explicit PageRowDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}
    Template tpl;
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override;
    QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &idx) const override;
    // Hit area of the disclosure triangle for a row rect.
    QRect disclosureRect(const QRect &row, int depth) const;
    // Hit area of hover action `i` for a row rect.
    QRect actionRect(const QRect &row, int i) const;
    // Row under the mouse and the action under it (-1: none); set by the list.
    int hoverRow = -1;
    int hoverAction = -1;
};

} // namespace folia
