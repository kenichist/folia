# Vendored third-party sources

| Library | Version | License | Upstream URL | SHA-256 of upstream tarball |
|---|---|---|---|---|
| Lua | 5.5.1 | MIT | https://www.lua.org/ftp/lua-5.5.1.tar.gz | `1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce` |
| SQLite (amalgamation) | 3.54.1 | Public domain | https://www.sqlite.org/2026/sqlite-autoconf-3540100.tar.gz | `926ef8d3eab93fbf6e06011901273eec076b25594a4e1cdadd50c987f4f6d64e` |
| miniz | 3.1.2 | MIT | https://github.com/richgel999/miniz/archive/refs/tags/3.1.2.tar.gz | `98468f8924934b723276680f85238b6c78bf1f8b49b4459cc9b7214a20e2e9fb` |

Both tarballs were fetched through Homebrew (`brew fetch --build-from-source lua sqlite`),
which verifies the formula checksums above, then unpacked (one local patch, listed below). `lua/doc` was
removed; only `sqlite3.c`, `sqlite3.h` and `sqlite3ext.h` were kept from SQLite.
miniz was fetched the same way (`brew fetch --build-from-source miniz`, 2026-11-09).
Only the zip-reading and inflate sources were kept (`miniz.c`, `miniz_tinfl.c`,
`miniz_zip.c`, their headers, `miniz_tdef.h` which `miniz.h` includes, and
`LICENSE`); it reads uploaded Word files (docs/ATTACHMENTS.md §8) and is built
without compression, zip writing, stdio or time (`third_party/CMakeLists.txt`).
`miniz/miniz_export.h` is ours: upstream generates it with CMake, and a static
build exports nothing.

Vendoring (instead of linking the system copy) keeps the same Lua and SQLite
build on macOS, Windows and Linux, and lets us pin compile options
(`third_party/CMakeLists.txt`).

## Local modifications

- `lua/src/lstrlib.c`: the pattern matcher counts `match()` steps per call
  (`FOLIA_MAXMATCHSTEPS`, default 4,000,000; reset per `gmatch` iteration) and
  raises "pattern too complex (step limit)" beyond it. Backtracking runs in C,
  where the VM instruction hook used for the shell's budgets cannot interrupt
  it. Search for `FOLIA` in that file to see every changed line.

## Embedded icon data

`src/core/LucideIcons.cpp` embeds the SVG element markup of a few
[Lucide](https://lucide.dev) icons, copied verbatim from
`lucide-static@0.469.0` (https://unpkg.com/lucide-static@0.469.0/icons/).
Lucide is ISC licensed: copyright (c) for portions of Lucide are held by Cole
Bemis 2013-2022 as part of Feather (MIT); all other copyright (c) for Lucide
are held by Lucide Contributors 2022. To add an icon, paste its elements into
the table in that file.
