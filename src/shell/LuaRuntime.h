// Sandboxed Lua state for shell code.
//  * Libraries: base (minus dofile/loadfile/load/collectgarbage/require),
//    string (minus dump), table, math, utf8, coroutine. No io, os, package,
//    debug: no disk or network access from the UI language.
//  * Budgets per invocation: allocated bytes (custom allocator; a failed
//    allocation becomes a Lua memory error), VM instructions and wall-clock
//    time (count hook), and host calls. Exceeding any budget aborts that
//    invocation with an error; the host keeps the last valid layout.
#pragma once

#include <QElapsedTimer>
#include <QString>
#include <QStringList>

struct lua_State;
struct lua_Debug;

namespace folia {

class LuaRuntime {
public:
    struct Limits {
        size_t memoryBytes = 24u << 20;
        qint64 instructions = 5'000'000;
        qint64 millis = 250;
        int hostCalls = 20000;
    };

    LuaRuntime() : LuaRuntime(Limits()) {}
    explicit LuaRuntime(const Limits &limits);
    ~LuaRuntime();
    LuaRuntime(const LuaRuntime &) = delete;
    LuaRuntime &operator=(const LuaRuntime &) = delete;

    lua_State *L() const { return m_L; }
    bool valid() const { return m_L != nullptr; }
    const Limits &limits() const { return m_lim; }
    void setLimits(const Limits &l) { m_lim = l; }

    // Compile and run `code`; on success its first result is left on the stack.
    bool run(const QByteArray &code, const QString &chunkName, QString *error);
    // Protected call of the function below `nargs` arguments, with budgets.
    bool call(int nargs, int nresults, QString *error);

    // Host-call budget for C functions exposed to Lua.
    bool consumeHostCall();
    size_t memoryUsed() const { return m_used; }
    size_t memoryPeak() const { return m_peak; }
    qint64 lastInstructions() const { return m_instr; }
    QStringList takeLog();

    static LuaRuntime *of(lua_State *L);

private:
    static void *alloc(void *ud, void *ptr, size_t osize, size_t nsize);
    static void hook(lua_State *L, lua_Debug *ar);
    static int print(lua_State *L);
    static int traceback(lua_State *L);
    static int trippedFn(lua_State *L);
    void openSandbox();

    lua_State *m_L = nullptr;
    Limits m_lim;
    size_t m_used = 0;
    size_t m_peak = 0;
    qint64 m_instr = 0;
    int m_hostCalls = 0;
    int m_depth = 0;
    bool m_budgetActive = false;
    bool m_tripped = false;
    QString m_tripReason;
    QElapsedTimer m_clock;
    QStringList m_log;
};

} // namespace folia
