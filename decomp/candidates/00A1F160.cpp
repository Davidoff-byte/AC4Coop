// 00A1F160 - ObjectRegistryLookupOwner
// NOT COMPILED: probe arm. This file transcribes the verified call site and
// the binary's own structure; it is hashed as the candidate for receipt 00A1F160.
// It is not presented as a byte-equal reimplementation (that arm needs cl 16.00.40219.01).
// src/phase1.cpp:4370-4371  ResolveEntityByIdRaw():
//     return Hook::CallClassMethod<void*, void*, uint32_t, uint32_t>(
//         0x00A1F160, (void*)0x04DD9824, (uint32_t)id, (uint32_t)(id >> 32));
// ROLE (from the call site): resolve (id_lo,id_hi) in the engine object registry at
// 0x04DD9824 and return the registered owner pointer.
// WHAT THE BYTES DO (verified, disassembled from AC4BFSP.exe at 0x00A1F160):
//   * push ebp / mov ebp,esp / push ecx,ebx,esi,edi; `mov esi,[ebp+8]`, `mov edi,[ebp+0xC]`,
//     `mov ebx,ecx` => __thiscall(this=ECX, uint32 id_lo, uint32 id_hi), and `ret 8` at the exits
//     confirms exactly two stack arguments (12 bytes of pushed args popped by the caller's 8 --
//     the callee pops 8, i.e. thiscall).
//   * `test esi,esi / jne` then `test edi,edi / je 0xA1F20A` => (0,0) short-circuits to the
//     "not found" exit.
//   * `mov ecx,0x04DD601C ; call 0x00D587F0` -- locks the registry critical section; the call site
//     pre-checks that same object at 0x04DD601C, so the source comment and the binary agree.
//   * `cmp byte ptr [0x027F4AC6],0 ; je 0xA1F200` -- the exact byte_27F4AC6 gate the comment names.
//   * `push edi / push esi / push 0 ; mov ecx,ebx ; call 0x00A1E8D0` -- table lookup;
//     on a hit `mov eax,[eax+8]` returns entry->owner (+8 in the 16-byte entry), unlocks via
//     0x00AC6580 and returns; on a miss it tries 0x04DD5FA0 (call 0x00A076F0) and then the
//     thiscall fallback 0x00A1DC00, each path unlocking 0x04DD601C again before returning.
//   * no generation check anywhere in the body -- matches the warning in the source comment that
//     the caller must re-verify the object's own id (the call site does).
