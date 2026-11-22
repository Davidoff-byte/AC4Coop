#pragma once

// [Coop] CallTraceVt (default empty = off): log calls through chosen vtables, to find which engine function runs for an
// action we can trigger but have not located statically (first use: fast travel).
//
// Format: "VT:N,VT:N,..." (hex vtable VA, number of slots). Each slot whose target is in the exe's code and is not shared
// by another listed vtable (shared base methods would flood the log) gets a 40-byte stub: it pushes [first stack arg],
// [return address], ecx and the slot id, calls a cdecl logger, restores every register and the flags, and continues at
// the original target with `push orig; ret`. The vtable dword is swapped to the stub. Read-only otherwise: no field is
// written, no code byte is patched. Each slot logs its first 12 calls in full, then counts; a summary every 10 s.
namespace CallTrace {
    void Init();
}
