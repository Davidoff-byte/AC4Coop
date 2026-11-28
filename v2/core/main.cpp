// AC4Coop v2 entry (docs/v2/ARCHITECTURE.md). DllMain does only what must happen before WinMain: config, logger, build
// gate, launcher arm check, the two pre-WinMain patches, the crash recorder; everything else runs on the init thread.
#include "build_gate.h"
#include "build_info.h"
#include "config.h"
#include "crash_ctx.h"
#include "hooks.h"
#include "log.h"
#include "paths.h"
#include "tick.h"
#include "../boot/boot.h"
#include "../modules.h"

#include <windows.h>
#include <bcrypt.h>
#include <process.h>
#include <cstdio>

namespace {

HMODULE g_self = nullptr;

bool Sha256File(const char* path, char hex[65]) {
    hex[0] = 0;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr; bool ok = false;
    static unsigned char buf[1 << 16]; unsigned char dig[32];
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0))) {
        DWORD n = 0; ok = true;
        while (ReadFile(f, buf, sizeof(buf), &n, nullptr) && n) if (!BCRYPT_SUCCESS(BCryptHashData(h, buf, n, 0))) { ok = false; break; }
        if (ok && BCRYPT_SUCCESS(BCryptFinishHash(h, dig, 32, 0))) for (int i = 0; i < 32; ++i) sprintf_s(hex + 2 * i, 3, "%02x", dig[i]);
        else ok = false;
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return ok;
}

bool LaunchArmed() {
    const v2::Config& c = v2::Cfg();
    if (!c.requireArm) return true;
    char p[MAX_PATH]; _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\AC4Coop.armed", Paths::ModuleDir());
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &a)) return false;
    FILETIME now; GetSystemTimeAsFileTime(&now);
    const long long t = ((long long)now.dwHighDateTime << 32 | now.dwLowDateTime) -
                        ((long long)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime);
    const long long ageS = t / 10000000LL;
    return ageS >= -5 && ageS <= c.armWindowSec;
}

unsigned __stdcall InitThread(void*) {
    v2::LogConfig();
    v2::boot::InstallLate();
    v2::tick::Install();
    v2::modules::Init();            // feature modules register their ticks / own-updates / hooks here
    v2::hooks::LogManifest();
    LOGI("[Init] done: %u hook site(s), %u refused\n", (unsigned)v2::hooks::Count(), (unsigned)v2::hooks::Refused());
    v2::log::Flush();
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    g_self = mod;
    DisableThreadLibraryCalls(mod);
    v2::LoadConfig(Paths::Ini());
    char leaf[64], logPath[MAX_PATH], asiPath[MAX_PATH], asiHash[65];
    _snprintf_s(leaf, sizeof(leaf), _TRUNCATE, "phase1_log_%u.txt", GetCurrentProcessId());
    Paths::LogPath(logPath, sizeof(logPath), leaf);
    v2::log::Start(logPath, (v2::log::Level)v2::Cfg().logLevel);
    GetModuleFileNameA(mod, asiPath, MAX_PATH);
    Sha256File(asiPath, asiHash);
#ifdef AC4COOP_DEVTOOLS
    const char* flavour = " devtools";
#else
    const char* flavour = "";
#endif
    LOGI("[Build] asi=%s commit=%s tree=%s version=v2%s\n", asiHash, BUILD_GIT_COMMIT, BUILD_GIT_TREE, flavour);
    if (!BuildGate::Verify(mod)) { LOGE("[Init] build not trusted - inert (no patches)\n"); return TRUE; }
    if (!LaunchArmed()) { LOGI("[Init] not started by the co-op launcher - inert\n"); return TRUE; }
    v2::boot::InstallEarly();
    CrashCtx::Init();
    _beginthreadex(nullptr, 0, InitThread, nullptr, 0, nullptr);
    return TRUE;
}
