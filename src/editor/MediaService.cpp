#include "editor/MediaService.h"

#include "app/Platform.h"
#include "core/Instrument.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>

#include <cmath>

namespace folia {

size_t qHash(const MediaKey &k, size_t seed)
{
    return qHashMulti(seed, k.hash, k.width, k.height, k.orientation, k.color, k.kind);
}

MediaService::DecodeCost MediaService::costFor(const QString &mime)
{
    // Fitted with headroom to folia_mediaprobe peaks (M1, macOS 26.6) at
    // 400-6000 px outputs and to the in-app peaks of `--bench images`
    // (docs/BENCHMARKS.md §6). JPEG (DCT scaling), PNG and HEIC reduce by up
    // to 1/8 per side; WebP is decoded at full size. Anything else gets the
    // worst case.
    DecodeCost c;
    if (mime == QLatin1String("image/jpeg")) {
        c = {1.5, 2.0, 6ll << 20, 3};
    } else if (mime == QLatin1String("image/png")) {
        c = {1.9, 2.1, 32ll << 20, 3}; // in the GUI app PNG decodes carry ~30 MB more than in the probe
    } else if (mime == QLatin1String("image/heic") || mime == QLatin1String("image/heif")) {
        c = {1.0, 2.0, 8ll << 20, 3};
    } else if (mime == QLatin1String("image/webp")) {
        c = {1.2, 2.1, 4ll << 20, 0};
    }
    return c;
}

qint64 MediaService::estimateFor(const DecodeCost &c, int nw, int nh, int ow, int oh)
{
    int k = 0;
    while (k < c.maxReduction && ow > 0 && (nw >> (k + 1)) >= ow)
        ++k;
    const double inter = 4.0 * double(qMax(1, nw >> k)) * double(qMax(1, nh >> k));
    return qint64(inter * c.intermediateFactor + 4.0 * double(ow) * oh * c.outputFactor) + c.fixedBytes;
}

MediaService::MediaService(const Config &cfg, QObject *parent)
    : QObject(parent), m_cfg(cfg), m_budget(cfg.budgetBytes)
{
    qRegisterMetaType<MediaKey>();
    m_pool.setMaxThreadCount(kLanes); // one worker per lane
    m_pool.setExpiryTimeout(cfg.workerExpiryMs);
    m_decoder = [](const Request &r, QString *error) {
        return platform::decodeImage(r.path, qMax(r.key.width, r.key.height), error);
    };
    m_coolDown.setSingleShot(true);
    connect(&m_coolDown, &QTimer::timeout, this, [this] {
        m_budget.restore();
        pump();
        emit budgetChanged(); // full-density decodes again
    });
}

MediaService::~MediaService()
{
    m_jobs.clear();
    m_pool.waitForDone(); // completions are queued to a dead object: dropped
}

MediaService::Request MediaService::imageRequest(const AttachmentInfo &info, const QString &path, qreal logicalWidth,
                                                 qreal dpr) const
{
    Request r;
    r.path = path;
    r.mime = info.mime;
    r.lane = Lane::Image;
    r.key.hash = info.hash;
    r.key.orientation = quint8(info.orientation);
    if (info.width <= 0 || info.height <= 0)
        return r;
    const DecodeCost c = costFor(info.mime);
    double w = qMax(1.0, qMin<double>(std::round(logicalWidth * dpr), info.width));
    double h = w * info.height / info.width;
    if (w * h > double(m_cfg.maxPixels)) {
        const double s = std::sqrt(double(m_cfg.maxPixels) / (w * h));
        w *= s;
        h *= s;
    }
    // One decode never needs more than kMaxDecodeShare of the current budget:
    // past that (tall images; every image during a pressure cool-down) the
    // bitmap gets a lower density and is drawn scaled up.
    const double room = double(m_budget.limit()) * kMaxDecodeShare;
    int ow = qMax(1, int(std::floor(w)));
    int oh = qMax(1, int(std::floor(h)));
    for (int i = 0; i < 64 && ow > 1 && estimateFor(c, info.width, info.height, ow, oh) > room; ++i) {
        ow = qMax(1, int(ow * 0.92));
        oh = qMax(1, int(std::floor(double(ow) * info.height / info.width)));
    }
    r.key.width = ow;
    r.key.height = oh;
    r.estimate = estimateFor(c, info.width, info.height, ow, oh);
    return r;
}

// --------------------------------------------------------------- cache ----
const QImage *MediaService::lookup(const MediaKey &key)
{
    auto it = m_cache.find(key);
    if (it == m_cache.end())
        return nullptr;
    m_lru.splice(m_lru.begin(), m_lru, it->lru);
    return &it->image;
}

const QImage *MediaService::nearest(const QString &hash, int width, double tolerance, MediaKey *found)
{
    auto it = m_byHash.constFind(hash);
    if (it == m_byHash.cend() || width <= 0)
        return nullptr;
    const MediaKey *best = nullptr;
    int bestDiff = 0;
    for (const MediaKey &k : *it) {
        const int diff = qAbs(k.width - width);
        if (tolerance >= 0 && diff > tolerance * width)
            continue;
        if (!best || diff < bestDiff) {
            best = &k;
            bestDiff = diff;
        }
    }
    if (!best)
        return nullptr;
    if (found)
        *found = *best;
    return lookup(*best);
}

void MediaService::insert(const MediaKey &key, QImage image, bool asLeastRecent)
{
    if (m_cache.contains(key))
        evict(key);
    // At most N resolutions per attachment: drop the least recent other one
    // that nothing shows.
    while (m_byHash.value(key.hash).size() >= m_cfg.maxResolutionsPerHash) {
        MediaKey victim;
        for (auto l = m_lru.rbegin(); l != m_lru.rend(); ++l)
            if (l->hash == key.hash && !m_want.contains(*l)) {
                victim = *l;
                break;
            }
        if (victim.isNull())
            break;
        evict(victim);
    }
    Entry e;
    e.bytes = image.sizeInBytes();
    e.image = std::move(image);
    if (asLeastRecent) {
        m_lru.push_back(key);
        e.lru = std::prev(m_lru.end());
    } else {
        m_lru.push_front(key);
        e.lru = m_lru.begin();
    }
    m_budget.addCached(e.bytes);
    m_cache.insert(key, std::move(e));
    m_byHash[key.hash].push_back(key);
}

void MediaService::evict(const MediaKey &key)
{
    auto it = m_cache.find(key);
    if (it == m_cache.end())
        return;
    // Blocks hold keys only: this entry is the bitmap's only owner, so
    // removing it frees the pixels.
    Q_ASSERT_X(it->image.isDetached(), "MediaService::evict", "a QImage copy outlives its cache entry");
    m_budget.removeCached(it->bytes);
    m_lru.erase(it->lru);
    m_cache.erase(it);
    auto h = m_byHash.find(key.hash);
    if (h != m_byHash.end()) {
        h->removeAll(key);
        if (h->isEmpty())
            m_byHash.erase(h);
    }
}

qint64 MediaService::evictableBytes() const
{
    qint64 n = 0;
    for (auto it = m_cache.cbegin(); it != m_cache.cend(); ++it)
        if (!m_want.contains(it.key()))
            n += it->bytes;
    return n;
}

qint64 MediaService::evictUnwanted(qint64 bytes, const QString &except)
{
    qint64 freed = 0;
    for (auto l = m_lru.end(); l != m_lru.begin() && freed < bytes;) {
        --l;
        if (m_want.contains(*l) || (!except.isEmpty() && l->hash == except))
            continue;
        const MediaKey k = *l;
        const qint64 b = m_cache.value(k).bytes;
        l = std::next(l); // evict() erases the node; continue from its successor
        evict(k);
        freed += b;
    }
    return freed;
}

void MediaService::trim(qint64 target)
{
    while (m_budget.cached() > target && !m_lru.empty())
        evict(m_lru.back());
}

void MediaService::memoryPressure(bool critical)
{
    trim(critical ? 0 : qMin(m_cfg.warningFloorBytes, m_budget.cached()));
    m_budget.lower(critical ? 0.25 : 0.5);
    m_coolDown.start(m_cfg.coolDownMs); // one wakeup when it ends
    emit budgetChanged();
}

// ---------------------------------------------------------- scheduling ----
void MediaService::recomputeWants()
{
    m_want.clear();
    for (auto o = m_owners.cbegin(); o != m_owners.cend(); ++o) {
        int order = 0;
        for (const Request &r : *o) {
            auto it = m_want.find(r.key);
            if (it == m_want.end())
                m_want.insert(r.key, {r.priority, order});
            else if (r.priority < it->priority || (r.priority == it->priority && order < it->order))
                *it = {r.priority, order};
            ++order;
        }
    }
}

void MediaService::setWanted(const void *owner, const QVector<Request> &wants)
{
    QVector<Request> &mine = m_owners[owner];
    // Cache hits: keys this owner newly wants that are already decoded.
    QSet<MediaKey> before;
    before.reserve(mine.size());
    for (const Request &r : std::as_const(mine))
        before.insert(r.key);
    for (const Request &r : wants)
        if (!before.contains(r.key) && m_cache.contains(r.key))
            instr::add(instr::ImageCacheHits);
    mine = wants;
    if (mine.isEmpty())
        m_owners.remove(owner);
    recomputeWants();
    // New jobs: one per key, for keys neither cached nor already pending.
    for (const Request &r : wants) {
        if (r.key.isNull() || m_cache.contains(r.key) || m_tooLarge.contains(r.key) || m_failed.contains(r.key.hash))
            continue;
        auto j = m_jobs.find(r.key);
        if (j == m_jobs.end()) {
            Job job;
            job.req = r;
            m_jobs.insert(r.key, job);
        }
    }
    // Stale jobs are dropped before they start; priorities follow the wants.
    for (auto j = m_jobs.begin(); j != m_jobs.end();) {
        auto w = m_want.constFind(j.key());
        if (w == m_want.cend()) {
            if (!j->running) {
                instr::add(instr::MediaJobsDroppedQueued);
                j = m_jobs.erase(j);
                continue;
            }
        } else {
            j->req.priority = w->priority;
            j->order = w->order;
        }
        ++j;
    }
    pump();
}

void MediaService::removeOwner(const void *owner)
{
    if (m_owners.contains(owner))
        setWanted(owner, {});
}

int MediaService::queuedJobs() const
{
    int n = 0;
    for (const Job &j : m_jobs)
        n += j.running ? 0 : 1;
    return n;
}

int MediaService::runningJobs() const
{
    int n = 0;
    for (const MediaKey &k : m_running)
        n += k.isNull() ? 0 : 1;
    return n;
}

void MediaService::pump()
{
    for (int l = 0; l < kLanes; ++l)
        while (m_running[l].isNull() && startNext(Lane(l))) {
        }
}

// Starts the best queued job of `lane`. Returns true if the lane should be
// examined again (a job was refused and another may be startable).
bool MediaService::startNext(Lane lane)
{
    Job *best = nullptr;
    bool visibleWaiting = false;
    for (auto it = m_jobs.begin(); it != m_jobs.end(); ++it) {
        if (it->running)
            continue;
        if (it->req.priority == 0)
            visibleWaiting = true;
        if (it->req.lane != lane)
            continue;
        if (!best || it->req.priority < best->req.priority
            || (it->req.priority == best->req.priority && it->order < best->order))
            best = &*it;
    }
    if (!best)
        return false;
    if (best->req.priority > 0) {
        // Directional prefetch: only on an idle worker, below half the budget.
        if (visibleWaiting || runningJobs() > 0 || m_budget.used() >= m_budget.limit() * m_cfg.prefetchMaxUse)
            return false;
    }
    const qint64 est = best->req.estimate;
    switch (m_budget.check(est, evictableBytes())) {
    case MediaBudget::Admit::TooLarge: {
        const MediaKey key = best->req.key;
        m_tooLarge.insert(key);
        m_jobs.remove(key);
        instr::add(instr::MediaTooLarge);
        emit refused(key);
        return true;
    }
    case MediaBudget::Admit::Wait:
        if (!best->waitCounted) {
            best->waitCounted = true;
            instr::add(instr::MediaBudgetWaits);
        }
        return false;
    case MediaBudget::Admit::Run:
        break;
    }
    if (const qint64 over = m_budget.overBy(est))
        evictUnwanted(over);
    best->running = true;
    m_budget.reserve(est);
    const Request req = best->req;
    m_running[int(lane)] = req.key;
    ++m_inFlight;
    QPointer<MediaService> self(this);
    Decoder decoder = m_decoder;
    m_pool.start([self, req, decoder] {
        QElapsedTimer t;
        t.start();
        const qint64 cpu0 = platform::threadCpuMicros();
        QString error;
        QImage img = decoder(req, &error);
        const qint64 cpu = platform::threadCpuMicros() - cpu0;
        const qint64 wall = t.nsecsElapsed() / 1000;
        if (!self)
            return; // nobody to deliver to: the bitmap is freed here
        QMetaObject::invokeMethod(self.data(), [self, key = req.key, img = std::move(img), error, cpu, wall]() mutable {
            if (self)
                self->finished(key, std::move(img), error, cpu, wall);
        }, Qt::QueuedConnection);
    });
    return false;
}

void MediaService::finished(const MediaKey &key, QImage image, const QString &error, qint64 cpuUs, qint64 wallUs)
{
    --m_inFlight;
    auto j = m_jobs.find(key);
    Lane lane = Lane::Image;
    if (j != m_jobs.end()) {
        lane = j->req.lane;
        m_budget.release(j->req.estimate);
        m_jobs.erase(j);
    }
    if (m_running[int(lane)] == key)
        m_running[int(lane)] = MediaKey();
    if (lane == Lane::Image)
        instr::add(instr::ImageDecodes);
    else
        instr::add(instr::PdfRenders);
    instr::record(lane == Lane::Image ? "image_decode_us" : "pdf_render_us", wallUs);
    instr::record(lane == Lane::Image ? "image_decode_cpu_us" : "pdf_render_cpu_us", cpuUs);
    if (image.isNull()) {
        m_failed.insert(key.hash);
        Q_UNUSED(error);
        emit refused(key);
    } else if (!m_want.contains(key)) {
        // Finished but unwanted: never shown. Kept only if it fits as is.
        instr::add(instr::MediaJobsDiscarded);
        instr::add(instr::MediaCpuUsDiscarded, cpuUs);
        if (m_budget.used() + image.sizeInBytes() <= m_budget.limit())
            insert(key, std::move(image), /*asLeastRecent=*/true);
    } else {
        const qint64 bytes = image.sizeInBytes();
        if (const qint64 over = m_budget.overBy(bytes))
            evictUnwanted(over, key.hash);
        insert(key, std::move(image), false);
        emit arrived(key);
    }
    pump();
}

void MediaService::waitIdle(int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while ((!m_jobs.isEmpty() || m_inFlight > 0) && t.elapsed() < timeoutMs) {
        // Jobs that are only waiting for budget never finish by themselves.
        if (m_inFlight == 0 && runningJobs() == 0)
            break;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
}

} // namespace folia
