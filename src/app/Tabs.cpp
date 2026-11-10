#include "app/Tabs.h"

#include <QStringList>

namespace folia {

TabList::TabList()
{
    m_tabs.push_back(make(0));
}

NoteTab TabList::make(PageId page)
{
    NoteTab t;
    t.uid = m_nextUid++;
    t.page = page;
    return t;
}

void TabList::capPush(QVector<PageId> &v, PageId id)
{
    if (id <= 0 || (!v.isEmpty() && v.last() == id))
        return;
    v.push_back(id);
    if (v.size() > kMaxHistory)
        v.removeFirst();
}

int TabList::indexOfUid(quint32 uid) const
{
    for (int i = 0; i < m_tabs.size(); ++i)
        if (m_tabs[i].uid == uid)
            return i;
    return -1;
}

bool TabList::showsPage(PageId id, int exceptIndex) const
{
    for (int i = 0; i < m_tabs.size(); ++i)
        if (i != exceptIndex && m_tabs[i].page == id)
            return true;
    return false;
}

void TabList::reset(const QVector<PageId> &pages, int active)
{
    m_tabs.clear();
    m_closed.clear();
    for (PageId p : pages) {
        if (m_tabs.size() >= kMaxTabs)
            break;
        m_tabs.push_back(make(p));
    }
    if (m_tabs.isEmpty())
        m_tabs.push_back(make(0));
    m_active = qBound(0, active, int(m_tabs.size()) - 1);
}

int TabList::insert(int at, PageId page)
{
    if (m_tabs.size() >= kMaxTabs)
        return -1;
    if (at < 0 || at > m_tabs.size())
        at = int(m_tabs.size());
    m_tabs.insert(at, make(page));
    if (at <= m_active && m_tabs.size() > 1)
        ++m_active;
    return at;
}

void TabList::setActive(int i)
{
    if (i >= 0 && i < m_tabs.size())
        m_active = i;
}

bool TabList::close(int i)
{
    if (m_tabs.size() <= 1 || i < 0 || i >= m_tabs.size())
        return false;
    if (m_tabs[i].page > 0) {
        m_closed.push_back({m_tabs[i], i});
        if (m_closed.size() > kMaxClosed)
            m_closed.removeFirst();
    }
    m_tabs.removeAt(i);
    if (i < m_active)
        --m_active;
    else if (i == m_active)
        m_active = qMin(i, int(m_tabs.size()) - 1); // the right neighbour, or the left one at the end
    return true;
}

int TabList::reopenClosed()
{
    if (m_closed.isEmpty() || m_tabs.size() >= kMaxTabs)
        return -1;
    Closed c = m_closed.takeLast();
    const int at = qBound(0, c.index, int(m_tabs.size()));
    c.tab.uid = m_nextUid++; // a new tab as far as the strip is concerned
    m_tabs.insert(at, c.tab);
    m_active = at;
    return at;
}

void TabList::move(int from, int to)
{
    if (from < 0 || from >= m_tabs.size() || to < 0 || to >= m_tabs.size() || from == to)
        return;
    const quint32 activeUid = m_tabs[m_active].uid;
    m_tabs.move(from, to);
    m_active = indexOfUid(activeUid);
}

void TabList::pushHistory(PageId from)
{
    NoteTab &t = current();
    capPush(t.back, from);
    t.forward.clear();
}

PageId TabList::back()
{
    NoteTab &t = current();
    if (t.back.isEmpty())
        return 0;
    const PageId id = t.back.takeLast();
    capPush(t.forward, t.page);
    return id;
}

PageId TabList::forward()
{
    NoteTab &t = current();
    if (t.forward.isEmpty())
        return 0;
    const PageId id = t.forward.takeLast();
    capPush(t.back, t.page);
    return id;
}

bool TabList::dropPages(const QSet<PageId> &gone)
{
    if (gone.isEmpty())
        return false;
    auto prune = [&gone](QVector<PageId> &v) {
        v.erase(std::remove_if(v.begin(), v.end(), [&gone](PageId p) { return gone.contains(p); }), v.end());
        // Adjacent duplicates left by the removal would make Back a no-op.
        v.erase(std::unique(v.begin(), v.end()), v.end());
    };
    bool closed = false;
    for (int i = int(m_tabs.size()) - 1; i >= 0; --i) {
        if (i != m_active && gone.contains(m_tabs[i].page)) {
            m_tabs.removeAt(i);
            if (i < m_active)
                --m_active;
            closed = true;
        }
    }
    for (NoteTab &t : m_tabs) {
        prune(t.back);
        prune(t.forward);
    }
    for (int i = int(m_closed.size()) - 1; i >= 0; --i) {
        if (gone.contains(m_closed[i].tab.page)) {
            m_closed.removeAt(i);
            continue;
        }
        prune(m_closed[i].tab.back);
        prune(m_closed[i].tab.forward);
    }
    return closed;
}

QVector<PageId> TabList::referencedPages() const
{
    QSet<PageId> seen;
    QVector<PageId> out;
    auto add = [&](PageId p) {
        if (p > 0 && !seen.contains(p)) {
            seen.insert(p);
            out.push_back(p);
        }
    };
    auto addTab = [&](const NoteTab &t) {
        add(t.page);
        for (PageId p : t.back)
            add(p);
        for (PageId p : t.forward)
            add(p);
    };
    for (const NoteTab &t : m_tabs)
        addTab(t);
    for (const Closed &c : m_closed)
        addTab(c.tab);
    return out;
}

QString TabList::serialize() const
{
    QStringList ids;
    int active = 0;
    for (int i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs[i].page <= 0)
            continue;
        if (i == m_active)
            active = int(ids.size());
        ids << QString::number(m_tabs[i].page);
    }
    return QStringLiteral("a=%1;p=%2").arg(active).arg(ids.join(QLatin1Char(',')));
}

bool TabList::parse(const QString &s, QVector<PageId> *pages, int *active)
{
    QVector<PageId> out;
    int a = 0;
    for (const QString &part : s.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        if (part.startsWith(QLatin1String("a="))) {
            bool ok = false;
            const int v = part.mid(2).toInt(&ok);
            if (ok)
                a = v;
        } else if (part.startsWith(QLatin1String("p="))) {
            for (const QString &id : part.mid(2).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                bool ok = false;
                const PageId p = id.trimmed().toLongLong(&ok);
                if (ok && p > 0 && out.size() < kMaxTabs)
                    out.push_back(p);
            }
        }
    }
    if (out.isEmpty())
        return false;
    if (pages)
        *pages = out;
    if (active)
        *active = qBound(0, a, int(out.size()) - 1);
    return true;
}

} // namespace folia
