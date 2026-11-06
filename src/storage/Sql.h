// Minimal RAII helpers over the SQLite C API. Each Db is owned by exactly one
// thread (SQLITE_THREADSAFE=2).
#pragma once

#include <QByteArray>
#include <QString>
#include <sqlite3.h>

namespace folia::sql {

class Stmt {
public:
    Stmt() = default;
    Stmt(sqlite3 *db, const char *sql);
    ~Stmt();
    Stmt(const Stmt &) = delete;
    Stmt &operator=(const Stmt &) = delete;
    Stmt(Stmt &&o) noexcept : m_db(o.m_db), m_stmt(o.m_stmt) { o.m_stmt = nullptr; }
    Stmt &operator=(Stmt &&o) noexcept;

    bool valid() const { return m_stmt != nullptr; }
    void reset();
    Stmt &bind(int i, qint64 v);
    Stmt &bind(int i, int v) { return bind(i, qint64(v)); }
    Stmt &bind(int i, const QString &v);
    Stmt &bind(int i, const QByteArray &v); // BLOB
    Stmt &bindNull(int i);
    // SQLITE_ROW / SQLITE_DONE / error code
    int step();
    // step() and return true iff SQLITE_DONE; the error code is kept in lastError
    bool exec();
    int lastError() const { return m_last; }

    qint64 i64(int col) const { return sqlite3_column_int64(m_stmt, col); }
    int i32(int col) const { return sqlite3_column_int(m_stmt, col); }
    bool isNull(int col) const { return sqlite3_column_type(m_stmt, col) == SQLITE_NULL; }
    QString text(int col) const;
    QByteArray blob(int col) const;

private:
    sqlite3 *m_db = nullptr;
    sqlite3_stmt *m_stmt = nullptr;
    int m_last = SQLITE_OK;
};

// Execute one or more statements without results. Returns SQLite code.
int execAll(sqlite3 *db, const char *sql, QString *error = nullptr);
QString errorString(sqlite3 *db);

} // namespace folia::sql
