// DevTools only: the three v1 symbols the ported pilot bridge (pilot.cpp) calls, mapped onto v2.
#include "../core/log.h"
#include "../core/mem.h"
#include "../engine/world.h"
#include "../engine/scimitar/AI/Human.h"

void Phase1_LogLine(const char* s) { LOGI("%s", s); }

// v1 S9 test command (kCmdLifeHit): not served in v2 (phase 3 brings the combat module).
int Phase1_TestLifeHit(void*, int, int*, int*) { return -9; }

namespace {
// A Scimitar Handle holds a HandleContainer* {object, ?, flags}; the engine's "none" sentinel is 0x04DD5F8C.
void* ResolveHandle(const Handle& h) {
    const uintptr_t hc = (uintptr_t)h.m_HandleContainer;
    if (!hc || hc == 0x04DD5F8C) return nullptr;
    void* o = v2::mem::HandleObject((const void*)hc);
    return (uintptr_t)o == 0x04DD5F8C ? nullptr : o;
}
}  // namespace

// The local player's ship (the vehicle Edward stands on): Human::m_IsOnShipEntity, else m_Data.m_Vehicle (v1 phase1.cpp:386).
void* Phase1_PlayerShipEntity() {
    Human* h = (Human*)v2::engine::LocalPlayerHuman();
    if (!h) return nullptr;
    void* s = ResolveHandle(h->m_IsOnShipEntity);
    return s ? s : ResolveHandle(h->m_Data.m_Vehicle);
}
