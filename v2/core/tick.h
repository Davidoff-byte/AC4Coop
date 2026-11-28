#pragma once
// The game tick (ARCHITECTURE.md s3, R1): the local player's Human update, hooked at 0x016FAACD (esi = the Human being
// updated). All engine writes happen here, from modules registered in a fixed order, with no SEH around them (R2).
// Other Humans' updates are offered to own-update handlers (a module may write only the Human it is given).
#include <cstdint>

namespace v2::tick {

using TickFn      = void (*)(void* playerHuman, uint64_t nowUs);
using OwnUpdateFn = void (*)(void* human);
using CmdFn       = void (*)(const void* args);

bool Install();                                              // init: the Human update detour (HookManager)
void Register(const char* name, int order, TickFn fn);       // init only; lower order runs first
void RegisterOwnUpdate(const char* name, OwnUpdateFn fn);    // init only
// Any thread: run fn(args) at the start of the next tick, in post order. args (<= 56 bytes) are copied.
bool Post(CmdFn fn, const void* args, uint32_t size);

bool     InWorld();                // the last tick saw a world with a player (lock-free)
void*    PlayerHuman();            // the Human of the last tick (any thread; may be stale)
uint64_t TickCount();

}  // namespace v2::tick
