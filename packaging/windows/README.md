# Windows build (not yet built or measured — no Windows machine was available)

Status: **source and commands only; unmeasured.** The code base is portable
(Qt 6 Widgets, vendored Lua and SQLite, `PlatformPortable.cpp` for process
counters via `GetProcessMemoryInfo`/`GetProcessTimes`), but it has not been
compiled with MSVC yet, so expect small fixes on first build.

Prerequisites (reference machine to be named when acceptance runs):
- Windows 11 23H2+ x64 (and/or ARM64), Visual Studio 2022 17.10+ (Desktop C++),
  CMake ≥ 3.21, Ninja, Qt 6.8+ for MSVC 2022 64-bit (online installer or
  `aqt install-qt windows desktop 6.8.3 win64_msvc2022_64`).

From a "x64 Native Tools Command Prompt for VS 2022":

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
cmake --build build
ctest --test-dir build --output-on-failure
mkdir dist && copy build\src\Folia.exe dist\ && xcopy /E /I presets dist\presets
C:\Qt\6.8.3\msvc2022_64\bin\windeployqt.exe --release --no-translations --no-opengl-sw dist\Folia.exe
```

Benchmarks: run `dist\Folia.exe --bench <scenario> ...` exactly as on macOS
(see docs/BENCHMARKS.md). The footprint counter reported on Windows is
`PROCESS_MEMORY_COUNTERS_EX.PrivateUsage`; idle wakeups are not available
from this counter set (report as unmeasured, or use ETW/WPA).
