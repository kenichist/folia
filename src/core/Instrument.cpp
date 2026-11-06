#include "core/Instrument.h"

#include <atomic>
#include <mutex>

namespace folia::instr {
namespace {
std::atomic<qint64> g_counters[CounterCount];
std::atomic<bool> g_sampling{false};
std::mutex g_mu;
QHash<QString, QVector<qint64>> g_samples;
} // namespace

const char *counterName(Counter c)
{
    static const char *names[CounterCount] = {
        "blocks_laid_out", "block_height_estimates", "bytes_copied_for_save", "save_snapshots",
        "save_snapshot_full_walks", "preview_requests", "preview_renders", "preview_cache_hits",
        "preview_stale_drops", "preview_cancelled", "preview_outstanding", "preview_cache_bytes",
        "doc_cache_bytes", "doc_cache_entries", "highlight_lines", "highlight_state_steps",
        "lua_invocations", "lua_errors", "lua_renders", "widgets_created", "widgets_reused",
        "paint_events", "media_budget_bytes", "media_reserved_bytes", "media_cache_bytes",
        "image_decodes", "image_cache_hits", "pdf_renders", "pdf_tiles", "pdf_cache_hits",
        "media_jobs_dropped_queued",
        "media_jobs_discarded", "media_cpu_us_discarded", "media_budget_waits", "media_too_large",
        "attachment_imports", "attachment_gc_deleted",
    };
    return (c >= 0 && c < CounterCount) ? names[c] : "?";
}

void add(Counter c, qint64 v) { g_counters[c].fetch_add(v, std::memory_order_relaxed); }
void set(Counter c, qint64 v) { g_counters[c].store(v, std::memory_order_relaxed); }
qint64 get(Counter c) { return g_counters[c].load(std::memory_order_relaxed); }

bool samplingEnabled() { return g_sampling.load(std::memory_order_relaxed); }
void setSamplingEnabled(bool on) { g_sampling.store(on); }

void record(const char *metric, qint64 micros)
{
    if (!samplingEnabled())
        return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_samples[QString::fromLatin1(metric)].push_back(micros);
}

QHash<QString, QVector<qint64>> takeSamples()
{
    std::lock_guard<std::mutex> lk(g_mu);
    QHash<QString, QVector<qint64>> out;
    out.swap(g_samples);
    return out;
}

} // namespace folia::instr
