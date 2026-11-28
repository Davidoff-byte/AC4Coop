#pragma once
// [Coop] CrashContext=1 (default 0): read-only crash recorder. When the process takes an access violation of the
// EXECUTE kind (the instruction pointer itself is not code: a call through a freed or null object), it writes the
// registers, the stack and the objects the registers and stack point at to AC4Coop_crashctx_<pid>.txt in the log
// directory, once, and lets the exception continue exactly as before. It changes no engine state and handles nothing.
// WHY: the Windows crash dump of this game holds no heap, so the freed object and its owner cannot be read from it
// (crashes 2026-11-12 15:58 and 16:07).
namespace CrashCtx {
    void Init();   // DllMain attach, after the build gate
}
