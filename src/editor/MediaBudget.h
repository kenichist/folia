// One memory budget for all media work (docs/ATTACHMENTS.md §2):
//
//     media bytes = cached pixels + reserved in-flight working sets
//
// A job reserves its estimated working set before it starts and releases it
// when it finishes (the result is then accounted as cached bytes). A job
// waits while it does not fit; one whose estimate exceeds the whole (base)
// budget is refused. Memory pressure lowers the effective budget for a
// cool-down period so caches do not grow straight back. Main thread only.
#pragma once

#include <QtGlobal>

namespace folia {

class MediaBudget {
public:
    enum class Admit { Run, Wait, TooLarge };

    explicit MediaBudget(qint64 bytes) : m_base(bytes), m_limit(bytes) { publish(); }

    qint64 base() const { return m_base; }
    qint64 limit() const { return m_limit; } // lowered during a cool-down
    qint64 reserved() const { return m_reserved; }
    qint64 cached() const { return m_cached; }
    qint64 used() const { return m_reserved + m_cached; }
    bool coolingDown() const { return m_limit < m_base; }

    void setBase(qint64 bytes);
    // Effective budget = base × factor until restore().
    void lower(double factor);
    void restore();

    // `evictable`: cached bytes the caller could free to make room.
    Admit check(qint64 estimate, qint64 evictable) const;
    // Cached bytes that must go before `estimate` fits (0: fits now).
    qint64 overBy(qint64 estimate) const { return qMax<qint64>(0, used() + estimate - m_limit); }

    void reserve(qint64 bytes);
    void release(qint64 bytes);
    void addCached(qint64 bytes);
    void removeCached(qint64 bytes);

private:
    void publish() const;
    qint64 m_base;
    qint64 m_limit;
    qint64 m_reserved = 0;
    qint64 m_cached = 0;
};

} // namespace folia
