#pragma once

// Null guard for the world-update walk's two leaves: 0x0075A1F0 and 0x0075A1B0.
//
// The walk (sub_4AD990) loads its render/level instance L ONCE per walk and hands the same value to
// both leaves:
//
//   sub_4AD990  4ADB03  mov  ecx,[edi+4]     ; ecx = L, one value for the whole walk
//               4ADB0A  call sub_75A1F0      ; -> sub_5D55C0
//               4ADB15  mov  ecx,[edi+4]     ; the SAME L
//               4ADB1C  call sub_75A1B0      ; -> sub_5D2A50
//               4ADB2B  mov  [edi+4],eax     ; restored
//
// [edi+4] is written only at 0x65DDF7 (L = *(X+0x134), X = *(U+0x24), U = *(W+0x814)), so L is per
// walk, not per node -- a proxy cannot supply its own. When L == 0 the leaf faults at its first
// dereference:
//
//   75A1F8 / 75A1B8   8B 88 A0 00 00 00    mov ecx,[ecx+0A0h]    ; EIP 0x0035A1F8 in the guest dump
//
// The engine's own early-out for that situation is byte [U+0x159] != 0 (0x65DDC7, also honoured at
// 0x669429), but that flag kills the whole view's render -- not acceptable. This unit instead makes the
// dereference impossible: a detour at each function ENTRY returns immediately when ecx == 0.
//
// Behaviour when ecx != 0 is byte-for-byte the original: the detour re-executes the 8 stolen bytes
// (push ebp; mov ebp,esp; mov eax,[ebp+8]; push eax; push ecx) and jumps to the original instruction at
// site+8. No register is rewritten and only the flags word passes through `test ecx,ecx`, which no
// instruction before the next flag write reads.
//
// The null path returns the function's own convention exactly: `pop ebp; retn 4` in the original
// epilogue becomes `xor eax,eax; ret 4` (we never pushed EBP), so it also drops the one stack argument.
//
// Config: [Coop] WalkNullGuard (default 1). 0 = nothing is patched, not even a VirtualAlloc.
// Inert unless BuildGate::Trusted() and ac4bfsp.exe is present -- i.e. inert unless the mod is loaded
// against the verified build.
namespace WalkGuard {
    // Installs both entry detours into one cave within +-2 GB of the module, then starts a reporter
    // that logs the null-hit count at most once per second. Every site is byte-verified against the
    // IDB-documented prologue, resume instruction and epilogue before anything is written; a mismatch
    // leaves BOTH functions untouched (never half-patched). Safe to call more than once.
    void Init();
}
