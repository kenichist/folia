// IPreviewService over the native math renderer: bounded worker pool,
// byte-capped LRU cache of display lists, per-block supersession and
// cancellation. Results for anything but a block's newest request are cached
// but never delivered ("only the current revision can update the view").
#pragma once

#include "core/Api.h"

#include <QHash>
#include <QObject>
#include <QThreadPool>

#include <atomic>
#include <list>
#include <memory>

namespace folia {

class PreviewService : public QObject, public IPreviewService {
    Q_OBJECT
public:
    explicit PreviewService(qint64 cacheCapBytes = 4 << 20, int threads = 1, QObject *parent = nullptr);
    ~PreviewService() override;

    void renderPreview(const PreviewRequest &req, QObject *ctx,
                       std::function<void(PreviewResult)> done) override;
    void cancel(BlockId block) override;

    // Synchronous cache probe (main thread).
    bool lookup(const PreviewRequest &req, PreviewResult *out);
    void trim(qint64 targetBytes);
    void setCacheCap(qint64 bytes);
    qint64 cacheBytes() const { return m_bytes; }
    int outstanding() const { return m_outstanding; }
    bool rendererUsed() const { return m_rendered > 0; }
    // Block until all queued renders finish (tests/bench).
    void waitIdle();

private:
    struct Entry {
        std::shared_ptr<const MathDisplayList> list;
        QString error;
        int errorOffset = -1;
        qint64 bytes = 0;
        std::list<quint64>::iterator lru;
    };
    static quint64 keyFor(const PreviewRequest &req);
    void insert(quint64 key, Entry e);

    QThreadPool m_pool;
    qint64 m_cap;
    qint64 m_bytes = 0;
    QHash<quint64, Entry> m_cache;
    std::list<quint64> m_lru; // front = most recent
    QHash<BlockId, std::pair<quint64, std::shared_ptr<std::atomic<bool>>>> m_latest; // block -> (seq, cancel)
    quint64 m_seq = 0;
    int m_outstanding = 0;
    qint64 m_rendered = 0;
};

} // namespace folia
