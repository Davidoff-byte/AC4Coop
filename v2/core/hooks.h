#pragma once
// HookManager (ARCHITECTURE.md R6). Every patch v2 makes goes through here: the expected bytes are checked before the
// write, an overlap with an installed site is refused, and the manifest is logged at boot. Installs happen from DllMain
// or the init thread only; nothing is installed or removed while the game runs.
#include <cstddef>
#include <cstdint>

namespace v2::hooks {

// jmp rel32 at `va` to `target`, the rest of `len` (>= 5) stolen bytes NOP-padded. The caller's naked target re-executes
// the stolen instructions and jumps to *resume = va + len.
bool JmpDetour(const char* owner, const char* name, uintptr_t va, const uint8_t* expect, size_t len, void* target,
               uintptr_t* resume);
// Function hook: jmp at `va` to `detour`; *original receives a trampoline (the `len` stolen bytes, which must be
// position-independent whole instructions, then a jmp back to va + len) that calls the original function.
bool FunctionHook(const char* owner, const char* name, uintptr_t va, const uint8_t* expect, size_t len, void* detour,
                  void** original);
// Re-target an existing `call rel32` (E8) at `va`; *original receives its old target.
bool CallRedirect(const char* owner, const char* name, uintptr_t va, void* target, uintptr_t* original);
// Overwrite `len` bytes at `va` with `bytes` after checking `expect`.
bool BytePatch(const char* owner, const char* name, uintptr_t va, const uint8_t* expect, const uint8_t* bytes, size_t len);
// Swap one pointer slot (IAT entry or vtable slot); *original receives the old pointer. `expect` 0 = any.
bool PointerSlot(const char* owner, const char* name, void** slot, void* replacement, void** original, uintptr_t expect);

void   LogManifest();     // one line per installed site
size_t Count();
size_t Refused();

}  // namespace v2::hooks
