// Fault-injection shim over the platform's default SQLite VFS. Used by the
// recovery tests and the benchmark harness to simulate a full disk or I/O
// errors without filling the real disk. Pass-through (one indirect call per
// I/O) when no fault is armed.
#pragma once

namespace folia::faultvfs {

enum class Mode { None = 0, DiskFull = 1, IoError = 2 };

// Registers the "folia-fault" VFS (idempotent, thread-safe). Not the default.
const char *install();
// Arm a fault for subsequent xWrite/xTruncate/xSync calls on any file opened
// through this VFS. `afterWrites` lets that many writes succeed first.
void arm(Mode mode, int afterWrites = 0);
void disarm();
Mode mode();
long long injectedFailures();

} // namespace folia::faultvfs
