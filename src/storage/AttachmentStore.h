// Content-addressed attachment originals (docs/ATTACHMENTS.md §1).
//
//  * Files live under <data folder>/attachments/<sha256>; metadata rows live
//    in the workspace's `attachments` table (written through Store's writer,
//    so they are ordered with page saves).
//  * Import runs on one worker thread and never reads a file whole: it
//    streams the source into a temp file while hashing, fsyncs, renames the
//    file to its hash (fsyncing the folder), reads the header metadata, and
//    only then reports back, so the caller inserts the reference after the
//    file is durable (file first, reference second). A pasted in-memory image
//    is encoded to PNG on the worker.
//  * Garbage collection is batched and reference-based: a file is deleted
//    only when no stored block (live or trashed page) references it and no
//    open session pins it (pins stand for what an undo stack could bring
//    back) and no import is writing it. The sweep runs on the writer thread,
//    in one transaction, well after the last reference went away.
//  * The main-thread metadata cache answers layout queries (image sizes)
//    without touching the database.
#pragma once

#include "core/Attachments.h"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QThreadPool>
#include <QTimer>

#include <functional>
#include <memory>

namespace folia {

class Store;

class AttachmentStore : public QObject {
    Q_OBJECT
public:
    struct Options {
        QString dir;                         // .../attachments
        int gcDelayMs = 60000;               // after the last pin is released
        int startupGcDelayMs = 120000;       // first sweep after launch (orphans of a crash)
        qint64 orphanAgeMs = 10 * 60 * 1000; // unpinned files without a row / temp files
    };

    struct Source {
        QString path;      // a file to copy (streamed), or
        QByteArray bytes;  // encoded image data (clipboard PNG/JPEG/...), or
        QImage image;      // pixels, encoded to PNG on the worker
        QString name;      // suggested alt text (file name without extension)
    };

    struct ImportResult {
        bool ok = false;
        AttachmentInfo info;
        QString alt;
        QString error;
        qint64 micros = 0;
        qint64 bytesCopied = 0;
    };

    AttachmentStore(Store *store, const Options &opt, QObject *parent = nullptr);
    ~AttachmentStore() override;

    QString directory() const { return m_opt.dir; }
    QString pathFor(const QString &hash) const;

    // ---- metadata (main thread)
    const AttachmentInfo *info(const QString &hash) const;
    void remember(const AttachmentInfo &info);
    // Unknown hash: look it up (database, then the file's header) and emit
    // infoArrived(hash) when known, or missing(hash).
    void fetchInfo(const QString &hash);
    bool isMissing(const QString &hash) const { return m_missing.contains(hash); }

    // ---- import (callback on ctx's thread; dropped if ctx is gone)
    void import(const Source &src, QObject *ctx, std::function<void(ImportResult)> done);
    int importsInFlight() const { return m_inFlight; }
    void waitForImports();

    // ---- pins (thread-safe). Every open session pins each hash it has
    // shown, until it closes; imports pin while they write.
    void pin(const QString &hash);
    void unpin(const QString &hash);
    bool isPinned(const QString &hash) const;

    // ---- garbage collection
    void scheduleGc();
    // Sweep now (on the writer thread); done(deleted) on the main thread.
    void collectGarbage(std::function<void(int deleted)> done = {});

signals:
    void infoArrived(const QString &hash);

private:
    struct Pins;
    void runImport(const Source &src, ImportResult *res);

    Store *m_store;
    Options m_opt;
    QThreadPool m_pool; // one import worker
    std::shared_ptr<Pins> m_pins;
    QHash<QString, AttachmentInfo> m_info;
    QSet<QString> m_fetching;
    QSet<QString> m_missing;
    QTimer m_gcTimer;
    int m_inFlight = 0;
};

} // namespace folia
