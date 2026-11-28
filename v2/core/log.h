#pragma once
// v2 logger (docs/v2/ARCHITECTURE.md s8): any thread formats into a bounded lock-free queue (Vyukov MPMC cells, single
// consumer); one writer thread appends to the file. Never blocks the caller and never allocates after Start; a full
// queue drops the line and counts it. File: <log dir>\phase1_log_<pid>.txt (the name the tools read; first line says v2).
#include <cstdarg>
#include <cstdint>

namespace v2::log {

enum Level : uint8_t { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

void     Start(const char* path, Level minLevel);   // opens the file, starts the writer thread
void     SetLevel(Level minLevel);
void     Write(Level lv, const char* fmt, ...);
void     WriteV(Level lv, const char* fmt, va_list ap);
void     Flush();                                   // drain synchronously (crash handler, shutdown)
uint32_t Dropped();

}  // namespace v2::log

#define LOGD(...) ::v2::log::Write(::v2::log::kDebug, __VA_ARGS__)
#define LOGI(...) ::v2::log::Write(::v2::log::kInfo, __VA_ARGS__)
#define LOGW(...) ::v2::log::Write(::v2::log::kWarn, __VA_ARGS__)
#define LOGE(...) ::v2::log::Write(::v2::log::kError, __VA_ARGS__)
