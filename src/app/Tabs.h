// Note tabs (docs/ARCHITECTURE.md §5, "Tabs"): an ordered list of tabs,
// each a page id with its own back/forward history. Plain data: no widgets,
// no documents. A tab that is not shown costs its ids and nothing else;
// showing it opens its page through AppController's document cache, like
// any other navigation (background tabs load only when shown, as Obsidian's
// deferred views do). Caret and scroll come with the page (its stored
// selection), so a tab needs no view state of its own.
#pragma once

#include "core/Types.h"

#include <QSet>
#include <QString>
#include <QVector>

namespace folia {

struct NoteTab {
    quint32 uid = 0;  // stable identity across moves (the strip's hover/drag)
    PageId page = 0;  // 0 only while a new tab's page is being created
    QVector<PageId> back, forward;
};

class TabList {
public:
    static constexpr int kMaxTabs = 100;
    static constexpr int kMaxHistory = 50;
    static constexpr int kMaxClosed = 20;

    TabList();

    int count() const { return int(m_tabs.size()); }
    int activeIndex() const { return m_active; }
    const NoteTab &at(int i) const { return m_tabs[i]; }
    const NoteTab &current() const { return m_tabs[m_active]; }
    NoteTab &current() { return m_tabs[m_active]; }
    int indexOfUid(quint32 uid) const;
    bool showsPage(PageId id, int exceptIndex = -1) const;

    // Replaces everything with one tab per page (0s and duplicates kept as
    // given, capped at kMaxTabs) and `active` clamped; no closed history.
    void reset(const QVector<PageId> &pages, int active);
    // A tab showing `page` at `at` (-1 or past the end: appended). Returns
    // its index, or -1 when kMaxTabs are open. Does not change the active tab.
    int insert(int at, PageId page);
    void setActive(int i);
    // Closes tab i; never the last one (returns false). When the active tab
    // closes, its right neighbour becomes active (the left one at the end).
    bool close(int i);
    // Reopens the most recently closed tab at its old position, active.
    // Returns its index, or -1 when there is none (or no room).
    int reopenClosed();
    bool canReopen() const { return !m_closed.isEmpty(); }
    void move(int from, int to);

    // The current tab leaves `from` for another page: `from` goes on its back
    // stack (capped), and its forward stack is cleared.
    void pushHistory(PageId from);
    // Pops the current tab's back (forward) stack, pushing its page onto the
    // other one. Returns the page to show, or 0 when the stack is empty.
    PageId back();
    PageId forward();

    // Pages that no longer exist (trashed): tabs showing them close, except
    // the active one (the caller moves it elsewhere); they leave every
    // history and the closed-tab stack. Returns true when a tab closed.
    bool dropPages(const QSet<PageId> &gone);
    // Every page a tab shows or can go back/forward to, and closed tabs.
    QVector<PageId> referencedPages() const;

    // One settings value: "a=<active>;p=<id>,<id>,…" (shown pages only).
    QString serialize() const;
    // Parses serialize()'s output; junk ids are skipped. False when nothing
    // usable was found.
    static bool parse(const QString &s, QVector<PageId> *pages, int *active);

private:
    struct Closed {
        NoteTab tab;
        int index = 0;
    };
    NoteTab make(PageId page);
    static void capPush(QVector<PageId> &v, PageId id);

    QVector<NoteTab> m_tabs;
    QVector<Closed> m_closed;
    int m_active = 0;
    quint32 m_nextUid = 1;
};

} // namespace folia
