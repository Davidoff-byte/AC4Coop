// 00C4F2A0 - HavokBodyCtorFromCinfo
// NOT COMPILED: probe arm. This file transcribes the verified call site and
// the binary's own structure; it is hashed as the candidate for receipt 00C4F2A0.
// It is not presented as a byte-equal reimplementation (that arm needs cl 16.00.40219.01).
// src/phase1.cpp:1811  ConstructGhostBody(): Hook::CallClassMethod<void, void*, void*>(0x00C4F2A0, body, cinfo);
// ROLE (from the call site): builds the Havok body/ghost in `body` from `cinfo`.
// WHAT THE BYTES DO (verified, disassembled from AC4BFSP.exe at 0x00C4F2A0):
//   * prologue push ebx / mov ebx,esp / and esp,-16 -> an MSVC function with an aligned frame;
//     `mov esi,ecx` => __thiscall, ECX = the destination object (`body`).
//   * push ebx / mov ebx,esp makes [ebx+4] the RETURN ADDRESS and [ebx+8] the FIRST (and only)
//     stack argument, which is why the function ends `ret 4`: [ebx+8] -> EDI = the cinfo
//     pointer. `mov eax,[edi+4]` is pushed and calls 0x00C42170 -- the base-class constructor
//     on the destination with cinfo[+4].
//   * `mov dword ptr [esi],0x025DBDA4` -- installs the class vtable on the new object, so this
//     is a CONSTRUCTOR, not a helper.
//   * then copies cinfo fields into the new object: byte [edi+0xC4] -> [esi+0x88],
//     word [edi+0xC6] -> [esi+0xA6], dword [edi] -> [esi+0x2C], dword [edi+0xC0] handled with a
//     null test and call 0x00BA3100 (a shared_ptr/refcount copy), [esi+0x210] released via
//     0x00BA31A0. The counts/fields line up with the call site's cinfo layout.
//   * ends with a `ret` followed by 0xCC padding (function end).
// It returns with the object fully constructed in ECX-relative storage; the caller keeps its own
// pointer, which is why the call site ignores the return value.
