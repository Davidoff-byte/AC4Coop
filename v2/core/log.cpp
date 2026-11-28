#include "log.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <process.h>

namespace v2::log {
namespace {

constexpr uint32_t kCells = 8192;          // power of two
constexpr uint32_t kLine  = 480;           // bytes per line (longer lines are cut)
static_assert((kCells & (kCells - 1)) == 0, "kCells must be a power of two");

struct Cell {
    std::atomic<uint32_t> seq;
    uint32_t tid;
    uint32_t ms;
    uint8_t  level;
    char     text[kLine];
};

Cell*                 g_cells = nullptr;
std::atomic<uint32_t> g_enq{0};
uint32_t              g_deq = 0;            // writer thread only
std::atomic<uint32_t> g_dropped{0};
std::atomic<int>      g_min{kInfo};
HANDLE                g_file = INVALID_HANDLE_VALUE;
HANDLE                g_wake = nullptr;
CRITICAL_SECTION      g_flushLock;          // writer thread vs Flush(); never taken by producers
uint64_t              g_t0 = 0;
volatile LONG         g_running = 0;

const char kLevelChar[4] = { 'D', 'I', 'W', 'E' };

uint32_t NowMs() { return (uint32_t)(GetTickCount64() - g_t0); }

// Drains every ready cell to the file. Writer thread or Flush() (both under g_flushLock).
void DrainLocked() {
    char buf[kLine + 48];
    for (;;) {
        Cell& c = g_cells[g_deq & (kCells - 1)];
        const uint32_t seq = c.seq.load(std::memory_order_acquire);
        if (seq != g_deq + 1) return;                         // not written yet (or empty)
        const int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%7u.%03u %5u %c %s", c.ms / 1000, c.ms % 1000,
                                  c.tid, kLevelChar[c.level & 3], c.text);
        c.seq.store(g_deq + kCells, std::memory_order_release); // free the cell for the producer one lap ahead
        ++g_deq;
        if (n > 0 && g_file != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(g_file, buf, (DWORD)strlen(buf), &w, nullptr);
        }
    }
}

unsigned __stdcall WriterThread(void*) {
    while (InterlockedCompareExchange(&g_running, 1, 1) == 1) {
        WaitForSingleObject(g_wake, 100);
        EnterCriticalSection(&g_flushLock);
        DrainLocked();
        LeaveCriticalSection(&g_flushLock);
    }
    return 0;
}

}  // namespace

void Start(const char* path, Level minLevel) {
    if (g_cells) return;
    g_t0 = GetTickCount64();
    g_min.store(minLevel);
    g_cells = (Cell*)VirtualAlloc(nullptr, sizeof(Cell) * kCells, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_cells) return;
    for (uint32_t i = 0; i < kCells; ++i) g_cells[i].seq.store(i, std::memory_order_relaxed);
    InitializeCriticalSection(&g_flushLock);
    g_file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    InterlockedExchange(&g_running, 1);
    _beginthreadex(nullptr, 0, WriterThread, nullptr, 0, nullptr);
}

void SetLevel(Level minLevel) { g_min.store(minLevel); }

void WriteV(Level lv, const char* fmt, va_list ap) {
    if (!g_cells || (int)lv < g_min.load(std::memory_order_relaxed)) return;
    uint32_t pos = g_enq.load(std::memory_order_relaxed);
    Cell* c = nullptr;
    for (;;) {
        c = &g_cells[pos & (kCells - 1)];
        const uint32_t seq = c->seq.load(std::memory_order_acquire);
        const int32_t dif = (int32_t)(seq - pos);
        if (dif == 0) {
            if (g_enq.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
        } else if (dif < 0) {
            g_dropped.fetch_add(1, std::memory_order_relaxed);  // full: the writer is a lap behind
            return;
        } else {
            pos = g_enq.load(std::memory_order_relaxed);
        }
    }
    c->tid = GetCurrentThreadId();
    c->ms = NowMs();
    c->level = (uint8_t)lv;
    const int n = _vsnprintf_s(c->text, kLine, _TRUNCATE, fmt, ap);
    if (n < 0) { c->text[kLine - 2] = '\n'; c->text[kLine - 1] = 0; }   // cut: keep the line terminated
    c->seq.store(pos + 1, std::memory_order_release);
    if (g_wake && lv >= kWarn) SetEvent(g_wake);
}

void Write(Level lv, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); WriteV(lv, fmt, ap); va_end(ap);
}

void Flush() {
    if (!g_cells) return;
    EnterCriticalSection(&g_flushLock);
    DrainLocked();
    if (g_file != INVALID_HANDLE_VALUE) FlushFileBuffers(g_file);
    LeaveCriticalSection(&g_flushLock);
}

uint32_t Dropped() { return g_dropped.load(std::memory_order_relaxed); }

}  // namespace v2::log
