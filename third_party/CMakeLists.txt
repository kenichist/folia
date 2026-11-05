# Vendored, checksum-verified upstream sources (see third_party/README.md).

# --- Lua 5.5.1 (MIT) ---------------------------------------------------------
# The interpreter/compiler front-ends (lua.c, luac.c) are excluded. Libraries
# that touch the OS (io, os, package, debug) are compiled but never opened in
# the UI sandbox (see src/shell/LuaRuntime.cpp).
file(GLOB FOLIA_LUA_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/lua/src/*.c)
list(REMOVE_ITEM FOLIA_LUA_SOURCES
     ${CMAKE_CURRENT_SOURCE_DIR}/lua/src/lua.c
     ${CMAKE_CURRENT_SOURCE_DIR}/lua/src/luac.c
     ${CMAKE_CURRENT_SOURCE_DIR}/lua/src/onelua.c)
add_library(folia_lua STATIC ${FOLIA_LUA_SOURCES})
target_include_directories(folia_lua PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/lua/src)
set_target_properties(folia_lua PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
if(APPLE)
  target_compile_definitions(folia_lua PRIVATE LUA_USE_MACOSX)
elseif(UNIX)
  target_compile_definitions(folia_lua PRIVATE LUA_USE_LINUX)
endif()

# --- SQLite 3.54.1 amalgamation (public domain) ------------------------------
add_library(folia_sqlite STATIC ${CMAKE_CURRENT_SOURCE_DIR}/sqlite/sqlite3.c)
target_include_directories(folia_sqlite PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sqlite)
set_target_properties(folia_sqlite PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(folia_sqlite PRIVATE
  SQLITE_THREADSAFE=2            # connections are never shared across threads
  SQLITE_DQS=0
  SQLITE_DEFAULT_MEMSTATUS=0
  SQLITE_DEFAULT_WAL_SYNCHRONOUS=2
  SQLITE_LIKE_DOESNT_MATCH_BLOBS
  SQLITE_MAX_EXPR_DEPTH=0
  SQLITE_OMIT_DEPRECATED
  SQLITE_OMIT_SHARED_CACHE
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_USE_ALLOCA
  SQLITE_ENABLE_FTS5)
find_package(Threads REQUIRED)
target_link_libraries(folia_sqlite PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
