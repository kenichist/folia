#include "storage/AttachmentStore.h"

#include "app/Platform.h"
#include "core/Instrument.h"
#include "storage/Sql.h"
#include "storage/Store.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QImageWriter>
#include <QMetaObject>
#include <QPointer>
#include <QRandomGenerator>
#include <QThread>

#include <mutex>

#ifdef Q_OS_WIN
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace folia {

struct AttachmentStore::Pins {
    std::mutex mu;
    QHash<QString, int> counts;
};

namespace {

constexpr qint64 kChunk = 256 * 1024;
const QLatin1String kTempPrefix(".import-");

// Forwards writes to a file and hashes them on the way (PNG encoding of a
// pasted image goes straight to disk, never through a whole-file buffer).
class HashingWriter : public QIODevice {
public:
    HashingWriter(QFile *out, QCryptographicHash *hash) : m_out(out), m_hash(hash) { open(QIODevice::WriteOnly); }
    qint64 total = 0;

protected:
    qint64 readData(char *, qint64) override { return -1; }
    qint64 writeData(const char *data, qint64 len) override
    {
        const qint64 n = m_out->write(data, len);
        if (n > 0) {
            m_hash->addData(QByteArrayView(data, n));
            total += n;
        }
        return n;
    }

private:
    QFile *m_out;
    QCryptographicHash *m_hash;
};

bool syncFile(QFile &f)
{
    if (!f.flush())
        return false;
#ifdef Q_OS_WIN
    return _commit(f.handle()) == 0;
#else
    return ::fsync(f.handle()) == 0;
#endif
}

void syncDir(const QString &dir)
{
#ifndef Q_OS_WIN
    const int fd = ::open(QFile::encodeName(dir).constData(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#else
    Q_UNUSED(dir);
#endif
}

// Atomic rename that replaces an existing file (the same content re-imported).
bool replaceFile(const QString &from, const QString &to)
{
#ifdef Q_OS_WIN
    return MoveFileExW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(from).utf16()),
                       reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(to).utf16()),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    return ::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0;
#endif
}

} // namespace

AttachmentStore::AttachmentStore(Store *store, const Options &opt, QObject *parent)
    : QObject(parent), m_store(store), m_opt(opt), m_pins(std::make_shared<Pins>())
{
    m_pool.setMaxThreadCount(1);
    m_pool.setExpiryTimeout(30000); // see docs/ATTACHMENTS.md §5 (worker expiry)
    m_gcTimer.setSingleShot(true);
    connect(&m_gcTimer, &QTimer::timeout, this, [this] {
        if (m_inFlight > 0) {
            m_gcTimer.start(m_opt.gcDelayMs);
            return;
        }
        collectGarbage();
    });
    connect(store, &Store::attachmentsLoaded, this, [this](const QVector<AttachmentInfo> &infos) {
        for (const AttachmentInfo &a : infos)
            remember(a);
    });
    // A sweep after launch only when there is something to sweep: it clears
    // what a crash left between writing a file and saving its reference.
    if (!QDir(m_opt.dir).isEmpty(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot))
        m_gcTimer.start(m_opt.startupGcDelayMs);
}

AttachmentStore::~AttachmentStore()
{
    m_pool.waitForDone();
}

QString AttachmentStore::pathFor(const QString &hash) const
{
    return m_opt.dir + QLatin1Char('/') + hash;
}

// ------------------------------------------------------------ metadata ----
const AttachmentInfo *AttachmentStore::info(const QString &hash) const
{
    auto it = m_info.constFind(hash);
    return it == m_info.cend() ? nullptr : &*it;
}

void AttachmentStore::remember(const AttachmentInfo &a)
{
    if (!a.valid())
        return;
    m_info.insert(a.hash, a);
    m_missing.remove(a.hash);
}

void AttachmentStore::fetchInfo(const QString &hash)
{
    if (!attachments::isHash(hash) || m_info.contains(hash) || m_fetching.contains(hash) || m_missing.contains(hash))
        return;
    m_fetching.insert(hash);
    m_store->attachmentInfo({hash}, this, [this, hash](QVector<AttachmentInfo> rows) {
        if (!rows.isEmpty()) {
            m_fetching.remove(hash);
            remember(rows.first());
            emit infoArrived(hash);
            return;
        }
        // No row (lost to a crash, or text pasted from elsewhere): derive it
        // from the file's header, off the main thread.
        const QString path = pathFor(hash);
        QPointer<AttachmentStore> self(this);
        m_pool.start([self, hash, path] {
            AttachmentInfo a;
            a.hash = hash;
            const bool ok = QFileInfo::exists(path) && readMetadata(path, &a, nullptr);
            QMetaObject::invokeMethod(self, [self, hash, a, ok] {
                if (!self)
                    return;
                self->m_fetching.remove(hash);
                if (ok) {
                    self->remember(a);
                    self->m_store->putAttachment(a);
                } else {
                    self->m_missing.insert(hash);
                }
                emit self->infoArrived(hash);
            }, Qt::QueuedConnection);
        });
    });
}

// -------------------------------------------------------------- import ----
void AttachmentStore::runImport(const Source &src, ImportResult *res)
{
    QElapsedTimer t;
    t.start();
    if (!QDir().mkpath(m_opt.dir)) {
        res->error = tr("Cannot create the attachments folder");
        return;
    }
    const QString tmp = m_opt.dir + QLatin1Char('/') + kTempPrefix
        + QString::number(QRandomGenerator::global()->generate64(), 16) + QStringLiteral(".tmp");
    QFile out(tmp);
    if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        res->error = out.errorString();
        return;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    bool ok = true;
    if (!src.path.isEmpty() || src.device) {
        QFile file(src.path);
        QIODevice *in = src.device;
        if (!in && file.open(QIODevice::ReadOnly))
            in = &file;
        if (!in) {
            res->error = file.errorString();
            ok = false;
        } else {
            QByteArray buf(int(kChunk), Qt::Uninitialized);
            for (;;) {
                const qint64 n = in->read(buf.data(), kChunk);
                if (n < 0) {
                    res->error = in->errorString();
                    ok = false;
                    break;
                }
                if (n == 0)
                    break;
                hash.addData(QByteArrayView(buf.constData(), n));
                if (out.write(buf.constData(), n) != n) {
                    res->error = out.errorString();
                    ok = false;
                    break;
                }
                res->bytesCopied += n;
            }
        }
    } else if (!src.bytes.isEmpty()) {
        for (qint64 at = 0; ok && at < src.bytes.size(); at += kChunk) {
            const qint64 n = qMin<qint64>(kChunk, src.bytes.size() - at);
            hash.addData(QByteArrayView(src.bytes.constData() + at, n));
            ok = out.write(src.bytes.constData() + at, n) == n;
            res->bytesCopied += n;
        }
        if (!ok)
            res->error = out.errorString();
    } else if (!src.image.isNull()) {
        HashingWriter w(&out, &hash);
        QImageWriter iw(&w, "png");
        ok = iw.write(src.image);
        res->bytesCopied = w.total;
        if (!ok)
            res->error = iw.errorString();
    } else {
        ok = false;
        res->error = tr("Nothing to import");
    }
    if (ok && !syncFile(out)) {
        ok = false;
        res->error = out.errorString();
    }
    out.close();
    if (!ok) {
        QFile::remove(tmp);
        return;
    }
    const QString h = QString::fromLatin1(hash.result().toHex());
    const QString final = pathFor(h);
    {
        // Pinned before the file appears under its name, so a sweep can never
        // delete it between here and the reference being inserted.
        std::lock_guard<std::mutex> lk(m_pins->mu);
        ++m_pins->counts[h];
    }
    res->info.hash = h; // the main thread releases this pin
    if (!replaceFile(tmp, final)) {
        res->error = tr("Cannot store the attachment");
        QFile::remove(tmp);
        return;
    }
    syncDir(m_opt.dir);
    QString why;
    if (!readMetadata(final, &res->info, &why)) {
        res->error = why;
        return; // unreferenced: the next sweep removes it
    }
    res->alt = src.name;
    res->ok = true;
    res->micros = t.nsecsElapsed() / 1000;
}

bool AttachmentStore::readMetadata(const QString &path, AttachmentInfo *a, QString *error)
{
    // PDFs first: ImageIO also opens some PDFs (as a picture of page 1).
    if (platform::looksLikePdf(path)) {
        const platform::PdfInfo pi = platform::pdfInfo(path);
        if (!pi.ok) {
            if (error)
                *error = tr("Cannot read this PDF (%1)").arg(pi.error);
            return false;
        }
        a->mime = QStringLiteral("application/pdf");
        a->bytes = QFileInfo(path).size();
        a->pages = pi.pages;
        a->pageGeom = attachments::encodePageGeom(pi.geom);
        a->width = qMax(1, qRound(pi.geom.first().w)); // first page, points, as shown
        a->height = qMax(1, qRound(pi.geom.first().h));
        a->orientation = 1;
        return true;
    }
    const platform::ImageInfo ii = platform::imageInfo(path);
    if (!ii.ok) {
        if (error)
            *error = tr("Not a supported image or PDF (%1)").arg(ii.error);
        return false;
    }
    a->mime = ii.mime;
    a->bytes = QFileInfo(path).size();
    a->width = ii.width;
    a->height = ii.height;
    a->orientation = ii.orientation;
    return true;
}

void AttachmentStore::import(const Source &src, QObject *ctx, std::function<void(ImportResult)> done)
{
    ++m_inFlight;
    QPointer<QObject> guard(ctx);
    QPointer<AttachmentStore> self(this);
    m_pool.start([this, self, src, guard, done] {
        auto res = std::make_shared<ImportResult>();
        runImport(src, res.get()); // `this` outlives the pool (destructor waits)
        QMetaObject::invokeMethod(this, [self, res, guard, done] {
            if (!self)
                return;
            AttachmentStore *s = self.data();
            --s->m_inFlight;
            if (res->ok) {
                instr::add(instr::AttachmentImports);
                instr::record("attachment_import_us", res->micros);
                s->remember(res->info);
                // Queued on the writer before the page save that will carry
                // the reference: metadata row first, reference second.
                s->m_store->putAttachment(res->info);
            }
            if (guard && done)
                done(*res); // inserts the reference; the session pins the hash
            if (!res->info.hash.isEmpty())
                s->unpin(res->info.hash); // the import's own pin
        }, Qt::QueuedConnection);
    });
}

void AttachmentStore::runJob(std::function<void()> work, QObject *ctx, std::function<void()> done)
{
    ++m_inFlight;
    QPointer<QObject> guard(ctx);
    QPointer<AttachmentStore> self(this);
    m_pool.start([self, work, guard, done] {
        work();
        QMetaObject::invokeMethod(self.data(), [self, guard, done] {
            if (!self)
                return;
            --self->m_inFlight;
            if (guard && done)
                done();
        }, Qt::QueuedConnection);
    });
}

AttachmentStore::ImportResult AttachmentStore::importNow(const Source &src)
{
    ImportResult res;
    runImport(src, &res);
    if (!res.ok && !res.info.hash.isEmpty()) {
        unpin(res.info.hash); // nothing will reference it
        res.info.hash.clear();
    }
    return res;
}

void AttachmentStore::waitForImports()
{
    m_pool.waitForDone();
    QElapsedTimer t;
    t.start();
    while (m_inFlight > 0 && t.elapsed() < 10000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

// ---------------------------------------------------------------- pins ----
void AttachmentStore::pin(const QString &hash)
{
    std::lock_guard<std::mutex> lk(m_pins->mu);
    ++m_pins->counts[hash];
}

void AttachmentStore::unpin(const QString &hash)
{
    bool released = false;
    {
        std::lock_guard<std::mutex> lk(m_pins->mu);
        auto it = m_pins->counts.find(hash);
        if (it == m_pins->counts.end())
            return;
        if (--*it <= 0) {
            m_pins->counts.erase(it);
            released = true;
        }
    }
    if (released)
        scheduleGc(); // the last thing that could bring it back is gone
}

bool AttachmentStore::isPinned(const QString &hash) const
{
    std::lock_guard<std::mutex> lk(m_pins->mu);
    return m_pins->counts.value(hash) > 0;
}

// ------------------------------------------------------------------ gc ----
void AttachmentStore::scheduleGc()
{
    if (QThread::currentThread() != thread()) { // an unpin on the import worker
        QMetaObject::invokeMethod(this, &AttachmentStore::scheduleGc, Qt::QueuedConnection);
        return;
    }
    m_gcTimer.start(m_opt.gcDelayMs); // restarted: one sweep per quiet period
}

void AttachmentStore::collectGarbage(std::function<void(int)> done)
{
    m_gcTimer.stop();
    auto deleted = std::make_shared<QStringList>();
    const QString dir = m_opt.dir;
    const qint64 orphanAge = m_opt.orphanAgeMs;
    std::shared_ptr<Pins> pins = m_pins;
    m_store->writeJob([deleted, dir, orphanAge, pins](sqlite3 *db) -> int {
        QElapsedTimer sweep;
        sweep.start();
        deleted->clear();
        // Runs inside the writer's transaction: no save can interleave, so
        // "referenced" is exact for everything committed. Uncommitted
        // references live in open sessions, which pin them.
        QSet<QString> referenced;
        {
            // Both forms: `![alt](attachment:…)` (images), `[name](attachment:…)` (PDFs).
            sql::Stmt s(db, "SELECT text FROM blocks WHERE kind=0 AND text LIKE '%](attachment:%)'");
            int rc;
            while ((rc = s.step()) == SQLITE_ROW) {
                QString h;
                if (attachments::parseAnyMarkdown(s.text(0), &h))
                    referenced.insert(h);
            }
            if (rc != SQLITE_DONE)
                return rc;
        }
        QSet<QString> rows;
        {
            sql::Stmt s(db, "SELECT hash FROM attachments");
            int rc;
            while ((rc = s.step()) == SQLITE_ROW)
                rows.insert(s.text(0));
            if (rc != SQLITE_DONE)
                return rc;
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        QHash<QString, qint64> files; // hash -> age (ms)
        QStringList staleTemps;
        const QFileInfoList entries = QDir(dir).entryInfoList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
        for (const QFileInfo &fi : entries) {
            const QString name = fi.fileName();
            const qint64 age = now - fi.lastModified().toMSecsSinceEpoch();
            if (attachments::isHash(name))
                files.insert(name, age);
            else if (name.startsWith(kTempPrefix) && age > orphanAge)
                staleTemps << fi.absoluteFilePath();
        }
        QSet<QString> candidates = rows;
        for (auto it = files.cbegin(); it != files.cend(); ++it)
            candidates.insert(it.key());
        candidates.subtract(referenced);
        sql::Stmt del(db, "DELETE FROM attachments WHERE hash=?1");
        for (const QString &h : std::as_const(candidates)) {
            std::lock_guard<std::mutex> lk(pins->mu); // an import cannot pin it mid-delete
            if (pins->counts.value(h) > 0)
                continue;
            if (!rows.contains(h) && files.value(h) < orphanAge)
                continue; // a file with no row yet: leave young ones alone
            QFile::remove(dir + QLatin1Char('/') + h);
            if (rows.contains(h)) {
                del.bind(1, h);
                if (!del.exec())
                    return del.lastError();
                del.reset();
            }
            deleted->push_back(h);
        }
        for (const QString &p : std::as_const(staleTemps))
            QFile::remove(p);
        instr::record("attachment_gc_us", sweep.nsecsElapsed() / 1000);
        return SQLITE_OK;
    }, this, [this, deleted, done] {
        instr::add(instr::AttachmentGcDeleted, deleted->size());
        for (const QString &h : std::as_const(*deleted))
            m_info.remove(h);
        if (done)
            done(int(deleted->size()));
    });
}

} // namespace folia
