#include "editor/MediaBudget.h"

#include "core/Instrument.h"

namespace folia {

void MediaBudget::setBase(qint64 bytes)
{
    const double factor = m_base > 0 ? double(m_limit) / double(m_base) : 1.0;
    m_base = qMax<qint64>(0, bytes);
    m_limit = qint64(m_base * factor);
    publish();
}

void MediaBudget::lower(double factor)
{
    m_limit = qMin(m_limit, qint64(m_base * qBound(0.0, factor, 1.0)));
    publish();
}

void MediaBudget::restore()
{
    m_limit = m_base;
    publish();
}

MediaBudget::Admit MediaBudget::check(qint64 estimate, qint64 evictable) const
{
    if (estimate > m_base)
        return Admit::TooLarge; // could never run, even with every cache empty
    if (m_reserved + estimate > m_limit)
        return Admit::Wait; // in-flight work cannot be evicted: wait for it
    if (used() - qMin(evictable, m_cached) + estimate > m_limit)
        return Admit::Wait;
    return Admit::Run;
}

void MediaBudget::reserve(qint64 bytes)
{
    m_reserved += bytes;
    publish();
}

void MediaBudget::release(qint64 bytes)
{
    m_reserved = qMax<qint64>(0, m_reserved - bytes);
    publish();
}

void MediaBudget::addCached(qint64 bytes)
{
    m_cached += bytes;
    publish();
}

void MediaBudget::removeCached(qint64 bytes)
{
    m_cached = qMax<qint64>(0, m_cached - bytes);
    publish();
}

void MediaBudget::publish() const
{
    instr::set(instr::MediaBudgetBytes, m_limit);
    instr::set(instr::MediaReservedBytes, m_reserved);
    instr::set(instr::MediaCacheBytes, m_cached);
}

} // namespace folia
