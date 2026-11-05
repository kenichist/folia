// Read-only page data exposed to the shell (natively; never per-frame Lua):
// current page, its ancestors, a bounded title cache and change signals.
#pragma once

#include "core/Types.h"

#include <QHash>
#include <QObject>

namespace folia {

class Store;

class DataContext : public QObject {
    Q_OBJECT
public:
    explicit DataContext(Store *store, QObject *parent = nullptr) : QObject(parent), m_store(store) {}

    Store *store() const { return m_store; }
    PageId currentPage() const { return m_current; }
    const QVector<PageMeta> &ancestors() const { return m_ancestors; } // root .. current

    QString titleOf(PageId id) const { return m_titles.value(id); }
    void noteTitle(PageId id, const QString &title)
    {
        if (m_titles.size() > 20000)
            m_titles.clear(); // bounded cache; refilled lazily from queries
        auto it = m_titles.find(id);
        if (it != m_titles.end() && *it == title)
            return;
        m_titles.insert(id, title);
        emit titleChanged(id, title);
    }

    void setCurrent(PageId id, const QVector<PageMeta> &ancestors)
    {
        const bool changed = id != m_current;
        m_current = id;
        m_ancestors = ancestors;
        for (const PageMeta &m : ancestors)
            m_titles.insert(m.id, m.title);
        if (changed)
            emit currentPageChanged(id);
        emit ancestorsChanged();
    }

    void notifyTreeChanged(PageId parent) { emit treeChanged(parent); }
    void notifyStatus(const QString &status)
    {
        if (status == m_status)
            return;
        m_status = status;
        emit statusChanged(status);
    }
    QString status() const { return m_status; }

signals:
    void currentPageChanged(folia::PageId id);
    void ancestorsChanged();
    void treeChanged(folia::PageId parent);
    void titleChanged(folia::PageId id, const QString &title);
    void statusChanged(const QString &status);
    void revealRequested(folia::PageId id);

private:
    Store *m_store;
    PageId m_current = 0;
    QVector<PageMeta> m_ancestors;
    QHash<PageId, QString> m_titles;
    QString m_status;
};

} // namespace folia
