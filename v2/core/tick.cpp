#include "tick.h"
#include "clock.h"
#include "hooks.h"
#include "log.h"
#include "../engine/world.h"

#include <windows.h>
#include <atomic>
#include <cstring>

namespace v2::tick {
namespace {

// Human update site (v1 phase1.cpp RVA_HumanUpdate 0x12FAACD, 6 stolen bytes = mov ecx,[esi+108h]; esi = Human).
constexpr uintptr_t kHumanUpdateVa = 0x016FAACD;
const uint8_t       kHumanUpdateBytes[6] = { 0x8B, 0x8E, 0x08, 0x01, 0x00, 0x00 };
uintptr_t           g_resume = 0;

struct Module { const char* name; int order; TickFn fn; };
struct Own    { const char* name; OwnUpdateFn fn; };
Module g_mods[32]; int g_modN = 0;
Own    g_own[16];  int g_ownN = 0;

// Command queue: bounded MPSC (Vyukov cells), drained at the start of the tick.
struct Cmd { std::atomic<uint32_t> seq; CmdFn fn; uint32_t size; uint8_t args[56]; };
constexpr uint32_t kCmds = 256;
Cmd                   g_cmd[kCmds];
std::atomic<uint32_t> g_cmdEnq{0};
uint32_t              g_cmdDeq = 0;   // tick thread only
std::atomic<uint32_t> g_cmdDropped{0};

std::atomic<void*>    g_player{nullptr};      // the player's Human (cached; validated in the tick)
std::atomic<int>      g_inTick{0};
std::atomic<uint64_t> g_lastTickUs{0};
std::atomic<uint64_t> g_ticks{0};
std::atomic<int>      g_inWorld{0};
std::atomic<uint64_t> g_lastResolveUs{0};
std::atomic<int>      g_fallback{0};

// stats (tick thread)
uint64_t g_statT0 = 0, g_statTicks = 0, g_statFallback = 0, g_statMaxUs = 0;

void DrainCommands() {
    for (;;) {
        Cmd& c = g_cmd[g_cmdDeq & (kCmds - 1)];
        if (c.seq.load(std::memory_order_acquire) != g_cmdDeq + 1) return;
        const CmdFn fn = c.fn;
        uint8_t args[56]; memcpy(args, c.args, c.size);
        c.seq.store(g_cmdDeq + kCmds, std::memory_order_release);
        ++g_cmdDeq;
        fn(args);                     // engine work, no SEH (R2)
    }
}

void RunTick(void* human, bool fallback) {
    int expected = 0;
    if (!g_inTick.compare_exchange_strong(expected, 1)) return;   // one tick at a time
    const uint64_t t0 = clock::NowUs();
    // Validate the cached player every 500 ms (respawn / reload replaces the Human).
    if (t0 - g_lastResolveUs.load() > 500000) {
        g_lastResolveUs.store(t0);
        void* ph = engine::LocalPlayerHuman();
        g_player.store(ph);
        const int now = ph ? 1 : 0;
        if (now != g_inWorld.load()) LOGI("[Tick] %s world (player human %p)\n", now ? "in" : "left", ph);
        g_inWorld.store(now);
        if (!ph || ph != human) { g_inTick.store(0); return; }
    }
    if (fallback != (g_fallback.load() != 0)) {
        g_fallback.store(fallback ? 1 : 0);
        LOGW("[Tick] fallback %s (player update %s the hook)\n", fallback ? "on" : "off", fallback ? "stopped reaching" : "reaches");
    }
    DrainCommands();
    for (int i = 0; i < g_modN; ++i) g_mods[i].fn(human, t0);   // fixed order, no SEH (R2)
    const uint64_t t1 = clock::NowUs();
    g_lastTickUs.store(t1);
    g_ticks.fetch_add(1);
    // 5 s stats
    ++g_statTicks; if (fallback) ++g_statFallback;
    if (t1 - t0 > g_statMaxUs) g_statMaxUs = t1 - t0;
    if (!g_statT0) g_statT0 = t1;
    if (t1 - g_statT0 >= 5000000) {
        const double s = (double)(t1 - g_statT0) / 1e6;
        LOGI("[Stats] tick hz=%.1f fallback=%llu maxUs=%llu cmdDropped=%u logDropped=%u\n", g_statTicks / s,
             (unsigned long long)g_statFallback, (unsigned long long)g_statMaxUs, g_cmdDropped.load(), log::Dropped());
        g_statT0 = t1; g_statTicks = g_statFallback = g_statMaxUs = 0;
    }
    g_inTick.store(0);
}

void __cdecl OnHumanUpdate(void* human) {
    void* player = g_player.load(std::memory_order_relaxed);
    if (human && human == player) { RunTick(human, false); return; }
    const uint64_t now = clock::NowUs();
    if (!player) {
        // Find the player: read-only engine getters, at most every 250 ms across all threads.
        const uint64_t last = g_lastResolveUs.load();
        if (now - last < 250000) return;
        uint64_t exp = last;
        if (!g_lastResolveUs.compare_exchange_strong(exp, now)) return;
        void* ph = engine::LocalPlayerHuman();
        g_player.store(ph);
        if (ph && ph == human) RunTick(human, false);
        return;
    }
    // Fallback: the player's update has not reached the hook for 100 ms (seen at the helm, v1 b52ce83).
    const uint64_t lt = g_lastTickUs.load();
    if (lt && now - lt > 100000) RunTick(player, true);
    for (int i = 0; i < g_ownN; ++i) g_own[i].fn(human);
}

__declspec(naked) void HumanUpdateDetour() {
    __asm {
        pushad
        pushfd
        push esi
        call OnHumanUpdate
        add  esp, 4
        popfd
        popad
        mov  ecx, [esi + 0x108]     // the stolen instruction
        jmp  dword ptr [g_resume]
    }
}

}  // namespace

bool Install() {
    for (uint32_t i = 0; i < kCmds; ++i) g_cmd[i].seq.store(i, std::memory_order_relaxed);
    return hooks::JmpDetour("tick", "HumanUpdate", kHumanUpdateVa, kHumanUpdateBytes, sizeof(kHumanUpdateBytes),
                            (void*)&HumanUpdateDetour, &g_resume);
}

void Register(const char* name, int order, TickFn fn) {
    if (g_modN >= 32) return;
    int i = g_modN++;
    while (i > 0 && g_mods[i - 1].order > order) { g_mods[i] = g_mods[i - 1]; --i; }
    g_mods[i] = { name, order, fn };
    LOGI("[Tick] module %s order %d\n", name, order);
}

void RegisterOwnUpdate(const char* name, OwnUpdateFn fn) {
    if (g_ownN >= 16) return;
    g_own[g_ownN++] = { name, fn };
    LOGI("[Tick] own-update %s\n", name);
}

bool Post(CmdFn fn, const void* args, uint32_t size) {
    if (!fn || size > sizeof(Cmd::args)) return false;
    uint32_t pos = g_cmdEnq.load(std::memory_order_relaxed);
    Cmd* c = nullptr;
    for (;;) {
        c = &g_cmd[pos & (kCmds - 1)];
        const int32_t dif = (int32_t)(c->seq.load(std::memory_order_acquire) - pos);
        if (dif == 0) { if (g_cmdEnq.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break; }
        else if (dif < 0) { g_cmdDropped.fetch_add(1); return false; }
        else pos = g_cmdEnq.load(std::memory_order_relaxed);
    }
    c->fn = fn; c->size = size;
    if (size) memcpy(c->args, args, size);
    c->seq.store(pos + 1, std::memory_order_release);
    return true;
}

bool     InWorld() { return g_inWorld.load() != 0; }
void*    PlayerHuman() { return g_player.load(); }
uint64_t TickCount() { return g_ticks.load(); }

}  // namespace v2::tick
