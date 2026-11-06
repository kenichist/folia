// Decoded-media cache and scheduler (docs/ATTACHMENTS.md §2, §3, §5).
//
//  * One MediaBudget covers cached pixels and reserved in-flight work.
//  * Two lanes, one scheduler: at most one image decode and at most one PDF
//    render run at a time, concurrently only when both reservations fit.
//  * The queue is owned here, not by QThreadPool: each owner (a layout)
//    states what it wants now, in priority order (visible first, then at most
//    one viewport of directional prefetch). Queued jobs nobody wants any more
//    are dropped before they start; there is one pending job per cache key.
//    Prefetch starts only when nothing visible is waiting and the budget is
//    under half used.
//  * Results nobody wants any more when they finish are not shown (their CPU
//    time is counted) and only cached if they fit without evicting anything.
//  * Blocks keep cache keys, never QImage copies: paint looks a key up and
//    draws from the cache entry, so evicting an entry frees its pixels.
// Main thread only, except the decoder, which runs on the lane's worker.
#pragma once

#include "core/Attachments.h"
#include "editor/MediaBudget.h"
#include "editor/MediaKey.h"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QSize>
#include <QThreadPool>
#include <QTimer>
#include <QVector>

#include <functional>
#include <list>

namespace folia {



class MediaService : public QObject {
    Q_OBJECT
public:
    enum class Lane : quint8 { Image = 0, Pdf = 1 };
    static constexpr int kLanes = 2;

    struct Request {
        MediaKey key;
        QString path;
        QString mime;
        Lane lane = Lane::Image;
        int priority = 0;     // 0 visible, 1 prefetch
        qint64 estimate = 0;  // working set to reserve (bytes)
    };
    // Runs on a worker. Returns a null image (and sets *error) on failure.
    using Decoder = std::function<QImage(const Request &, QString *error)>;

    struct Config {
        qint64 budgetBytes = 128ll << 20;  // cached pixels + reserved work
        qint64 maxPixels = 16ll << 20;    // decoded bitmap clamp (16 MP)
        int maxResolutionsPerHash = 2;
        int workerExpiryMs = 30000;
        int coolDownMs = 60000;
        double prefetchMaxUse = 0.5;      // prefetch only below half the budget
        qint64 warningFloorBytes = 16ll << 20;
    };

    // Per-format working-set model, calibrated with folia_mediaprobe
    // (docs/BENCHMARKS.md, "Image decode cost"). ImageIO decodes at
    // native/2^k, the largest power-of-two reduction (k <= maxReduction) that
    // still covers the output, then scales into the thumbnail, which is
    // copied into our bitmap:
    //   estimate = intermediate×intermediateFactor + output×outputFactor + fixedBytes
    // with sizes in bytes at 4 bytes per pixel.
    struct DecodeCost {
        double intermediateFactor = 1.2;
        double outputFactor = 2.0;
        qint64 fixedBytes = 12ll << 20;
        int maxReduction = 0; // 0: decodes at full size whatever the output
    };
    static qint64 estimateFor(const DecodeCost &c, int nativeW, int nativeH, int outW, int outH);
    // One decode may reserve at most this fraction of the current budget; the
    // decoded size is clamped so it fits.
    static constexpr double kMaxDecodeShare = 0.75;
    static DecodeCost costFor(const QString &mime);

    explicit MediaService(const Config &cfg, QObject *parent = nullptr);
    MediaService() : MediaService(Config{}) {}
    ~MediaService() override;

    void setDecoder(Decoder d) { m_decoder = std::move(d); }
    const Config &config() const { return m_cfg; }
    MediaBudget &budget() { return m_budget; }
    const MediaBudget &budget() const { return m_budget; }

    // The bitmap to decode for an image shown `logicalWidth` wide at `dpr`:
    // physical width = logical × dpr, capped at the native width, height from
    // the aspect ratio, total pixels clamped so one decode fits the current
    // (possibly lowered) budget: tall images, and every image during a
    // pressure cool-down, get a lower density and are drawn scaled up rather
    // than not at all. Also fills the reservation estimate.
    Request imageRequest(const AttachmentInfo &info, const QString &path, qreal logicalWidth, qreal dpr) const;

    // ---- cache (paint)
    const QImage *lookup(const MediaKey &key);       // touches the LRU
    bool contains(const MediaKey &key) const { return m_cache.contains(key); }
    // The cached resolution of `hash` nearest to `width`; only within
    // ±tolerance (fraction) unless tolerance < 0.
    const QImage *nearest(const QString &hash, int width, double tolerance, MediaKey *found = nullptr);
    bool tooLarge(const MediaKey &key) const { return m_tooLarge.contains(key); }
    bool failed(const QString &hash) const { return m_failed.contains(hash); }
    qint64 cacheBytes() const { return m_budget.cached(); }
    int cacheEntries() const { return int(m_cache.size()); }

    // ---- scheduling
    // Replaces everything `owner` wanted; `wants` are in priority order.
    void setWanted(const void *owner, const QVector<Request> &wants);
    void removeOwner(const void *owner);
    bool wanted(const MediaKey &key) const { return m_want.contains(key); }
    int queuedJobs() const;
    int runningJobs() const;
    void waitIdle(int timeoutMs = 30000); // tests and benchmarks

    // ---- memory
    void trim(qint64 targetBytes);
    // Trim toward 0 (critical) or a floor (warning) and lower the budget for
    // the cool-down period.
    void memoryPressure(bool critical);

signals:
    void arrived(const folia::MediaKey &key); // a wanted bitmap is now cached
    void refused(const folia::MediaKey &key); // too large to preview, or failed
    // The effective budget changed (pressure, end of a cool-down): request
    // sizes follow it, so owners should state their wants again.
    void budgetChanged();

private:
    struct Entry {
        QImage image;
        qint64 bytes = 0;
        std::list<MediaKey>::iterator lru;
    };
    struct Job {
        Request req;
        int order = 0;
        bool running = false;
        bool waitCounted = false;
    };
    struct Want {
        int priority = 0;
        int order = 0;
    };

    void recomputeWants();
    void pump();
    bool startNext(Lane lane);
    void finished(const MediaKey &key, QImage image, const QString &error, qint64 cpuUs, qint64 wallUs);
    void insert(const MediaKey &key, QImage image, bool asLeastRecent);
    void evict(const MediaKey &key);
    qint64 evictUnwanted(qint64 bytes, const QString &except = QString());
    qint64 evictableBytes() const;

    Config m_cfg;
    MediaBudget m_budget;
    Decoder m_decoder;
    QThreadPool m_pool;
    QHash<MediaKey, Entry> m_cache;
    std::list<MediaKey> m_lru; // front = most recent
    QHash<QString, QVector<MediaKey>> m_byHash;
    QHash<MediaKey, Job> m_jobs; // one pending job per key
    MediaKey m_running[kLanes];
    QHash<const void *, QVector<Request>> m_owners;
    QHash<MediaKey, Want> m_want;
    QSet<MediaKey> m_tooLarge;
    QSet<QString> m_failed;
    QTimer m_coolDown;
    int m_inFlight = 0; // results not yet delivered back (incl. abandoned)
};

} // namespace folia

