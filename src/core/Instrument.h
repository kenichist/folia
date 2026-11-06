// Performance-and-QA instrumentation. Counters are always on (one relaxed
// atomic add); latency samples are collected only when enabled (bench mode),
// so release behaviour is unchanged when nobody is measuring.
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QString>
#include <QVector>
#include <QtGlobal>

namespace folia::instr {

enum Counter : int {
    BlocksLaidOut,        // QTextLayout passes run by the editor layout
    BlockHeightEstimates, // blocks given an estimated (not laid out) height
    BytesCopiedForSave,   // UTF-16 bytes extracted from the editor for saves
    SaveSnapshots,
    SaveSnapshotFullWalks,
    PreviewRequests,
    PreviewRenders,
    PreviewCacheHits,
    PreviewStaleDrops,    // results discarded because a newer revision exists
    PreviewCancelled,
    PreviewOutstanding,   // gauge
    PreviewCacheBytes,    // gauge
    DocCacheBytes,        // gauge (estimated)
    DocCacheEntries,      // gauge
    HighlightLines,
    HighlightStateSteps,
    LuaInvocations,
    LuaErrors,
    LuaRenders,
    WidgetsCreated,
    WidgetsReused,
    PaintEvents,
    // Media (docs/ATTACHMENTS.md §6). Gauges are set, counters only grow.
    MediaBudgetBytes,      // gauge: current budget (lowered during a pressure cool-down)
    MediaReservedBytes,    // gauge: reserved for jobs in flight
    MediaCacheBytes,       // gauge: decoded pixels held by the cache
    ImageDecodes,
    ImageCacheHits,
    PdfRenders,
    PdfTiles,
    MediaJobsDroppedQueued, // removed from the queue before they started
    MediaJobsDiscarded,     // finished, but nobody wanted the result any more
    MediaCpuUsDiscarded,    // worker CPU time spent on those
    MediaBudgetWaits,       // jobs that had to wait for budget
    MediaTooLarge,          // jobs refused: estimate exceeds the whole budget
    AttachmentImports,
    AttachmentGcDeleted,
    CounterCount
};

const char *counterName(Counter c);
void add(Counter c, qint64 v = 1);
void set(Counter c, qint64 v);
qint64 get(Counter c);

bool samplingEnabled();
void setSamplingEnabled(bool on);
void record(const char *metric, qint64 micros);
QHash<QString, QVector<qint64>> takeSamples();

// RAII main-thread timer: records elapsed µs under `metric` when sampling.
class Scope {
public:
    explicit Scope(const char *metric) : m_metric(metric)
    {
        if (samplingEnabled())
            m_t.start();
    }
    ~Scope()
    {
        if (m_t.isValid())
            record(m_metric, m_t.nsecsElapsed() / 1000);
    }

private:
    const char *m_metric;
    QElapsedTimer m_t;
};

} // namespace folia::instr
