#include "storage/Store.h"

#include "core/Instrument.h"
#include "storage/FaultVfs.h"
#include "storage/Sql.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLockFile>
#include <QMetaObject>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace folia {

namespace {

constexpr qint64 kGap = 1 << 16;
constexpr int kSchemaVersion = 2;

const char *kSchemaV1 = R"SQL(
CREATE TABLE IF NOT EXISTS pages(
  id INTEGER PRIMARY KEY,
  parent_id INTEGER NOT NULL DEFAULT 0,
  ord INTEGER NOT NULL,
  title TEXT NOT NULL DEFAULT '',
  rev INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  trashed INTEGER NOT NULL DEFAULT 0,
  sel_anchor_block INTEGER NOT NULL DEFAULT 0,
  sel_anchor_off INTEGER NOT NULL DEFAULT 0,
  sel_pos_block INTEGER NOT NULL DEFAULT 0,
  sel_pos_off INTEGER NOT NULL DEFAULT 0,
  scroll_y INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS pages_by_parent ON pages(parent_id, trashed, ord);
CREATE INDEX IF NOT EXISTS pages_by_title ON pages(title COLLATE NOCASE);
CREATE INDEX IF NOT EXISTS pages_by_updated ON pages(updated_at);
CREATE TABLE IF NOT EXISTS blocks(
  id INTEGER PRIMARY KEY,
  page_id INTEGER NOT NULL,
  ord INTEGER NOT NULL,
  kind INTEGER NOT NULL,
  lang TEXT NOT NULL DEFAULT '',
  text TEXT NOT NULL DEFAULT '',
  target INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS blocks_by_page ON blocks(page_id, ord);
CREATE TABLE IF NOT EXISTS kv(key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;
)SQL";

// v2: attachment metadata (docs/ATTACHMENTS.md §1). The originals are files
// named by hash; width/height are after the orientation transform.
const char *kSchemaV2 = R"SQL(
CREATE TABLE IF NOT EXISTS attachments(
  hash TEXT PRIMARY KEY,
  mime TEXT NOT NULL DEFAULT '',
  bytes INTEGER NOT NULL DEFAULT 0,
  width INTEGER NOT NULL DEFAULT 0,
  height INTEGER NOT NULL DEFAULT 0,
  orientation INTEGER NOT NULL DEFAULT 1,
  pages INTEGER NOT NULL DEFAULT 0,
  page_geom BLOB,
  created_at INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;
)SQL";

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

AttachmentInfo readAttachment(const sql::Stmt &s)
{
    // Column order: hash, mime, bytes, width, height, orientation, pages, page_geom
    AttachmentInfo a;
    a.hash = s.text(0);
    a.mime = s.text(1);
    a.bytes = s.i64(2);
    a.width = s.i32(3);
    a.height = s.i32(4);
    a.orientation = s.i32(5);
    a.pages = s.i32(6);
    a.pageGeom = s.blob(7);
    return a;
}

QVector<AttachmentInfo> queryAttachments(sqlite3 *db, const QStringList &hashes)
{
    QVector<AttachmentInfo> out;
    if (hashes.isEmpty())
        return out;
    sql::Stmt s(db, "SELECT hash, mime, bytes, width, height, orientation, pages, page_geom FROM attachments WHERE hash=?1");
    for (const QString &h : hashes) {
        s.bind(1, h);
        if (s.step() == SQLITE_ROW)
            out.push_back(readAttachment(s));
        s.reset();
    }
    return out;
}

bool applyPragmas(sqlite3 *db, bool writer, QString *error)
{
    const char *common =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA busy_timeout=4000;"
        "PRAGMA temp_store=MEMORY;"
        "PRAGMA cache_size=-2048;"
        "PRAGMA mmap_size=0;";
    if (sql::execAll(db, common, error) != SQLITE_OK)
        return false;
    if (writer) {
        const char *w =
            "PRAGMA synchronous=FULL;"
            "PRAGMA journal_size_limit=4194304;"
#ifdef Q_OS_MACOS
            "PRAGMA checkpoint_fullfsync=ON;"
#endif
            ;
        return sql::execAll(db, w, error) == SQLITE_OK;
    }
    return sql::execAll(db, "PRAGMA query_only=ON;", error) == SQLITE_OK;
}

sqlite3 *openDb(const QString &path, const char *vfs, bool writer, QString *error)
{
    sqlite3 *db = nullptr;
    const int flags = (writer ? (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) : SQLITE_OPEN_READONLY)
        | SQLITE_OPEN_NOMUTEX;
    const int rc = sqlite3_open_v2(QFile::encodeName(path).constData(), &db, flags, vfs);
    if (rc != SQLITE_OK) {
        if (error)
            *error = db ? sql::errorString(db) : QString::fromUtf8(sqlite3_errstr(rc));
        sqlite3_close(db);
        return nullptr;
    }
    sqlite3_extended_result_codes(db, 1);
    if (!applyPragmas(db, writer, error)) {
        sqlite3_close(db);
        return nullptr;
    }
    return db;
}

PageMeta readMeta(const sql::Stmt &s)
{
    // Column order: id, parent_id, ord, title, rev, child_count
    PageMeta m;
    m.id = s.i64(0);
    m.parent = s.i64(1);
    m.ord = s.i64(2);
    m.title = s.text(3);
    m.rev = Revision(s.i64(4));
    m.childCount = s.i32(5);
    return m;
}

#define META_COLS \
    "p.id, p.parent_id, p.ord, p.title, p.rev, " \
    "(SELECT COUNT(*) FROM pages c WHERE c.parent_id = p.id AND c.trashed = 0)"

// Longest strictly increasing subsequence over `vals` (entries < 0 are
// "absent" and never kept). Returns a keep-mask.
std::vector<bool> lisKeep(const std::vector<qint64> &vals)
{
    const int n = int(vals.size());
    std::vector<int> tailIdx, prev(n, -1);
    std::vector<qint64> tails;
    for (int i = 0; i < n; ++i) {
        if (vals[i] < 0)
            continue;
        auto it = std::lower_bound(tails.begin(), tails.end(), vals[i]);
        const int pos = int(it - tails.begin());
        if (it == tails.end()) {
            tails.push_back(vals[i]);
            tailIdx.push_back(i);
        } else {
            *it = vals[i];
            tailIdx[pos] = i;
        }
        prev[i] = pos > 0 ? tailIdx[pos - 1] : -1;
    }
    std::vector<bool> keep(n, false);
    int k = tailIdx.empty() ? -1 : tailIdx.back();
    while (k >= 0) {
        keep[k] = true;
        k = prev[k];
    }
    return keep;
}

} // namespace

// ============================================================== Writer ====
struct Store::Writer {
    struct Job {
        enum Type { Save, Create, Move, Trash, Setting, Custom } type = Save;
        SaveBatch batch;
        PageMeta meta;   // Create
        int index = -1;  // Create/Move
        PageId id = 0, newParent = 0;
        bool linkInParent = false;
        QSet<PageId> held;
        QPointer<QObject> ctx;
        std::function<void(MoveResult, PageId)> moveDone;
        std::function<void(bool, PageId)> trashDone;
        QString key, value;
        std::function<int(sqlite3 *)> custom;
        std::function<void()> customDone;
        int attempts = 0;
    };

    Store *store;
    sqlite3 *db = nullptr;
    std::thread thread;
    std::mutex mu;
    std::condition_variable cv, idleCv;
    std::deque<std::shared_ptr<Job>> queue;
    std::unordered_map<PageId, std::shared_ptr<Job>> queuedSave; // not yet taken
    bool stopping = false;
    bool busy = false;
    int retryDelayMs = 0;
    QString crashPoint;
    int crashAfter = 1;
    int saveCommitsSeen = 0;

    std::atomic<qint64> commits{0}, failures{0}, coalesced{0}, bytes{0}, lastMicros{0};

    // Prepared statements (writer thread only)
    std::unique_ptr<sql::Stmt> sUpsert, sDelete, sTitle, sRev, sSel, sOrdList, sSetOrd,
        sDeleteRow, sMaxOrd;

    void start()
    {
        thread = std::thread([this] { run(); });
    }

    void enqueue(std::shared_ptr<Job> job)
    {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (job->type == Job::Save) {
                auto it = queuedSave.find(job->batch.page);
                if (it != queuedSave.end()) {
                    it->second->batch.mergeNewer(job->batch);
                    coalesced.fetch_add(1);
                    return;
                }
                queuedSave.emplace(job->batch.page, job);
            }
            queue.push_back(std::move(job));
        }
        cv.notify_one();
    }

    bool waitIdle(int timeoutMs)
    {
        std::unique_lock<std::mutex> lk(mu);
        return idleCv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                               [this] { return queue.empty() && !busy; });
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lk(mu);
            stopping = true;
        }
        cv.notify_all();
        if (thread.joinable())
            thread.join();
        sUpsert.reset(); sDelete.reset(); sTitle.reset(); sRev.reset(); sSel.reset();
        sOrdList.reset(); sSetOrd.reset(); sDeleteRow.reset(); sMaxOrd.reset();
        if (db)
            sqlite3_close(db);
        db = nullptr;
    }

    void prepare()
    {
        sUpsert = std::make_unique<sql::Stmt>(db,
            "INSERT INTO blocks(id, page_id, ord, kind, lang, text, target) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7) "
            "ON CONFLICT(id) DO UPDATE SET page_id=excluded.page_id, kind=excluded.kind, "
            "lang=excluded.lang, text=excluded.text, target=excluded.target RETURNING ord");
        sDelete = std::make_unique<sql::Stmt>(db, "DELETE FROM blocks WHERE id=?1 AND page_id=?2");
        sTitle = std::make_unique<sql::Stmt>(db, "UPDATE pages SET title=?2 WHERE id=?1");
        sRev = std::make_unique<sql::Stmt>(db,
            "UPDATE pages SET rev=MAX(rev, ?2), updated_at=?3 WHERE id=?1");
        sSel = std::make_unique<sql::Stmt>(db,
            "UPDATE pages SET sel_anchor_block=?2, sel_anchor_off=?3, sel_pos_block=?4, "
            "sel_pos_off=?5, scroll_y=?6 WHERE id=?1");
        sOrdList = std::make_unique<sql::Stmt>(db, "SELECT id, ord FROM blocks WHERE page_id=?1");
        sSetOrd = std::make_unique<sql::Stmt>(db, "UPDATE blocks SET ord=?2 WHERE id=?1");
        sDeleteRow = std::make_unique<sql::Stmt>(db, "DELETE FROM blocks WHERE id=?1");
        sMaxOrd = std::make_unique<sql::Stmt>(db, "SELECT COALESCE(MAX(ord), 0) FROM blocks WHERE page_id=?1");
    }

    void maybeCrash(const char *point)
    {
        if (crashPoint == QLatin1String(point) && saveCommitsSeen >= crashAfter) {
            fprintf(stderr, "folia: injected crash at %s after %d save commits\n", point, saveCommitsSeen);
            fflush(stderr);
            std::abort();
        }
    }

    int execSave(const SaveBatch &b, qint64 *bytesOut)
    {
        qint64 written = 0;
        const qint64 now = nowMs();
        QHash<BlockId, bool> inserted;
        if (b.hasTitle) {
            sTitle->bind(1, b.page).bind(2, b.title);
            if (!sTitle->exec())
                return sTitle->lastError();
            written += b.title.size() * 2;
        }
        bool needsAppend = false;
        for (const BlockRecord &r : b.upserts) {
            sUpsert->bind(1, r.id).bind(2, b.page).bind(3, qint64(0)).bind(4, int(r.kind))
                .bind(5, r.lang).bind(6, r.text).bind(7, r.target);
            int rc = sUpsert->step();
            // RETURNING ord: a brand-new row still has ord 0 and must be placed
            // by the order step (updates never touch ord).
            if (rc == SQLITE_ROW) {
                if (sUpsert->i64(0) == 0)
                    inserted.insert(r.id, true);
                rc = sUpsert->step();
            }
            sUpsert->reset();
            if (rc != SQLITE_DONE)
                return rc;
            written += (r.text.size() + r.lang.size()) * 2 + 32;
        }
        for (BlockId id : b.deletes) {
            sDelete->bind(1, id).bind(2, b.page);
            if (!sDelete->exec())
                return sDelete->lastError();
            written += 8;
        }
        if (b.hasOrder) {
            if (int rc = applyOrder(b.page, b.order, &written))
                return rc;
        } else if (!inserted.isEmpty()) {
            needsAppend = true;
        }
        if (needsAppend) {
            sMaxOrd->bind(1, b.page);
            qint64 maxOrd = sMaxOrd->step() == SQLITE_ROW ? sMaxOrd->i64(0) : 0;
            sMaxOrd->reset();
            for (const BlockRecord &r : b.upserts) {
                if (!inserted.contains(r.id))
                    continue;
                maxOrd += kGap;
                sSetOrd->bind(1, r.id).bind(2, maxOrd);
                if (!sSetOrd->exec())
                    return sSetOrd->lastError();
            }
        }
        if (b.hasSelection) {
            const SelectionAnchor &s = b.selection;
            sSel->bind(1, b.page).bind(2, s.anchorBlock).bind(3, s.anchorOffset).bind(4, s.posBlock)
                .bind(5, s.posOffset).bind(6, s.scrollY);
            if (!sSel->exec())
                return sSel->lastError();
        }
        sRev->bind(1, b.page).bind(2, qint64(b.rev)).bind(3, now);
        if (!sRev->exec())
            return sRev->lastError();
        *bytesOut = written;
        return SQLITE_OK;
    }

    // Reassign sparse `ord` keys so blocks follow `order`, touching as few rows
    // as possible: keep the longest already-increasing subsequence, place the
    // rest in the gaps, renumber everything only when a gap is exhausted.
    // Rows of this page that are not in `order` are deleted.
    int applyOrder(PageId page, const QVector<BlockId> &order, qint64 *written)
    {
        std::unordered_map<qint64, qint64> existing;
        sOrdList->bind(1, page);
        while (sOrdList->step() == SQLITE_ROW)
            existing.emplace(sOrdList->i64(0), sOrdList->i64(1));
        const int listRc = sOrdList->lastError();
        sOrdList->reset();
        if (listRc != SQLITE_DONE)
            return listRc;

        const int n = order.size();
        std::vector<qint64> cur(n, -1);
        for (int i = 0; i < n; ++i) {
            auto it = existing.find(order[i]);
            if (it != existing.end()) {
                cur[i] = it->second > 0 ? it->second : -1; // ord 0 == freshly inserted
                existing.erase(it);
            }
        }
        for (const auto &stale : existing) { // in DB, not in the page anymore
            sDeleteRow->bind(1, stale.first);
            if (!sDeleteRow->exec())
                return sDeleteRow->lastError();
        }
        std::vector<bool> keep = lisKeep(cur);
        std::vector<qint64> next(n, -1);
        bool renumber = false;
        int i = 0;
        qint64 lo = 0;
        while (i < n && !renumber) {
            if (keep[i]) {
                next[i] = cur[i];
                lo = cur[i];
                ++i;
                continue;
            }
            int j = i;
            while (j < n && !keep[j])
                ++j;
            const int run = j - i;
            const qint64 hi = j < n ? cur[j] : lo + qint64(run + 1) * kGap;
            const qint64 step = (hi - lo) / (run + 1);
            if (step < 1) {
                renumber = true;
                break;
            }
            for (int k = 0; k < run; ++k)
                next[i + k] = lo + step * (k + 1);
            i = j;
        }
        if (renumber) {
            for (int k = 0; k < n; ++k)
                next[k] = qint64(k + 1) * kGap;
        }
        for (int k = 0; k < n; ++k) {
            if (next[k] == cur[k])
                continue;
            sSetOrd->bind(1, order[k]).bind(2, next[k]);
            if (!sSetOrd->exec())
                return sSetOrd->lastError();
            *written += 16;
        }
        return SQLITE_OK;
    }

    // Sibling ord for inserting at `index` under `parent` (excluding `self`).
    int siblingOrd(PageId parent, int index, PageId self, qint64 *out)
    {
        sql::Stmt s(db, "SELECT id, ord FROM pages WHERE parent_id=?1 AND trashed=0 AND id<>?2 ORDER BY ord");
        s.bind(1, parent).bind(2, self);
        std::vector<std::pair<qint64, qint64>> sib;
        while (s.step() == SQLITE_ROW)
            sib.emplace_back(s.i64(0), s.i64(1));
        if (s.lastError() != SQLITE_DONE)
            return s.lastError();
        const int n = int(sib.size());
        if (index < 0 || index > n)
            index = n;
        const qint64 lo = index > 0 ? sib[index - 1].second : 0;
        if (index == n) {
            *out = lo + kGap;
            return SQLITE_OK;
        }
        const qint64 hi = sib[index].second;
        if (hi - lo > 1) {
            *out = lo + (hi - lo) / 2;
            return SQLITE_OK;
        }
        // Gap exhausted: renumber siblings, leaving room at `index`.
        sql::Stmt up(db, "UPDATE pages SET ord=?2 WHERE id=?1");
        for (int k = 0; k < n; ++k) {
            const qint64 o = qint64(k + (k >= index ? 2 : 1)) * kGap;
            up.bind(1, sib[k].first).bind(2, o);
            if (!up.exec())
                return up.lastError();
        }
        *out = qint64(index + 1) * kGap;
        return SQLITE_OK;
    }

    int appendLink(PageId page, PageId target)
    {
        sql::Stmt m(db, "SELECT COALESCE(MAX(ord), 0) FROM blocks WHERE page_id=?1");
        m.bind(1, page);
        const qint64 ord = (m.step() == SQLITE_ROW ? m.i64(0) : 0) + kGap;
        sql::Stmt ins(db, "INSERT INTO blocks(id, page_id, ord, kind, lang, text, target) VALUES(?1, ?2, ?3, 3, '', '', ?4)");
        ins.bind(1, store->allocateBlockId()).bind(2, page).bind(3, ord).bind(4, target);
        return ins.exec() ? SQLITE_OK : ins.lastError();
    }

    int removeLinks(PageId page, PageId target)
    {
        sql::Stmt d(db, "DELETE FROM blocks WHERE page_id=?1 AND kind=3 AND target=?2");
        d.bind(1, page).bind(2, target);
        return d.exec() ? SQLITE_OK : d.lastError();
    }

    int execCreate(const Job &j)
    {
        qint64 ord = 0;
        if (int rc = siblingOrd(j.meta.parent, j.index, j.meta.id, &ord))
            return rc;
        sql::Stmt s(db,
            "INSERT INTO pages(id, parent_id, ord, title, rev, created_at, updated_at) "
            "VALUES(?1, ?2, ?3, ?4, 0, ?5, ?5)");
        s.bind(1, j.meta.id).bind(2, j.meta.parent).bind(3, ord).bind(4, j.meta.title).bind(5, nowMs());
        if (!s.exec())
            return s.lastError();
        if (j.linkInParent && j.meta.parent != 0 && !j.held.contains(j.meta.parent))
            return appendLink(j.meta.parent, j.meta.id);
        return SQLITE_OK;
    }

    int execMove(const Job &j, MoveResult *res, PageId *oldParent)
    {
        sql::Stmt cur(db, "SELECT parent_id FROM pages WHERE id=?1 AND trashed=0");
        cur.bind(1, j.id);
        if (cur.step() != SQLITE_ROW) {
            *res = MoveResult::NotFound;
            return SQLITE_OK;
        }
        *oldParent = cur.i64(0);
        if (j.newParent != 0) {
            if (j.newParent == j.id) {
                *res = MoveResult::Cycle;
                return SQLITE_OK;
            }
            // Reject cycles: the new parent must not be a descendant of id.
            sql::Stmt anc(db,
                "WITH RECURSIVE up(id) AS (SELECT ?1 UNION ALL "
                "SELECT p.parent_id FROM pages p JOIN up ON p.id = up.id WHERE p.parent_id <> 0) "
                "SELECT EXISTS(SELECT 1 FROM up WHERE id = ?2), "
                "EXISTS(SELECT 1 FROM pages WHERE id = ?1 AND trashed = 0)");
            anc.bind(1, j.newParent).bind(2, j.id);
            if (anc.step() != SQLITE_ROW)
                return anc.lastError();
            if (anc.i32(0)) {
                *res = MoveResult::Cycle;
                return SQLITE_OK;
            }
            if (!anc.i32(1)) {
                *res = MoveResult::NotFound;
                return SQLITE_OK;
            }
        }
        qint64 ord = 0;
        if (int rc = siblingOrd(j.newParent, j.index, j.id, &ord))
            return rc;
        sql::Stmt up(db, "UPDATE pages SET parent_id=?2, ord=?3, updated_at=?4 WHERE id=?1");
        up.bind(1, j.id).bind(2, j.newParent).bind(3, ord).bind(4, nowMs());
        if (!up.exec())
            return up.lastError();
        if (*oldParent != j.newParent) {
            if (*oldParent != 0 && !j.held.contains(*oldParent))
                if (int rc = removeLinks(*oldParent, j.id))
                    return rc;
            if (j.newParent != 0 && !j.held.contains(j.newParent))
                if (int rc = appendLink(j.newParent, j.id))
                    return rc;
        }
        *res = MoveResult::Ok;
        return SQLITE_OK;
    }

    int execTrash(Job &j)
    {
        sql::Stmt p(db, "SELECT parent_id FROM pages WHERE id=?1");
        p.bind(1, j.id);
        j.meta.parent = p.step() == SQLITE_ROW ? p.i64(0) : 0;
        if (j.meta.parent != 0 && !j.held.contains(j.meta.parent))
            if (int rc = removeLinks(j.meta.parent, j.id))
                return rc;
        sql::Stmt s(db,
            "WITH RECURSIVE sub(id) AS (SELECT ?1 UNION ALL "
            "SELECT p.id FROM pages p JOIN sub ON p.parent_id = sub.id) "
            "UPDATE pages SET trashed=1, updated_at=?2 WHERE id IN sub");
        s.bind(1, j.id).bind(2, nowMs());
        return s.exec() ? SQLITE_OK : s.lastError();
    }

    int execSetting(const Job &j)
    {
        sql::Stmt s(db, "INSERT INTO kv(key, value) VALUES(?1, ?2) "
                        "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
        s.bind(1, j.key).bind(2, j.value);
        return s.exec() ? SQLITE_OK : s.lastError();
    }

    void run()
    {
        prepare();
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lk(mu);
                if (retryDelayMs > 0 && !stopping) {
                    cv.wait_for(lk, std::chrono::milliseconds(retryDelayMs), [this] { return stopping; });
                }
                cv.wait(lk, [this] { return stopping || !queue.empty(); });
                if (queue.empty()) {
                    if (stopping)
                        break;
                    continue;
                }
                job = queue.front();
                queue.pop_front();
                if (job->type == Job::Save) {
                    auto it = queuedSave.find(job->batch.page);
                    if (it != queuedSave.end() && it->second == job)
                        queuedSave.erase(it);
                }
                busy = true;
            }
            process(job);
            {
                std::lock_guard<std::mutex> lk(mu);
                busy = false;
                if (queue.empty())
                    idleCv.notify_all();
                if (stopping && retryDelayMs > 0) {
                    // Give up retrying on shutdown; flush() already reported failure.
                    break;
                }
            }
        }
    }

    void process(const std::shared_ptr<Job> &job)
    {
        QElapsedTimer t;
        t.start();
        int rc = sql::execAll(db, "BEGIN IMMEDIATE");
        qint64 written = 0;
        MoveResult moveRes = MoveResult::StorageError;
        PageId oldParent = 0;
        if (rc == SQLITE_OK) {
            switch (job->type) {
            case Job::Save: rc = execSave(job->batch, &written); break;
            case Job::Create: rc = execCreate(*job); break;
            case Job::Move: rc = execMove(*job, &moveRes, &oldParent); break;
            case Job::Trash: rc = execTrash(*job); break;
            case Job::Setting: rc = execSetting(*job); break;
            case Job::Custom: rc = job->custom ? job->custom(db) : SQLITE_OK; break;
            }
            if (rc == SQLITE_OK) {
                if (job->type == Job::Save)
                    maybeCrash("before-commit");
                rc = sql::execAll(db, "COMMIT");
            }
            if (rc != SQLITE_OK)
                sql::execAll(db, "ROLLBACK");
        }
        const qint64 micros = t.nsecsElapsed() / 1000;
        if (rc == SQLITE_OK) {
            retryDelayMs = 0;
            commits.fetch_add(1);
            bytes.fetch_add(written);
            lastMicros.store(micros);
            if (job->type == Job::Save) {
                ++saveCommitsSeen;
                maybeCrash("after-commit");
            }
            onSuccess(job, moveRes, oldParent, micros);
            return;
        }
        // Failure: keep the job (and its data) at the head of the queue.
        failures.fetch_add(1);
        ++job->attempts;
        StorageError err;
        err.sqliteCode = rc;
        err.diskFull = (rc & 0xff) == SQLITE_FULL;
        err.message = QString::fromUtf8(sqlite3_errstr(rc));
        {
            std::lock_guard<std::mutex> lk(mu);
            if (job->type == Job::Save) {
                auto it = queuedSave.find(job->batch.page);
                if (it != queuedSave.end()) {
                    // A newer batch arrived while we were failing: fold it in.
                    job->batch.mergeNewer(it->second->batch);
                    queue.erase(std::find(queue.begin(), queue.end(), it->second));
                    queuedSave.erase(it);
                }
                queuedSave.emplace(job->batch.page, job);
            }
            queue.push_front(job);
            retryDelayMs = retryDelayMs == 0 ? store->m_opt.retryInitialMs
                                             : std::min(retryDelayMs * 2, store->m_opt.retryMaxMs);
        }
        store->m_failing.store(true);
        const PageId page = job->type == Job::Save ? job->batch.page : job->meta.id;
        const Revision rev = job->type == Job::Save ? job->batch.rev : 0;
        Store *s = store;
        QMetaObject::invokeMethod(s, [s, page, rev, err] { emit s->commitFailed(page, rev, err); },
                                  Qt::QueuedConnection);
    }

    void onSuccess(const std::shared_ptr<Job> &job, MoveResult moveRes, PageId oldParent, qint64 micros)
    {
        Store *s = store;
        const bool wasFailing = s->m_failing.exchange(false);
        if (wasFailing)
            QMetaObject::invokeMethod(s, [s] { emit s->writerRecovered(); }, Qt::QueuedConnection);
        switch (job->type) {
        case Job::Save: {
            const PageId page = job->batch.page;
            const Revision rev = job->batch.rev;
            const bool titleChanged = job->batch.hasTitle;
            PageId parent = 0;
            if (titleChanged) {
                sql::Stmt p(db, "SELECT parent_id FROM pages WHERE id=?1");
                p.bind(1, page);
                if (p.step() == SQLITE_ROW)
                    parent = p.i64(0);
            }
            QMetaObject::invokeMethod(s, [s, page, rev, micros, titleChanged, parent] {
                emit s->committed(page, rev, micros);
                if (titleChanged)
                    emit s->treeChanged(parent);
            }, Qt::QueuedConnection);
            break;
        }
        case Job::Create: {
            const PageId parent = job->meta.parent;
            QMetaObject::invokeMethod(s, [s, parent] { emit s->treeChanged(parent); }, Qt::QueuedConnection);
            break;
        }
        case Job::Move: {
            auto done = job->moveDone;
            QPointer<QObject> ctx = job->ctx;
            const PageId np = job->newParent;
            QMetaObject::invokeMethod(s, [s, done, ctx, moveRes, oldParent, np] {
                if (done && ctx)
                    done(moveRes, oldParent);
                if (moveRes == MoveResult::Ok) {
                    emit s->treeChanged(oldParent);
                    if (np != oldParent)
                        emit s->treeChanged(np);
                }
            }, Qt::QueuedConnection);
            break;
        }
        case Job::Trash: {
            auto done = job->trashDone;
            QPointer<QObject> ctx = job->ctx;
            const PageId parent = job->meta.parent;
            QMetaObject::invokeMethod(s, [s, done, ctx, parent] {
                if (done && ctx)
                    done(true, parent);
                emit s->treeChanged(parent);
            }, Qt::QueuedConnection);
            break;
        }
        case Job::Setting:
            break;
        case Job::Custom:
            if (job->customDone) {
                auto done = job->customDone;
                QPointer<QObject> ctx = job->ctx;
                QMetaObject::invokeMethod(s, [done, ctx] {
                    if (ctx)
                        done();
                }, Qt::QueuedConnection);
            }
            break;
        }
    }
};

// ============================================================== Reader ====
struct Store::Reader {
    sqlite3 *db = nullptr;
    std::thread thread;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::function<void(sqlite3 *)>> queue;
    bool stopping = false;
    std::atomic<qint64> reads{0};

    void start()
    {
        thread = std::thread([this] {
            for (;;) {
                std::function<void(sqlite3 *)> job;
                {
                    std::unique_lock<std::mutex> lk(mu);
                    cv.wait(lk, [this] { return stopping || !queue.empty(); });
                    if (stopping && queue.empty())
                        break;
                    job = std::move(queue.front());
                    queue.pop_front();
                }
                job(db);
                reads.fetch_add(1);
            }
        });
    }
    void post(std::function<void(sqlite3 *)> fn)
    {
        {
            std::lock_guard<std::mutex> lk(mu);
            queue.push_back(std::move(fn));
        }
        cv.notify_one();
    }
    void stop()
    {
        {
            std::lock_guard<std::mutex> lk(mu);
            stopping = true;
        }
        cv.notify_all();
        if (thread.joinable())
            thread.join();
        if (db)
            sqlite3_close(db);
        db = nullptr;
    }
};

// =============================================================== Store ====
Store::Store(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<StorageError>();
    qRegisterMetaType<AttachmentInfo>();
}

Store::~Store()
{
    close();
}

bool Store::open(const Options &opt, QString *error)
{
    Q_ASSERT(!m_open);
    m_opt = opt;
    QDir().mkpath(QFileInfo(opt.path).absolutePath());
    if (opt.singleInstanceLock) {
        m_lock = new QLockFile(opt.path + QStringLiteral(".lock"));
        m_lock->setStaleLockTime(0);
        if (!m_lock->tryLock(100)) {
            if (error)
                *error = tr("The workspace is already open in another Folia window.");
            delete m_lock;
            m_lock = nullptr;
            return false;
        }
    }
    const char *vfs = opt.faultInjectable ? faultvfs::install() : nullptr;

    QString err;
    sqlite3 *wdb = openDb(opt.path, vfs, true, &err);
    if (!wdb) {
        if (error)
            *error = err;
        return false;
    }
    // Migrations (single transaction per version step).
    int version = 0;
    {
        sql::Stmt v(wdb, "PRAGMA user_version");
        if (v.step() == SQLITE_ROW)
            version = v.i32(0);
    }
    if (version > kSchemaVersion) {
        if (error)
            *error = tr("Workspace was created by a newer Folia (schema %1).").arg(version);
        sqlite3_close(wdb);
        return false;
    }
    if (version < 1) {
        QString merr;
        if (sql::execAll(wdb, "BEGIN IMMEDIATE", &merr) != SQLITE_OK
            || sql::execAll(wdb, kSchemaV1, &merr) != SQLITE_OK
            || sql::execAll(wdb, "PRAGMA user_version=1", &merr) != SQLITE_OK
            || sql::execAll(wdb, "COMMIT", &merr) != SQLITE_OK) {
            sql::execAll(wdb, "ROLLBACK");
            if (error)
                *error = tr("Could not initialise workspace: %1").arg(merr);
            sqlite3_close(wdb);
            return false;
        }
    }
    if (version < 2) {
        QString merr;
        if (sql::execAll(wdb, "BEGIN IMMEDIATE", &merr) != SQLITE_OK
            || sql::execAll(wdb, kSchemaV2, &merr) != SQLITE_OK
            || sql::execAll(wdb, "PRAGMA user_version=2", &merr) != SQLITE_OK
            || sql::execAll(wdb, "COMMIT", &merr) != SQLITE_OK) {
            sql::execAll(wdb, "ROLLBACK");
            if (error)
                *error = tr("Could not upgrade workspace: %1").arg(merr);
            sqlite3_close(wdb);
            return false;
        }
    }
    {
        // MAX(id) alone is a seek to the end of the table; together with a
        // COUNT(*) it was a scan of an index over every page (2-14 ms at
        // 50,000 pages), and startup only needs to know whether any exists.
        sql::Stmt s(wdb, "SELECT COALESCE(MAX(id), 0) FROM pages");
        if (s.step() == SQLITE_ROW)
            m_nextPageId.store(s.i64(0) + 1);
        m_emptyAtOpen = m_nextPageId.load() <= 1;
        sql::Stmt b(wdb, "SELECT COALESCE(MAX(id), 0) FROM blocks");
        if (b.step() == SQLITE_ROW)
            m_nextBlockId.store(b.i64(0) + 1);
        sql::Stmt kv(wdb, "SELECT key, value FROM kv");
        while (kv.step() == SQLITE_ROW)
            m_settings.insert(kv.text(0), kv.text(1));
    }
    sqlite3 *rdb = openDb(opt.path, nullptr, false, &err);
    if (!rdb) {
        if (error)
            *error = err;
        sqlite3_close(wdb);
        return false;
    }

    m_writer = std::make_unique<Writer>();
    m_writer->store = this;
    m_writer->db = wdb;
    m_writer->crashPoint = qEnvironmentVariable("FOLIA_CRASH_POINT");
    m_writer->crashAfter = qMax(1, qEnvironmentVariableIntValue("FOLIA_CRASH_AFTER"));
    m_writer->start();
    m_reader = std::make_unique<Reader>();
    m_reader->db = rdb;
    m_reader->start();
    m_open = true;
    return true;
}

bool Store::flush(int timeoutMs)
{
    if (!m_open)
        return true;
    const bool idle = m_writer->waitIdle(timeoutMs);
    return idle && !m_failing.load();
}

void Store::close()
{
    if (!m_open)
        return;
    flush(3000);
    // Reader first: the writer must be the last connection to close so
    // SQLite can checkpoint and remove the -wal/-shm files.
    m_reader->stop();
    m_writer->stop();
    m_writer.reset();
    m_reader.reset();
    m_open = false;
    if (m_lock) {
        m_lock->unlock();
        delete m_lock;
        m_lock = nullptr;
    }
}

void Store::deliver(QObject *ctx, std::function<void()> fn)
{
    // Always hop through `this` (main thread, outlives the threads) and check
    // the context there, so a destroyed context never sees a callback.
    QPointer<QObject> guard(ctx);
    QMetaObject::invokeMethod(this, [guard, fn = std::move(fn)] {
        if (guard)
            fn();
    }, Qt::QueuedConnection);
}

namespace {
// One page's blocks in order, plus the metadata of the attachments they reference.
void readPage(sqlite3 *db, PageId id, PageContent &pc, QVector<AttachmentInfo> &infos)
{
    pc.id = id;
    {
        sql::Stmt p(db,
            "SELECT parent_id, title, rev, sel_anchor_block, sel_anchor_off, sel_pos_block, "
            "sel_pos_off, scroll_y FROM pages WHERE id=?1");
        p.bind(1, id);
        if (p.step() == SQLITE_ROW) {
            pc.found = true;
            pc.parent = p.i64(0);
            pc.title = p.text(1);
            pc.rev = Revision(p.i64(2));
            pc.selection.anchorBlock = p.i64(3);
            pc.selection.anchorOffset = p.i32(4);
            pc.selection.posBlock = p.i64(5);
            pc.selection.posOffset = p.i32(6);
            pc.selection.scrollY = p.i32(7);
        }
    }
    QStringList hashes;
    if (pc.found) {
        sql::Stmt b(db, "SELECT id, kind, lang, text, target FROM blocks WHERE page_id=?1 ORDER BY ord");
        b.bind(1, id);
        while (b.step() == SQLITE_ROW) {
            BlockRecord r;
            r.id = b.i64(0);
            r.kind = BlockKind(b.i32(1));
            r.lang = b.text(2);
            r.text = b.text(3);
            r.target = b.i64(4);
            QString hash;
            if (r.kind == BlockKind::Text && attachments::parseAnyMarkdown(r.text, &hash))
                hashes << hash;
            pc.blocks.push_back(std::move(r));
        }
    }
    hashes.removeDuplicates();
    infos = queryAttachments(db, hashes);
}
} // namespace

struct Store::StartFetch {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    PageContent page;
    QVector<AttachmentInfo> infos;
};

void Store::openPage(PageId id, QObject *ctx, std::function<void(PageContent)> done)
{
    QPointer<QObject> guard(ctx);
    m_reader->post([this, id, guard, done](sqlite3 *db) {
        PageContent pc;
        QVector<AttachmentInfo> infos;
        readPage(db, id, pc, infos);
        QMetaObject::invokeMethod(this, [this, guard, done, pc = std::move(pc), infos = std::move(infos)]() mutable {
            if (!infos.isEmpty())
                emit attachmentsLoaded(infos); // before the page's layout asks for sizes
            if (guard)
                done(std::move(pc));
        }, Qt::QueuedConnection);
    });
}

void Store::prefetchStartPage(PageId id)
{
    auto fetch = std::make_shared<StartFetch>();
    m_startFetch = fetch;
    m_reader->post([fetch, id](sqlite3 *db) {
        PageId target = 0;
        if (id > 0) {
            sql::Stmt s(db, "SELECT 1 FROM pages WHERE id=?1 AND trashed=0");
            s.bind(1, id);
            if (s.step() == SQLITE_ROW)
                target = id;
        }
        if (target == 0) {
            sql::Stmt s(db, "SELECT id FROM pages WHERE parent_id=0 AND trashed=0 ORDER BY ord LIMIT 1");
            if (s.step() == SQLITE_ROW)
                target = s.i64(0);
        }
        PageContent pc;
        QVector<AttachmentInfo> infos;
        if (target > 0)
            readPage(db, target, pc, infos);
        {
            std::lock_guard<std::mutex> lk(fetch->mu);
            fetch->page = std::move(pc);
            fetch->infos = std::move(infos);
            fetch->done = true;
        }
        fetch->cv.notify_all();
    });
}

bool Store::takeStartPage(PageContent *page, int timeoutMs)
{
    const std::shared_ptr<StartFetch> fetch = std::move(m_startFetch);
    m_startFetch.reset();
    if (!fetch)
        return false;
    QVector<AttachmentInfo> infos;
    {
        std::unique_lock<std::mutex> lk(fetch->mu);
        if (!fetch->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return fetch->done; }))
            return false;
        *page = std::move(fetch->page);
        infos = std::move(fetch->infos);
    }
    if (!page->found)
        return false;
    if (!infos.isEmpty())
        emit attachmentsLoaded(infos); // before the page's layout asks for sizes
    return true;
}

qint64 Store::pageCount()
{
    if (!m_open)
        return 0;
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    qint64 n = 0;
    m_reader->post([&](sqlite3 *db) {
        sql::Stmt s(db, "SELECT COUNT(*) FROM pages");
        const qint64 v = s.step() == SQLITE_ROW ? s.i64(0) : 0;
        std::lock_guard<std::mutex> lk(mu);
        n = v;
        done = true;
        cv.notify_all();
    });
    std::unique_lock<std::mutex> lk(mu);
    cv.wait(lk, [&] { return done; });
    return n;
}

void Store::fetchChildren(PageId parent, int offset, int limit, QObject *ctx,
                          std::function<void(QVector<PageMeta>, int)> done)
{
    QPointer<QObject> guard(ctx);
    m_reader->post([this, parent, offset, limit, guard, done](sqlite3 *db) {
        QVector<PageMeta> rows;
        int total = 0;
        {
            sql::Stmt c(db, "SELECT COUNT(*) FROM pages WHERE parent_id=?1 AND trashed=0");
            c.bind(1, parent);
            if (c.step() == SQLITE_ROW)
                total = c.i32(0);
        }
        sql::Stmt s(db, "SELECT " META_COLS " FROM pages p WHERE p.parent_id=?1 AND p.trashed=0 "
                        "ORDER BY p.ord LIMIT ?2 OFFSET ?3");
        s.bind(1, parent).bind(2, limit).bind(3, offset);
        rows.reserve(qMin(limit, 1024));
        while (s.step() == SQLITE_ROW)
            rows.push_back(readMeta(s));
        QMetaObject::invokeMethod(this, [guard, done, rows = std::move(rows), total]() mutable {
            if (guard)
                done(std::move(rows), total);
        }, Qt::QueuedConnection);
    });
}

void Store::pageMeta(PageId id, QObject *ctx, std::function<void(PageMeta, bool)> done)
{
    QPointer<QObject> guard(ctx);
    m_reader->post([this, id, guard, done](sqlite3 *db) {
        sql::Stmt s(db, "SELECT " META_COLS " FROM pages p WHERE p.id=?1 AND p.trashed=0");
        s.bind(1, id);
        PageMeta m;
        const bool found = s.step() == SQLITE_ROW;
        if (found)
            m = readMeta(s);
        QMetaObject::invokeMethod(this, [guard, done, m, found] {
            if (guard)
                done(m, found);
        }, Qt::QueuedConnection);
    });
}

void Store::pagesMeta(const QVector<PageId> &ids, QObject *ctx, std::function<void(QVector<PageMeta>)> done)
{
    QPointer<QObject> guard(ctx);
    m_reader->post([this, ids, guard, done](sqlite3 *db) {
        QVector<PageMeta> rows;
        // Bound in chunks, far below SQLite's limit on host parameters.
        constexpr int kChunk = 500;
        for (int at = 0; at < ids.size(); at += kChunk) {
            const int n = qMin(kChunk, int(ids.size()) - at);
            QByteArray q = "SELECT " META_COLS " FROM pages p WHERE p.trashed=0 AND p.id IN (";
            for (int i = 0; i < n; ++i)
                q += i ? ",?" : "?";
            q += ')';
            sql::Stmt s(db, q.constData());
            for (int i = 0; i < n; ++i)
                s.bind(i + 1, qint64(ids[at + i]));
            while (s.step() == SQLITE_ROW)
                rows.push_back(readMeta(s));
        }
        QMetaObject::invokeMethod(this, [guard, done, rows = std::move(rows)]() mutable {
            if (guard)
                done(std::move(rows));
        }, Qt::QueuedConnection);
    });
}

void Store::ancestors(PageId id, QObject *ctx, std::function<void(QVector<PageMeta>)> done)
{
    QPointer<QObject> guard(ctx);
    m_reader->post([this, id, guard, done](sqlite3 *db) {
        sql::Stmt s(db,
            "WITH RECURSIVE up(id, depth) AS (SELECT ?1, 0 UNION ALL "
            "SELECT p.parent_id, up.depth + 1 FROM pages p JOIN up ON p.id = up.id "
            "WHERE p.parent_id <> 0 AND up.depth < 4096) "
            "SELECT " META_COLS " FROM up JOIN pages p ON p.id = up.id ORDER BY up.depth DESC");
        s.bind(1, id);
        QVector<PageMeta> rows;
        while (s.step() == SQLITE_ROW)
            rows.push_back(readMeta(s));
        QMetaObject::invokeMethod(this, [guard, done, rows = std::move(rows)]() mutable {
            if (guard)
                done(std::move(rows));
        }, Qt::QueuedConnection);
    });
}

void Store::searchTitles(const QString &query, int limit, QObject *ctx,
                         std::function<void(QVector<PageMeta>)> done)
{
    QPointer<QObject> guard(ctx);
    const quint64 gen = ++m_searchGen;
    m_reader->post([this, query, limit, guard, done, gen](sqlite3 *db) {
        if (gen != m_searchGen.load())
            return; // superseded before it started
        QString esc = query;
        esc.replace(QLatin1Char('\\'), QLatin1String("\\\\"))
            .replace(QLatin1Char('%'), QLatin1String("\\%"))
            .replace(QLatin1Char('_'), QLatin1String("\\_"));
        sql::Stmt s(db,
            "SELECT " META_COLS " FROM pages p WHERE p.trashed=0 AND p.title LIKE ?1 ESCAPE '\\' "
            "ORDER BY (p.title LIKE ?2 ESCAPE '\\') DESC, length(p.title), p.id LIMIT ?3");
        s.bind(1, QLatin1Char('%') + esc + QLatin1Char('%')).bind(2, esc + QLatin1Char('%')).bind(3, limit);
        QVector<PageMeta> rows;
        while (s.step() == SQLITE_ROW)
            rows.push_back(readMeta(s));
        if (gen != m_searchGen.load())
            return;
        QMetaObject::invokeMethod(this, [guard, done, rows = std::move(rows)]() mutable {
            if (guard)
                done(std::move(rows));
        }, Qt::QueuedConnection);
    });
}

void Store::recentPages(int limit, QObject *ctx, std::function<void(QVector<PageMeta>)> done)
{
    QPointer<QObject> guard(ctx);
    m_reader->post([this, limit, guard, done](sqlite3 *db) {
        sql::Stmt s(db, "SELECT " META_COLS " FROM pages p WHERE p.trashed=0 "
                        "ORDER BY p.updated_at DESC LIMIT ?1");
        s.bind(1, limit);
        QVector<PageMeta> rows;
        while (s.step() == SQLITE_ROW)
            rows.push_back(readMeta(s));
        QMetaObject::invokeMethod(this, [guard, done, rows = std::move(rows)]() mutable {
            if (guard)
                done(std::move(rows));
        }, Qt::QueuedConnection);
    });
}

void Store::siblingIndex(PageId id, QObject *ctx, std::function<void(PageId, int, int)> done)
{
    QPointer<QObject> guard(ctx);
    m_reader->post([this, id, guard, done](sqlite3 *db) {
        PageId parent = 0;
        int index = -1, count = 0;
        sql::Stmt p(db, "SELECT parent_id, ord FROM pages WHERE id=?1");
        p.bind(1, id);
        if (p.step() == SQLITE_ROW) {
            parent = p.i64(0);
            const qint64 ord = p.i64(1);
            sql::Stmt c(db, "SELECT SUM(ord < ?2), COUNT(*) FROM pages WHERE parent_id=?1 AND trashed=0");
            c.bind(1, parent).bind(2, ord);
            if (c.step() == SQLITE_ROW) {
                index = c.i32(0);
                count = c.i32(1);
            }
        }
        QMetaObject::invokeMethod(this, [guard, done, parent, index, count] {
            if (guard)
                done(parent, index, count);
        }, Qt::QueuedConnection);
    });
}

void Store::commit(SaveBatch batch)
{
    if (batch.page == 0 || batch.rev == 0)
        return;
    auto job = std::make_shared<Writer::Job>();
    job->type = Writer::Job::Save;
    job->batch = std::move(batch);
    m_writer->enqueue(std::move(job));
}

PageId Store::createPage(PageId parent, int index, const QString &title, bool linkInParent)
{
    auto job = std::make_shared<Writer::Job>();
    job->type = Writer::Job::Create;
    job->meta.id = allocatePageId();
    job->meta.parent = parent;
    job->meta.title = title;
    job->index = index;
    job->linkInParent = linkInParent;
    const PageId id = job->meta.id;
    m_writer->enqueue(std::move(job));
    return id;
}

void Store::movePage(PageId id, PageId newParent, int index, const QSet<PageId> &held, QObject *ctx,
                     std::function<void(MoveResult, PageId)> done)
{
    auto job = std::make_shared<Writer::Job>();
    job->type = Writer::Job::Move;
    job->id = id;
    job->newParent = newParent;
    job->index = index;
    job->held = held;
    job->ctx = ctx;
    job->moveDone = std::move(done);
    m_writer->enqueue(std::move(job));
}

void Store::trashPage(PageId id, const QSet<PageId> &held, QObject *ctx, std::function<void(bool, PageId)> done)
{
    auto job = std::make_shared<Writer::Job>();
    job->type = Writer::Job::Trash;
    job->id = id;
    job->held = held;
    job->ctx = ctx;
    job->trashDone = std::move(done);
    m_writer->enqueue(std::move(job));
}

void Store::writeJob(std::function<int(sqlite3 *)> fn, QObject *ctx, std::function<void()> done)
{
    if (!m_open)
        return; // closed (shutdown): nothing may reach the writer any more
    auto job = std::make_shared<Writer::Job>();
    job->type = Writer::Job::Custom;
    job->custom = std::move(fn);
    job->ctx = ctx;
    job->customDone = std::move(done);
    m_writer->enqueue(std::move(job));
}

void Store::putAttachment(const AttachmentInfo &a)
{
    writeJob([a](sqlite3 *db) {
        sql::Stmt s(db, "INSERT INTO attachments(hash, mime, bytes, width, height, orientation, pages, page_geom, "
                        "created_at) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9) ON CONFLICT(hash) DO UPDATE SET "
                        "mime=excluded.mime, bytes=excluded.bytes, width=excluded.width, height=excluded.height, "
                        "orientation=excluded.orientation, pages=excluded.pages, page_geom=excluded.page_geom");
        s.bind(1, a.hash).bind(2, a.mime).bind(3, a.bytes).bind(4, a.width).bind(5, a.height)
            .bind(6, a.orientation).bind(7, a.pages).bind(9, nowMs());
        if (a.pageGeom.isEmpty())
            s.bindNull(8);
        else
            s.bind(8, a.pageGeom);
        return s.exec() ? SQLITE_OK : s.lastError();
    });
}

void Store::attachmentInfo(const QStringList &hashes, QObject *ctx, std::function<void(QVector<AttachmentInfo>)> done)
{
    if (!m_open)
        return;
    QPointer<QObject> guard(ctx);
    m_reader->post([this, hashes, guard, done](sqlite3 *db) {
        QVector<AttachmentInfo> infos = queryAttachments(db, hashes);
        QMetaObject::invokeMethod(this, [guard, done, infos = std::move(infos)]() mutable {
            if (guard)
                done(std::move(infos));
        }, Qt::QueuedConnection);
    });
}

QString Store::setting(const QString &key, const QString &def) const
{
    return m_settings.value(key, def);
}

void Store::setSetting(const QString &key, const QString &value)
{
    if (m_settings.value(key) == value && m_settings.contains(key))
        return;
    m_settings.insert(key, value);
    auto job = std::make_shared<Writer::Job>();
    job->type = Writer::Job::Setting;
    job->key = key;
    job->value = value;
    m_writer->enqueue(std::move(job));
}

Store::Stats Store::stats() const
{
    Stats s;
    if (!m_writer)
        return s;
    s.commits = m_writer->commits.load();
    s.failures = m_writer->failures.load();
    s.coalesced = m_writer->coalesced.load();
    s.bytesWritten = m_writer->bytes.load();
    s.lastCommitMicros = m_writer->lastMicros.load();
    {
        std::lock_guard<std::mutex> lk(m_writer->mu);
        s.queued = qint64(m_writer->queue.size());
    }
    s.reads = m_reader->reads.load();
    return s;
}

} // namespace folia
