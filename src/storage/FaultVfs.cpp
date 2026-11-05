#include "storage/FaultVfs.h"

#include <sqlite3.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace folia::faultvfs {
namespace {

std::atomic<int> g_mode{0};
std::atomic<int> g_writesBeforeFail{0};
std::atomic<long long> g_failures{0};
sqlite3_vfs g_vfs;
sqlite3_vfs *g_real = nullptr;
std::once_flag g_once;

struct FaultFile {
    sqlite3_file base;
    sqlite3_file *real; // points just past this struct
};

sqlite3_file *real(sqlite3_file *f) { return reinterpret_cast<FaultFile *>(f)->real; }

int shouldFail()
{
    const int m = g_mode.load(std::memory_order_relaxed);
    if (m == 0)
        return SQLITE_OK;
    if (g_writesBeforeFail.fetch_sub(1) > 0)
        return SQLITE_OK;
    g_failures.fetch_add(1);
    return m == int(Mode::DiskFull) ? SQLITE_FULL : SQLITE_IOERR_WRITE;
}

int xClose(sqlite3_file *f)
{
    sqlite3_file *r = real(f);
    const int rc = r->pMethods ? r->pMethods->xClose(r) : SQLITE_OK;
    return rc;
}
int xRead(sqlite3_file *f, void *buf, int n, sqlite3_int64 off) { return real(f)->pMethods->xRead(real(f), buf, n, off); }
int xWrite(sqlite3_file *f, const void *buf, int n, sqlite3_int64 off)
{
    if (int rc = shouldFail())
        return rc;
    return real(f)->pMethods->xWrite(real(f), buf, n, off);
}
int xTruncate(sqlite3_file *f, sqlite3_int64 size)
{
    if (int rc = shouldFail())
        return rc == SQLITE_FULL ? SQLITE_FULL : SQLITE_IOERR_TRUNCATE;
    return real(f)->pMethods->xTruncate(real(f), size);
}
int xSync(sqlite3_file *f, int flags)
{
    if (g_mode.load() == int(Mode::IoError) && g_writesBeforeFail.load() <= 0) {
        g_failures.fetch_add(1);
        return SQLITE_IOERR_FSYNC;
    }
    return real(f)->pMethods->xSync(real(f), flags);
}
int xFileSize(sqlite3_file *f, sqlite3_int64 *s) { return real(f)->pMethods->xFileSize(real(f), s); }
int xLock(sqlite3_file *f, int l) { return real(f)->pMethods->xLock(real(f), l); }
int xUnlock(sqlite3_file *f, int l) { return real(f)->pMethods->xUnlock(real(f), l); }
int xCheckReservedLock(sqlite3_file *f, int *o) { return real(f)->pMethods->xCheckReservedLock(real(f), o); }
int xFileControl(sqlite3_file *f, int op, void *a) { return real(f)->pMethods->xFileControl(real(f), op, a); }
int xSectorSize(sqlite3_file *f) { return real(f)->pMethods->xSectorSize(real(f)); }
int xDeviceCharacteristics(sqlite3_file *f) { return real(f)->pMethods->xDeviceCharacteristics(real(f)); }
int xShmMap(sqlite3_file *f, int pg, int sz, int ext, void volatile **pp) { return real(f)->pMethods->xShmMap(real(f), pg, sz, ext, pp); }
int xShmLock(sqlite3_file *f, int off, int n, int flags) { return real(f)->pMethods->xShmLock(real(f), off, n, flags); }
void xShmBarrier(sqlite3_file *f) { real(f)->pMethods->xShmBarrier(real(f)); }
int xShmUnmap(sqlite3_file *f, int del) { return real(f)->pMethods->xShmUnmap(real(f), del); }
int xFetch(sqlite3_file *f, sqlite3_int64 off, int n, void **pp) { return real(f)->pMethods->xFetch(real(f), off, n, pp); }
int xUnfetch(sqlite3_file *f, sqlite3_int64 off, void *p) { return real(f)->pMethods->xUnfetch(real(f), off, p); }

const sqlite3_io_methods g_methods = {
    3, xClose, xRead, xWrite, xTruncate, xSync, xFileSize, xLock, xUnlock, xCheckReservedLock,
    xFileControl, xSectorSize, xDeviceCharacteristics, xShmMap, xShmLock, xShmBarrier, xShmUnmap,
    xFetch, xUnfetch,
};

int vOpen(sqlite3_vfs *, sqlite3_filename name, sqlite3_file *f, int flags, int *outFlags)
{
    auto *ff = reinterpret_cast<FaultFile *>(f);
    ff->real = reinterpret_cast<sqlite3_file *>(ff + 1);
    std::memset(ff->real, 0, size_t(g_real->szOsFile));
    const int rc = g_real->xOpen(g_real, name, ff->real, flags, outFlags);
    // Only install our methods when the real open succeeded (SQLite will not
    // call xClose otherwise).
    ff->base.pMethods = (rc == SQLITE_OK && ff->real->pMethods) ? &g_methods : nullptr;
    return rc;
}
int vDelete(sqlite3_vfs *, const char *n, int sync) { return g_real->xDelete(g_real, n, sync); }
int vAccess(sqlite3_vfs *, const char *n, int flags, int *res) { return g_real->xAccess(g_real, n, flags, res); }
int vFullPathname(sqlite3_vfs *, const char *n, int len, char *out) { return g_real->xFullPathname(g_real, n, len, out); }
void *vDlOpen(sqlite3_vfs *, const char *n) { return g_real->xDlOpen(g_real, n); }
void vDlError(sqlite3_vfs *, int n, char *m) { g_real->xDlError(g_real, n, m); }
void (*vDlSym(sqlite3_vfs *, void *h, const char *s))(void) { return g_real->xDlSym(g_real, h, s); }
void vDlClose(sqlite3_vfs *, void *h) { g_real->xDlClose(g_real, h); }
int vRandomness(sqlite3_vfs *, int n, char *out) { return g_real->xRandomness(g_real, n, out); }
int vSleep(sqlite3_vfs *, int us) { return g_real->xSleep(g_real, us); }
int vCurrentTime(sqlite3_vfs *, double *t) { return g_real->xCurrentTime(g_real, t); }
int vGetLastError(sqlite3_vfs *, int n, char *m) { return g_real->xGetLastError ? g_real->xGetLastError(g_real, n, m) : 0; }
int vCurrentTimeInt64(sqlite3_vfs *, sqlite3_int64 *t) { return g_real->xCurrentTimeInt64(g_real, t); }

} // namespace

const char *install()
{
    std::call_once(g_once, [] {
        g_real = sqlite3_vfs_find(nullptr);
        std::memset(&g_vfs, 0, sizeof g_vfs);
        g_vfs.iVersion = 2;
        g_vfs.szOsFile = int(sizeof(FaultFile)) + g_real->szOsFile;
        g_vfs.mxPathname = g_real->mxPathname;
        g_vfs.zName = "folia-fault";
        g_vfs.xOpen = vOpen;
        g_vfs.xDelete = vDelete;
        g_vfs.xAccess = vAccess;
        g_vfs.xFullPathname = vFullPathname;
        g_vfs.xDlOpen = vDlOpen;
        g_vfs.xDlError = vDlError;
        g_vfs.xDlSym = vDlSym;
        g_vfs.xDlClose = vDlClose;
        g_vfs.xRandomness = vRandomness;
        g_vfs.xSleep = vSleep;
        g_vfs.xCurrentTime = vCurrentTime;
        g_vfs.xGetLastError = vGetLastError;
        g_vfs.xCurrentTimeInt64 = vCurrentTimeInt64;
        sqlite3_vfs_register(&g_vfs, 0);
    });
    return g_vfs.zName;
}

void arm(Mode m, int afterWrites)
{
    g_writesBeforeFail.store(afterWrites);
    g_mode.store(int(m));
}

void disarm()
{
    g_mode.store(0);
}

Mode mode() { return Mode(g_mode.load()); }
long long injectedFailures() { return g_failures.load(); }

} // namespace folia::faultvfs
