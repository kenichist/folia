#include "shell/LuaRuntime.h"

#include "core/Instrument.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <QDebug>
#include <cstdlib>
#include <cstring>

namespace folia {

namespace {
constexpr int kHookEvery = 1000;
constexpr int kMaxLogLines = 200;
} // namespace

LuaRuntime::LuaRuntime(const Limits &limits) : m_lim(limits)
{
    m_L = lua_newstate(&LuaRuntime::alloc, this, 0x5eed);
    if (!m_L)
        return;
    *static_cast<LuaRuntime **>(lua_getextraspace(m_L)) = this;
    openSandbox();
    lua_sethook(m_L, &LuaRuntime::hook, LUA_MASKCOUNT, kHookEvery);
}

LuaRuntime::~LuaRuntime()
{
    if (!m_L)
        return;
    // Closing runs any pending finalizers: keep them on a short budget too.
    m_instr = 0;
    m_tripped = false;
    m_lim.instructions = qMin<qint64>(m_lim.instructions, 1'000'000);
    m_lim.millis = qMin<qint64>(m_lim.millis, 50);
    m_clock.start();
    m_budgetActive = true;
    lua_close(m_L);
}

LuaRuntime *LuaRuntime::of(lua_State *L)
{
    return *static_cast<LuaRuntime **>(lua_getextraspace(L));
}

void *LuaRuntime::alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    auto *self = static_cast<LuaRuntime *>(ud);
    // When ptr is null, osize encodes the object type, not a size.
    const size_t old = ptr ? osize : 0;
    if (nsize == 0) {
        self->m_used -= old;
        std::free(ptr);
        return nullptr;
    }
    // The budget applies to shell code (inside call()), where a refused
    // allocation becomes a catchable Lua memory error. Host-side stack work
    // between calls is small and bounded and must never fail unprotected.
    if (self->m_budgetActive && nsize > old && self->m_used + (nsize - old) > self->m_lim.memoryBytes)
        return nullptr;
    void *p = std::realloc(ptr, nsize);
    if (!p)
        return nullptr;
    self->m_used = self->m_used - old + nsize;
    if (self->m_used > self->m_peak)
        self->m_peak = self->m_used;
    return p;
}

void LuaRuntime::hook(lua_State *L, lua_Debug *)
{
    LuaRuntime *self = of(L);
    if (!self->m_budgetActive)
        return;
    self->m_instr += kHookEvery;
    // Once tripped, a budget stays tripped for the rest of the invocation:
    // every later hook raises again, and the sandbox's pcall/xpcall/
    // coroutine.resume re-raise it, so shell code cannot swallow it.
    if (self->m_instr > self->m_lim.instructions && !self->m_tripped) {
        self->m_tripped = true;
        self->m_tripReason = QStringLiteral("instruction budget exceeded (%1)").arg(self->m_lim.instructions);
    }
    if (self->m_clock.isValid() && self->m_clock.elapsed() > self->m_lim.millis && !self->m_tripped) {
        self->m_tripped = true;
        self->m_tripReason = QStringLiteral("time budget exceeded (%1 ms)").arg(self->m_lim.millis);
    }
    if (self->m_tripped)
        luaL_error(L, "%s", self->m_tripReason.toUtf8().constData());
}

int LuaRuntime::print(lua_State *L)
{
    LuaRuntime *self = of(L);
    QString line;
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        size_t len = 0;
        const char *s = luaL_tolstring(L, i, &len);
        if (i > 1)
            line += QLatin1Char('\t');
        line += QString::fromUtf8(s, qsizetype(qMin<size_t>(len, 4096)));
        lua_pop(L, 1);
    }
    if (self->m_log.size() >= kMaxLogLines)
        self->m_log.removeFirst();
    self->m_log << line;
    qInfo().noquote() << "[shell]" << line;
    return 0;
}

int LuaRuntime::traceback(lua_State *L)
{
    const char *msg = lua_tostring(L, 1);
    if (!msg) {
        if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING)
            return 1;
        msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

void LuaRuntime::openSandbox()
{
    luaL_openselectedlibs(m_L, LUA_GLIBK | LUA_COLIBK | LUA_MATHLIBK | LUA_STRLIBK | LUA_TABLIBK | LUA_UTF8LIBK, 0);
    // Remove everything that reaches outside the sandbox or defeats budgets.
    static const char *removed[] = {"dofile", "loadfile", "load", "collectgarbage", "require", nullptr};
    for (const char **k = removed; *k; ++k) {
        lua_pushnil(m_L);
        lua_setglobal(m_L, *k);
    }
    lua_getglobal(m_L, "string");
    lua_pushnil(m_L);
    lua_setfield(m_L, -2, "dump");
    lua_pop(m_L, 1);
    lua_pushcfunction(m_L, &LuaRuntime::print);
    lua_setglobal(m_L, "print");
    // Sandbox guards, bound to locals captured before any shell code runs
    // (shadowing globals later cannot affect them):
    //  * no finalizers: a __gc metamethod could run shell code during a host
    //    allocation or lua_close, outside budgets (Lua only marks an object
    //    for finalization if __gc is present when setmetatable runs);
    //  * budget errors cannot be swallowed by pcall/xpcall/coroutine.resume.
    static const char *guard =
        "local tripped = ...\n"
        "local raw_setmetatable, raw_pcall, raw_xpcall, co_resume = setmetatable, pcall, xpcall, coroutine.resume\n"
        "local rawget, error, type = rawget, error, type\n"
        "setmetatable = function(t, mt)\n"
        "  if type(mt) == 'table' and rawget(mt, '__gc') ~= nil then\n"
        "    error('__gc metamethods are not allowed in shell code', 2)\n"
        "  end\n"
        "  return raw_setmetatable(t, mt)\n"
        "end\n"
        "local function rethrow(ok, ...)\n"
        "  if not ok and tripped() then error((...), 0) end\n"
        "  return ok, ...\n"
        "end\n"
        "pcall = function(f, ...) return rethrow(raw_pcall(f, ...)) end\n"
        "xpcall = function(f, h, ...) return rethrow(raw_xpcall(f, h, ...)) end\n"
        "coroutine.resume = function(co, ...) return rethrow(co_resume(co, ...)) end\n";
    if (luaL_loadbufferx(m_L, guard, strlen(guard), "=sandbox", "t") == LUA_OK) {
        lua_pushcfunction(m_L, &LuaRuntime::trippedFn);
        lua_pcall(m_L, 1, 0, 0);
    } else {
        lua_pop(m_L, 1);
    }
}

int LuaRuntime::trippedFn(lua_State *L)
{
    lua_pushboolean(L, of(L)->m_tripped);
    return 1;
}

bool LuaRuntime::consumeHostCall()
{
    return ++m_hostCalls <= m_lim.hostCalls;
}

QStringList LuaRuntime::takeLog()
{
    QStringList out;
    out.swap(m_log);
    return out;
}

bool LuaRuntime::run(const QByteArray &code, const QString &chunkName, QString *error)
{
    const QByteArray name = "@" + chunkName.toUtf8();
    const int rc = luaL_loadbufferx(m_L, code.constData(), size_t(code.size()), name.constData(), "t");
    if (rc != LUA_OK) {
        if (error)
            *error = QString::fromUtf8(lua_tostring(m_L, -1));
        lua_pop(m_L, 1);
        return false;
    }
    return call(0, 1, error);
}

bool LuaRuntime::call(int nargs, int nresults, QString *error)
{
    instr::add(instr::LuaInvocations);
    const bool outer = m_depth == 0;
    if (outer) {
        m_instr = 0;
        m_hostCalls = 0;
        m_tripped = false;
        m_clock.start();
        m_budgetActive = true;
    }
    ++m_depth;
    const int base = lua_gettop(m_L) - nargs; // function index
    lua_pushcfunction(m_L, &LuaRuntime::traceback);
    lua_insert(m_L, base);
    const int rc = lua_pcall(m_L, nargs, nresults, base);
    lua_remove(m_L, base);
    --m_depth;
    if (outer) {
        m_budgetActive = false;
        m_clock.invalidate();
    }
    if (rc != LUA_OK) {
        instr::add(instr::LuaErrors);
        QString msg = rc == LUA_ERRMEM ? QStringLiteral("memory budget exceeded (%1 MB)").arg(m_lim.memoryBytes >> 20)
                                       : QString::fromUtf8(lua_tostring(m_L, -1));
        if (error)
            *error = msg;
        lua_pop(m_L, 1);
        // Return memory to the allocator after a failed (possibly huge) call.
        lua_gc(m_L, LUA_GCCOLLECT);
        return false;
    }
    return true;
}

} // namespace folia
