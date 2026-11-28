#include "boot.h"
#include "../core/config.h"
#include "../core/hooks.h"
#include "../core/log.h"
#include "../core/mem.h"
#include "../engine/title.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace v2::boot {
namespace {

// The IAT slot of `func` imported by name from `dll` in the main module.
void** FindImportSlot(const char* dll, const char* func) {
    const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    __try {   // pure reads of the PE headers (mem rule: reads only)
        auto* dos = (IMAGE_DOS_HEADER*)base;
        auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + d.VirtualAddress); imp->Name; ++imp) {
            if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
            auto* names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
            auto* slots = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++slots) {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                auto* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
                if (strcmp((const char*)ibn->Name, func) == 0) return (void**)&slots->u1.Function;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

// ---- two copies on one PC: WinMain's single-instance mutex gets the pid appended (v1 dllmain.cpp InstallMultiInstance)
typedef HANDLE(WINAPI* CreateMutexW_t)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
CreateMutexW_t g_createMutexW = nullptr;
HANDLE WINAPI MyCreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCWSTR name) {
    wchar_t buf[300];
    if (name) { wsprintfW(buf, L"%s_ac4coop_%u", name, GetCurrentProcessId()); name = buf; }
    return g_createMutexW(sa, owner, name);
}

// ---- windowed: replace the window style at the CreateWindowExA site (v1 windowed.cpp; 0x004065A0 add esi,0xCA0000)
uintptr_t g_styleResume = 0;
uint32_t  g_borderless = 0;
__declspec(naked) void StyleDetour() {
    __asm {
        cmp  g_borderless, 0
        jne  popup
        mov  esi, 0x00CA0000        // WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX
        jmp  done
    popup:
        mov  esi, 0x80000000        // WS_POPUP
    done:
        jmp  dword ptr [g_styleResume]
    }
}

// ---- keep simulating on focus loss: sub_4054D0(active) with active == 0 is the deactivate path (v1 InstallNoPauseHook)
typedef int(__cdecl* Deactivate_t)(char);
Deactivate_t g_deactivate = nullptr;
int __cdecl MyDeactivate(char active) { return active == 0 ? 0 : g_deactivate(active); }

// ---- skip the four boot videos: BinkOpen returns NULL for them (v1 video_hook.cpp; the caller handles NULL)
typedef void*(__stdcall* BinkOpen_t)(const char*, unsigned);
BinkOpen_t g_binkOpen = nullptr;
bool IsBootVideo(const char* name) {
    if (!name) return false;
    const char* leaf = strrchr(name, '\\'); leaf = leaf ? leaf + 1 : name;
    return _stricmp(leaf, "warning_disclaimer.bik") == 0 || _stricmp(leaf, "TWIMTBP.bik") == 0 ||
           _stricmp(leaf, "abstergo_logo.bik") == 0 || _stricmp(leaf, "ac4bf_logo.bik") == 0;
}
void* __stdcall MyBinkOpen(const char* name, unsigned flags) {
    if (IsBootVideo(name)) { LOGI("[Boot] skip video %s\n", name); return nullptr; }
    return g_binkOpen(name, flags);
}

// ---- auto-continue: the hook plumbing only; the title logic and its engine calls live in engine/title.cpp (R7).
typedef int(__thiscall* StartUpdate_t)(void*, int);
typedef char(__thiscall* SlotHover_t)(void*, uint32_t);
StartUpdate_t g_startUpdate = nullptr;
SlotHover_t   g_slotHover = nullptr;
int __fastcall MyStartUpdate(void* self, void*, int a2) {
    engine::title::TryPressStart(self);
    return g_startUpdate(self, a2);
}
char __fastcall MySlotHover(void* self, void*, uint32_t hovered) {
    const char r = g_slotHover(self, hovered);
    engine::title::TryContinue(self, hovered, Cfg().autoContinueSlot);
    return r;
}
}  // namespace

void InstallEarly() {
    const Config& c = Cfg();
    if (c.multiInstance) {
        void** slot = FindImportSlot("kernel32.dll", "CreateMutexW");
        FARPROC real = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateMutexW");
        if (slot) hooks::PointerSlot("boot", "CreateMutexW", slot, (void*)&MyCreateMutexW, (void**)&g_createMutexW, (uintptr_t)real);
        else LOGE("[Boot] CreateMutexW import not found\n");
    }
    if (!c.fullscreen) {
        static const uint8_t kStyle[6] = { 0x81, 0xC6, 0x00, 0x00, 0xCA, 0x00 };   // add esi, 0xCA0000
        g_borderless = c.borderless ? 1u : 0u;
        hooks::JmpDetour("boot", "WindowStyle", 0x004065A0, kStyle, sizeof(kStyle), (void*)&StyleDetour, &g_styleResume);
    }
}

void InstallLate() {
    const Config& c = Cfg();
    if (c.noPause) {
        static const uint8_t kDeact[7] = { 0x55, 0x8B, 0xEC, 0x53, 0x8A, 0x5D, 0x08 };   // push ebp; mov ebp,esp; push ebx; mov bl,[ebp+8]
        hooks::FunctionHook("boot", "NoPause", 0x004054D0, kDeact, sizeof(kDeact), (void*)&MyDeactivate, (void**)&g_deactivate);
    }
    if (c.skipIntro) {
        // The slot must hold the resolved address before we swap it (else the loader could still overwrite it).
        HMODULE bink = GetModuleHandleW(L"bink2w32.dll");
        void* real = bink ? (void*)GetProcAddress(bink, "_BinkOpen@8") : nullptr;
        void** slot = FindImportSlot("bink2w32.dll", "_BinkOpen@8");
        for (int i = 0; i < 50 && slot && real && *slot != real; ++i) Sleep(20);
        if (slot && real) hooks::PointerSlot("boot", "BinkOpen", slot, (void*)&MyBinkOpen, (void**)&g_binkOpen, (uintptr_t)real);
        else LOGW("[Boot] BinkOpen import not resolvable - intro videos play\n");
    }
    if (c.autoContinue) {
        static const uint8_t kStart[6] = { 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x08 };   // push esi; mov esi,ecx; mov eax,[esi+8]
        static const uint8_t kHover[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x28 };   // push ebp; mov ebp,esp; sub esp,28h
        hooks::FunctionHook("boot", "TitleStartUpdate", 0x017B4840, kStart, sizeof(kStart), (void*)&MyStartUpdate, (void**)&g_startUpdate);
        hooks::FunctionHook("boot", "TitleSlotHover", 0x018E2390, kHover, sizeof(kHover), (void*)&MySlotHover, (void**)&g_slotHover);
    }
}

}  // namespace v2::boot
