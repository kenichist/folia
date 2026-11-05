// Storage-and-tree owner: SQLite schema and migrations, explicit sibling
// order, title lookup, lazy paged child queries, save and recovery.
//
// Threads: one background writer (all mutations, strictly FIFO, one
// transaction per job) and one background reader (queries). The Store object
// itself lives on the main thread; callbacks are delivered there.
//
// Durability (docs/ARCHITECTURE.md §4): WAL, synchronous=FULL (fsync of the
// WAL per commit), checkpoint_fullfsync=ON on macOS. A job that fails (disk
// full, I/O error) stays at the head of the queue with its data and is retried
// with exponential backoff; newer save batches for the same page are merged
// into it, so a failure never drops pending edits.
#pragma once

#include "core/Api.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>

#include <atomic>
#include <memory>

class QLockFile;

namespace folia {

class Store : public QObject, public IStore {
    Q_OBJECT
public:
    struct Options {
        QString path;
        bool faultInjectable = false; // route the writer through the fault VFS
        int retryInitialMs = 250;
        int retryMaxMs = 4000;
        bool singleInstanceLock = true;
    };

    struct Stats {
        qint64 commits = 0;
        qint64 failures = 0;
        qint64 coalesced = 0;
        qint64 bytesWritten = 0;
        qint64 lastCommitMicros = 0;
        qint64 queued = 0;
        qint64 reads = 0;
    };

    explicit Store(QObject *parent = nullptr);
    ~Store() override;

    bool open(const Options &opt, QString *error);
    // Block (main thread) until every queued job is committed or `timeoutMs`
    // elapses. Returns true iff the queue drained with no failing job.
    bool flush(int timeoutMs = 5000);
    void close();
    bool isOpen() const { return m_open; }
    QString databasePath() const { return m_opt.path; }

    // --- IStore
    void openPage(PageId id, QObject *ctx, std::function<void(PageContent)> done) override;
    void fetchChildren(PageId parent, int offset, int limit, QObject *ctx,
                       std::function<void(QVector<PageMeta>, int)> done) override;
    void commit(SaveBatch batch) override;

    // --- more queries (reader thread)
    void pageMeta(PageId id, QObject *ctx, std::function<void(PageMeta, bool)> done);
    void ancestors(PageId id, QObject *ctx, std::function<void(QVector<PageMeta>)> done);
    // Case-insensitive substring match; prefix matches rank first. Superseded
    // searches (an older generation still queued) are skipped.
    void searchTitles(const QString &query, int limit, QObject *ctx,
                      std::function<void(QVector<PageMeta>)> done);
    void recentPages(int limit, QObject *ctx, std::function<void(QVector<PageMeta>)> done);
    // Index of `id` among its siblings (for keyboard reordering in views).
    void siblingIndex(PageId id, QObject *ctx, std::function<void(PageId parent, int index, int count)> done);

    // --- tree commands: ids are allocated synchronously, durability follows
    // in writer order. `index` -1 appends. Child-page link blocks are kept in
    // step with the hierarchy in the same transaction, except for pages listed
    // in `held` (content currently owned by an editor session, which applies
    // the link change itself).
    PageId createPage(PageId parent, int index, const QString &title, bool linkInParent = false);
    void movePage(PageId id, PageId newParent, int index, const QSet<PageId> &held, QObject *ctx,
                  std::function<void(MoveResult, PageId oldParent)> done);
    void trashPage(PageId id, const QSet<PageId> &held, QObject *ctx,
                   std::function<void(bool ok, PageId parent)> done);

    BlockId allocateBlockId() { return m_nextBlockId.fetch_add(1); }
    PageId allocatePageId() { return m_nextPageId.fetch_add(1); }

    // Small key/value settings, cached on the main thread.
    QString setting(const QString &key, const QString &def = {}) const;
    void setSetting(const QString &key, const QString &value);

    Stats stats() const;
    bool hasFailingJob() const { return m_failing.load(); }
    qint64 pageCount() const { return m_pageCountAtOpen; }

signals:
    void committed(folia::PageId page, folia::Revision rev, qint64 micros);
    void commitFailed(folia::PageId page, folia::Revision rev, const folia::StorageError &error);
    void writerRecovered();
    void treeChanged(folia::PageId parent);

private:
    struct Writer;
    struct Reader;
    friend struct Writer;
    friend struct Reader;

    void deliver(QObject *ctx, std::function<void()> fn);

    Options m_opt;
    bool m_open = false;
    std::unique_ptr<Writer> m_writer;
    std::unique_ptr<Reader> m_reader;
    std::atomic<qint64> m_nextBlockId{1};
    std::atomic<qint64> m_nextPageId{1};
    std::atomic<bool> m_failing{false};
    std::atomic<quint64> m_searchGen{0};
    QHash<QString, QString> m_settings;
    qint64 m_pageCountAtOpen = 0;
    QLockFile *m_lock = nullptr;
};

} // namespace folia

Q_DECLARE_METATYPE(folia::StorageError)
