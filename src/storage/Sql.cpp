#include "storage/Sql.h"

namespace folia::sql {

Stmt::Stmt(sqlite3 *db, const char *sql) : m_db(db)
{
    m_last = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &m_stmt, nullptr);
    if (m_last != SQLITE_OK)
        m_stmt = nullptr;
}

Stmt::~Stmt()
{
    if (m_stmt)
        sqlite3_finalize(m_stmt);
}

Stmt &Stmt::operator=(Stmt &&o) noexcept
{
    if (this != &o) {
        if (m_stmt)
            sqlite3_finalize(m_stmt);
        m_db = o.m_db;
        m_stmt = o.m_stmt;
        o.m_stmt = nullptr;
    }
    return *this;
}

void Stmt::reset()
{
    if (!m_stmt)
        return;
    sqlite3_reset(m_stmt);
    sqlite3_clear_bindings(m_stmt);
}

Stmt &Stmt::bind(int i, qint64 v)
{
    sqlite3_bind_int64(m_stmt, i, v);
    return *this;
}

Stmt &Stmt::bind(int i, const QString &v)
{
    // UTF-16 in, SQLite stores UTF-8; SQLITE_TRANSIENT copies before return.
    sqlite3_bind_text16(m_stmt, i, v.utf16(), int(v.size() * 2), SQLITE_TRANSIENT);
    return *this;
}

Stmt &Stmt::bind(int i, const QByteArray &v)
{
    sqlite3_bind_blob(m_stmt, i, v.constData(), int(v.size()), SQLITE_TRANSIENT);
    return *this;
}

QByteArray Stmt::blob(int col) const
{
    const void *p = sqlite3_column_blob(m_stmt, col);
    const int n = sqlite3_column_bytes(m_stmt, col);
    return p && n > 0 ? QByteArray(static_cast<const char *>(p), n) : QByteArray();
}

Stmt &Stmt::bindNull(int i)
{
    sqlite3_bind_null(m_stmt, i);
    return *this;
}

int Stmt::step()
{
    if (!m_stmt)
        return m_last = SQLITE_MISUSE;
    m_last = sqlite3_step(m_stmt);
    return m_last;
}

bool Stmt::exec()
{
    const int rc = step();
    reset();
    m_last = rc;
    return rc == SQLITE_DONE;
}

QString Stmt::text(int col) const
{
    const void *p = sqlite3_column_text16(m_stmt, col);
    const int bytes = sqlite3_column_bytes16(m_stmt, col);
    if (!p)
        return QString();
    return QString(reinterpret_cast<const QChar *>(p), bytes / 2);
}

int execAll(sqlite3 *db, const char *sql, QString *error)
{
    char *msg = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &msg);
    if (rc != SQLITE_OK && error)
        *error = QString::fromUtf8(msg ? msg : sqlite3_errstr(rc));
    sqlite3_free(msg);
    return rc;
}

QString errorString(sqlite3 *db)
{
    return QString::fromUtf8(sqlite3_errmsg(db));
}

} // namespace folia::sql
