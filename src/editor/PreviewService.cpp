#include "editor/PreviewService.h"

#include "core/Instrument.h"
#ifdef FOLIA_WITH_MATH
#include "math/MathRenderer.h"
#endif

#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>

namespace folia {

namespace {
qint64 listBytes(const PreviewResult &r)
{
#ifdef FOLIA_WITH_MATH
    return r.list ? r.list->byteSize() : 0;
#else
    Q_UNUSED(r);
    return 0;
#endif
}
} // namespace

PreviewService::PreviewService(qint64 cap, int threads, QObject *parent)
    : QObject(parent), m_cap(cap)
{
    m_pool.setMaxThreadCount(qMax(1, threads));
    m_pool.setExpiryTimeout(2000); // idle worker threads exit: no idle wakeups
}

PreviewService::~PreviewService()
{
    for (auto &v : m_latest)
        v.second->store(true);
    m_pool.waitForDone();
}

quint64 PreviewService::keyFor(const PreviewRequest &req)
{
    quint64 h = quint64(qHash(req.source, 0x51ed270b));
    h ^= quint64(qRound(req.pointSize * 64)) * 0x9e3779b97f4a7c15ull;
    h ^= quint64(req.kind) << 61;
    return h;
}

bool PreviewService::lookup(const PreviewRequest &req, PreviewResult *out)
{
    auto it = m_cache.find(keyFor(req));
    if (it == m_cache.end())
        return false;
    m_lru.splice(m_lru.begin(), m_lru, it->lru);
    out->block = req.block;
    out->blockRev = req.blockRev;
    out->ok = bool(it->list);
    out->list = it->list;
    out->error = it->error;
    out->errorOffset = it->errorOffset;
    out->cacheHit = true;
    instr::add(instr::PreviewCacheHits);
    return true;
}

void PreviewService::insert(quint64 key, Entry e)
{
    auto it = m_cache.find(key);
    if (it != m_cache.end()) {
        m_bytes -= it->bytes;
        m_lru.erase(it->lru);
        m_cache.erase(it);
    }
    m_lru.push_front(key);
    e.lru = m_lru.begin();
    m_bytes += e.bytes;
    m_cache.insert(key, std::move(e));
    trim(m_cap);
}

void PreviewService::trim(qint64 target)
{
    while (m_bytes > target && !m_lru.empty()) {
        const quint64 k = m_lru.back();
        m_lru.pop_back();
        auto it = m_cache.find(k);
        if (it != m_cache.end()) {
            m_bytes -= it->bytes;
            m_cache.erase(it);
        }
    }
    instr::set(instr::PreviewCacheBytes, m_bytes);
}

void PreviewService::setCacheCap(qint64 bytes)
{
    m_cap = bytes;
    trim(m_cap);
}

void PreviewService::cancel(BlockId block)
{
    auto it = m_latest.find(block);
    if (it != m_latest.end()) {
        it->second->store(true);
        m_latest.erase(it);
    }
}

void PreviewService::waitIdle()
{
    m_pool.waitForDone();
}

void PreviewService::renderPreview(const PreviewRequest &req, QObject *ctx,
                                   std::function<void(PreviewResult)> done)
{
    instr::add(instr::PreviewRequests);
    PreviewResult cached;
    if (lookup(req, &cached)) {
        // Supersede anything outstanding for this block, then answer now.
        cancel(req.block);
        QPointer<QObject> guard(ctx);
        QMetaObject::invokeMethod(this, [guard, done, cached] {
            if (guard)
                done(cached);
        }, Qt::QueuedConnection);
        return;
    }
    // Newest request per block wins; older outstanding ones are cancelled.
    auto prev = m_latest.find(req.block);
    if (prev != m_latest.end()) {
        prev->second->store(true);
        instr::add(instr::PreviewCancelled);
    }
    const quint64 seq = ++m_seq;
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    m_latest.insert(req.block, {seq, cancelFlag});
    ++m_outstanding;
    instr::set(instr::PreviewOutstanding, m_outstanding);
    const quint64 key = keyFor(req);
    QPointer<QObject> guard(ctx);
    QPointer<PreviewService> self(this);
    m_pool.start([self, req, key, seq, cancelFlag, guard, done] {
        PreviewResult res;
        res.block = req.block;
        res.blockRev = req.blockRev;
        QElapsedTimer t;
        t.start();
#ifdef FOLIA_WITH_MATH
        math::Style st;
        st.pointSize = req.pointSize;
        st.display = req.kind == PreviewKind::DisplayMath;
        math::Result r = math::render(req.source, st, math::Limits{}, cancelFlag.get());
        res.ok = r.ok;
        res.error = r.error;
        res.errorOffset = r.errorOffset;
        if (r.ok)
            res.list = std::make_shared<const MathDisplayList>(std::move(r.list));
#else
        res.ok = false;
        res.error = QStringLiteral("math rendering not built");
#endif
        res.renderMicros = t.nsecsElapsed() / 1000;
        const bool cancelled = cancelFlag->load();
        if (!self)
            return;
        QMetaObject::invokeMethod(self.data(), [self, res, key, seq, cancelled, guard, done] {
            if (!self)
                return;
            PreviewService *s = self.data();
            --s->m_outstanding;
            instr::set(instr::PreviewOutstanding, s->m_outstanding);
            if (cancelled && res.error == QLatin1String("cancelled")) {
                return;
            }
            ++s->m_rendered;
            instr::add(instr::PreviewRenders);
            instr::record("preview_render_us", res.renderMicros);
            Entry e;
            e.list = res.list;
            e.error = res.error;
            e.errorOffset = res.errorOffset;
            e.bytes = 96 + listBytes(res) + res.error.size() * 2;
            s->insert(key, std::move(e));
            auto it = s->m_latest.find(res.block);
            if (it == s->m_latest.end() || it->first != seq) {
                instr::add(instr::PreviewStaleDrops);
                return; // superseded: cached, never shown
            }
            s->m_latest.erase(it);
            if (guard)
                done(res);
        }, Qt::QueuedConnection);
    });
}

} // namespace folia
