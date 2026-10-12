// Phase 1 — spawn a second Human from the player's cached EntityBuilder + RowSelector, bind to pad 1.
#include "phase1.h"
#include "net.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include "Scimitar/World.h"
#include "Scimitar/EntityBuilder.h"
#include "Scimitar/RowSelector.h"
#include "Scimitar/DuplicationManager.h"
#include "Scimitar/PlayerPadManager.h"
#include "Scimitar/LoadingManager.h"
#include "Scimitar/LoadingThread.h"
#include "Scimitar/WhiteRoomManager.h"
#include "Scimitar/PlayerProgression.h"
#include "Scimitar/HandleManager.h"
#include "Scimitar/AI/AIEntityHelper.h"
#include "Scimitar/AI/Human.h"
#include "Scimitar/AI/HumanSwimFSM.h"
#include "Scimitar/Components/CharacterController.h"

namespace {

// sub_4054D0(char active): focus activate/deactivate. Inline-hook target (see InstallNoPauseHook), so it
// keeps its raw RVA — the hook patches this code rather than calling it through a class wrapper.
const uintptr_t RVA_Deactivate      = 0x54D0;

// Human activity-context replication (parkour/climb/swim/etc). A Human owns per-activity context
// objects; exactly one is ACTIVE on the "main AI track" (track 1) and drives its animation statechart.
// Ground is the implicit default (GetCurrentContextID returns 2 when no explicit context is active).
// Human* (from GetHuman) IS the AIActor base, so AIActor::TransitToContext/ExitCurrentContext thiscall
// directly on the Human pointer.
const int       MAIN_AI_TRACK       = 1;        // locomotion/parkour contexts live on track 1
const int       CTX_GROUND          = 2;        // default context id (no explicit context active)
const int       CTX_SWIM            = 10;       // Swim context: positions the character via its physics capsule
const int       ACTLAYER_PRIMARY    = 0;        // main body-animation layer (action replication)
const int       ACTPERSIST_ALWAYS   = 4;        // ACTPersistType: keep a blend-factor slot across action/transition
                                                // resets (default is transient) — makes our driven blend stick

// Per-frame MAIN-THREAD hook site (from AC4Hook): the Human update, which runs once per frame
// for the player. base+0x12FAACD steals 6 bytes:  8B 8E 08 01 00 00  =  mov ecx,[esi+0x108]
// (World::LinkUpdateAction was wrong: it only runs from EngineContextImp::RelinkWorldTasks on
//  graph changes, not per frame.)
const uintptr_t RVA_HumanUpdate = 0x12FAACD;
const size_t    HU_STEAL_LEN    = 6;

Entity*  g_puppet = nullptr;   // last-spawned puppet entity
void*  g_blockReg = nullptr; // entity whose RegisterPlayer we suppress (the puppet)
int    g_spawnTries = 0;     // net-mode spawn-retry budget (reset on despawn so respawn can retry)
int    g_spawnCd = 0;        // net-mode spawn-retry cooldown (frames)
uint64_t g_puppetWorldId = 0;// worldId the puppet was spawned in (informational)
void*  g_puppetVtbl = nullptr;// vtable ptr captured at spawn; if it changes/unmaps the entity was freed
                             // (world change) -> drop the dangling ref. A pause leaves it intact.
int    g_driveLoco = 0;      // [Coop] DriveLocomotion: replicate HumanGroundData player->puppet
int    g_driveContext = 0;   // [Coop] DriveContext: replicate the active Human context (parkour/climb/swim)
int    g_lastRemoteCtx = -1; // last remote contextId we drove onto the puppet (edge-trigger; -1 = fresh puppet)
int      g_driveActions = 1;             // [Coop] DriveActions: replay the peer's current action (anim clip)
uint32_t g_lastActionAlias = 0xFFFFFFFF; // last action alias driven onto the puppet (edge-trigger; -1 = fresh)
// Pending blend-in override for the action we just targeted on the puppet: applied once the puppet's current
// evaluator is that action (SetTargetAction may not create it synchronously). Frames left to wait; 0 = none.
uint32_t g_blendFixAlias = 0;
float    g_blendFixLen   = 0.0f;
float    g_blendFixStartFromEnd = 0.0f;
float    g_blendFixTargetStart  = 0.0f;
uint16_t g_blendFixFlags = 0;
int      g_blendFixWait  = 0;
int      g_disablePuppetAI = 1;          // [Coop] DisablePuppetAI: disable the puppet's behavior component so
                                         // its own statechart stops issuing actions/physics that fight our sync
bool     g_casActivated = false;         // whether the current puppet's CharacterActionSet has been activated (once)
// [Coop] HybridLoco: per-context action ownership. The crash is two threads playing an action at once, so we
// guarantee EXACTLY ONE action-writer at all times: on the GROUND context the puppet's OWN FSM owns actions
// (behavior ENABLED, we only feed HumanGroundData, we never SetTargetAction) — this reproduces the MVP's
// correct locomotion gait; on every OTHER context WE own actions (behavior DISABLED + action replication) for
// parkour/climb/etc. HybridLoco=0 falls back to the old global-flag behavior for A/B.
int      g_hybridLoco = 1;
int      g_seekOnSwitch = 1;   // [Coop] SeekOnSwitch: seek a newly targeted action to the peer's phase
float    g_minBlendIn = 0.1f;   // [Coop] MinBlendInMs: blend used when the peer's switch had none (0 = off)
int      g_bhvState   = -1;              // current puppet behavior-enable state: -1 unknown, 0 disabled, 1 enabled
int      g_switchCd   = 0;               // frames to suppress OUR action writes after a behavior toggle, so the
                                         // game's worker-thread action update can quiesce and never overlap ours
// [Coop] DriveContextMask: bitmask of which context ids we ALLOW the puppet to enter (bit N = context N).
// The target-less parkour contexts (Ladder/Ledge/Walling/NarrowObject/Climbing/Hanging/Boat) crash the
// engine's later Human::Update because it derefs climb-target data (wall normal / ledge point / edge query)
// a real climb computed but our externally-teleported puppet never did. InAir(6)/Swim(10) are self-contained
// (gravity/water) and safe. Default = (1<<6)|(1<<10) = 0x440. Ground(2) is always allowed (handled via
// ExitCurrentContext, no target needed). A disallowed id pops the puppet back to ground instead of crashing.
unsigned g_ctxMask = (1u << 6) | (1u << 10);
HookMath::Vector4  g_offset = { 2.0f, 0.0f, 0.0f, 0.0f }; // puppet stands +2m on X from the player

// [Coop] DisableSpawns: suppress ambient entity spawning so both clients present the same empty world
// (ambient NPCs/guards/ships are seeded independently per client and won't sync). Two layered hooks,
// both gated on this flag:
//   (1) SpawningRequest::Spawn (RVA 0x39B8D0) -> return 0 (declined), but ONLY once the player is live
//       in the world (playerInWorld). This is the universal spawn entry every strategy funnels through
//       (crowd, blob NPCs, guards, sphere, naval). Blocking it unconditionally hung boot because the
//       player + essential entities spawn through here too, so we allow spawns while the player is
//       absent (boot / world load / fast-travel / death) and only decline during live gameplay.
//   (2) CrowdHerder::UpdateSpawningNeeds (RVA 0x1578B50) -> no-op. Stops the herd crowd at the source
//       so it never even builds spawn requests (crowd is never essential, so this stays unconditional).
// The retail PC build ignores the -crowdmaxnpc dev knob, so memory hooks are the only way.
int    g_disableSpawns = 0;

// [Net] interpolation: buffer remote snapshots and render the puppet a fixed delay behind local
// time, lerping between the two snapshots bracketing render time. Hides jitter and packet loss.
int    g_interpEnabled = 1;  // [Net] Interpolation
int    g_interpDelayMs = 100;// [Net] InterpDelayMs (render this far behind now; >= ~2 packet intervals)

// HumanGroundData drive fields, offsets from the bundle pointer *(GetHuman(entity)+0x160).
// All PC-verified by the mapped Human.h.
//const uintptr_t OFF_BUNDLE              = 0x160; // *(Human+0x160) = HumanDataBundle*
//const uintptr_t OFF_G_HEADING           = 0x30;  // ubiVector4 m_DestHeading
//const uintptr_t OFF_G_CURSPD            = 0xA4;  // float m_CurSpeedRatio
//const uintptr_t OFF_G_DESTSPD           = 0xA8;  // float m_DestSpeedRatio
//const uintptr_t OFF_G_ISSET             = 0xFC;  // bool  m_IsSet
//const uintptr_t OFF_G_SPRINT            = 0xFD;  // bool  m_Sprint
//const uintptr_t OFF_G_BREAKHARDONEDGE   = 0xFE;  // bool  m_BreakHardOnEdge
//const uintptr_t OFF_G_BREAKHARD         = 0x101;  // bool  m_BreakHard
//const uintptr_t OFF_G_CURMVT            = 0x134; // u8    m_CurMvtType
//const uintptr_t OFF_G_DESTMVT           = 0x135; // u8    m_DestMvtType

uintptr_t base() { return (uintptr_t)GetModuleHandleW(nullptr); }

void L(const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    printf("%s", buf);
    OutputDebugStringA(buf);
    // Also append to a per-process file, flushed each line, so the trail survives a crash.
    static FILE* f = nullptr;
    if (!f) {
        char name[64], path[MAX_PATH];
        wsprintfA(name, "phase1_log_%u.txt", GetCurrentProcessId());
        Phase1::ModuleRelativePath(path, sizeof(path), name);
        fopen_s(&f, path, "w");
    }
    if (f) { fputs(buf, f); fflush(f); }
}

} // namespace

void Phase1::ModuleRelativePath(char* out, size_t cap, const char* fileName) {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&Phase1::ModuleRelativePath, &self);
    char dir[MAX_PATH] = {};
    DWORD n = self ? GetModuleFileNameA(self, dir, MAX_PATH) : 0;
    char* slash = (n > 0 && n < MAX_PATH) ? strrchr(dir, '\\') : nullptr;
    if (slash) slash[1] = 0;
    else       dir[0] = 0;   // fall back to the current directory
    _snprintf_s(out, cap, _TRUNCATE, "%s%s", dir, fileName);
}

namespace {

// SEH read-probe: actually attempt the read rather than trusting VirtualQuery.
// (VirtualQuery-based checks were wrongly rejecting valid large-address-aware pointers >2GB.)
bool readable(const void* p, size_t n) {
    if (!p) return false;
    __try {
        volatile char first = ((const volatile char*)p)[0];
        volatile char last  = ((const volatile char*)p)[n - 1];
        (void)first; (void)last;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Compact blend-array log: only the non-zero slots as "idx:val" (46 slots, most are 0 at any time).
static void FormatBlendSlots(const float* bf, int n, char* out, size_t cap) {
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < n && len + 16 < cap; ++i) {
        if (bf[i] > -0.005f && bf[i] < 0.005f) continue;
        len += _snprintf_s(out + len, cap - len, _TRUNCATE, "%s%d:%.2f", len ? " " : "", i, bf[i]);
    }
}

// Vec4 <-> plain-float[4] copies (the wire uses float[4] to avoid 16-byte alignment in the packed packet).
inline void vecToArr(float* d, const HookMath::Vector4& v) { d[0]=v.x; d[1]=v.y; d[2]=v.z; d[3]=v.w; }
inline void arrToVec(HookMath::Vector4& v, const float* s) { v.x=s[0]; v.y=s[1]; v.z=s[2]; v.w=s[3]; }

// Resolve a Scimitar handle container -> object pointer:  obj = HC[0] & (HC[2] >> 31)
void* handleDeref(void* hc) {
    if (!readable(hc, 12)) return nullptr;
    int32_t obj  = ((int32_t*)hc)[0];
    int32_t vbit = ((int32_t*)hc)[2] >> 31;   // all-ones if valid, else 0
    return (void*)(obj & vbit);
}

// True when the given world is the white-room (loadout/test world), which we must not spawn into.
// Callers run inside an SEH __try, so the raw derefs here are covered; the readable() probes keep the
// common "manager not up yet" case from tripping the handler.
bool inWhiteRoom(void* world) {
    WhiteRoomManager* wrm = WhiteRoomManager::GetInstance();
    if (!readable(wrm, 0x20)) return false;
    void* hc = wrm->GetWorldHandle();        // handle container to the white-room world
    void* wrWorld = handleDeref(hc);
    return wrWorld != nullptr && wrWorld == world;
}

// Reload an EntityBuilder by resource ID (mirrors LoadingThread::LoadFile) and wait for the object.
void* reloadBuilder(uint64_t id) {
    HandleManager* hm = HandleManager::GetInstance();
    if (!hm) { L("[Phase1] HandleManager unavailable\n"); return nullptr; }

    HandleContainer* handle = hm->CreateReference(id, nullptr);
    if (!readable(handle, 4)) { L("[Phase1] CreateReference failed\n"); return nullptr; }
    int stream = LoadingThread::GetFastLoadDataStream();
    char ok = LoadingThread::AddLoadTask_Callback(0, stream, 0xFFFFFFFF, id, (int)&handle, 0, 0, 0, 0, 0);
    LoadingThread::GetWaitTasks();
    L("[Phase1] reload AddLoadTask ok=%d handle=%p stream=%d\n", (int)ok, handle, stream);

    // object populates at handle->m_Object once the load completes
    for (int i = 0; i < 200; ++i) {
        if (readable(handle, 4)) {
            void* obj = handle->m_Object;
            if (readable(obj, 8)) return obj;
        }
        Sleep(10);
    }
    return nullptr;
}

// Walk Human -> swim context -> FSM -> HumanSwimScope (see HumanSwimFSM.h). Each hop is readable()-guarded
// so a chain that isn't up yet (not actually swimming) just yields null instead of faulting. Optionally
// reports the intermediate pointers for offset-verification logging.
HumanSwimScope* swimScope(Human* h, void** outCtx = nullptr, void** outFsm = nullptr) {
    void** pSwim = (void**)&h->m_HumanContextBundle.m_Swim;  // address only (no deref of the value yet)
    if (!readable(pSwim, 4)) return nullptr;                 // guards a wrong bundle offset in Human.h
    HumanSwim* swimCtx = (HumanSwim*)*pSwim;
    if (outCtx) *outCtx = swimCtx;
    if (!readable(swimCtx, 0x34)) return nullptr;
    HumanSwimFSM* fsm = swimCtx->m_FSM;
    if (outFsm) *outFsm = fsm;
    if (!readable(fsm, 0x10)) return nullptr;
    HumanSwimScope* sc = fsm->m_HumanSwimScope;
    if (!readable(sc, 0x1114)) return nullptr;   // need through m_DestSpeedRatio @ 0x1110
    return sc;
}

} // namespace

void* Phase1::TrySpawnPuppet() {
    Entity* newEntity = nullptr;
    __try {
        World* world = World::GetCurrentWorld();
        if (!world)
            return nullptr;

        Entity* player = world->GetPlayer();
        if (!player)
            return nullptr;

        // 2) player's cached builder handle + selector
        RowSelector*     selector  = PlayerProgression::GetCachedSelector();
        HandleContainer* builderHC = PlayerProgression::GetCachedBuilderHandle();
        void* builder = handleDeref(builderHC);

        // container diagnostics + extract the builder's resource ID (HandleContainer::m_ObjectID)
        uint64_t builderID = 0;
        if (readable(builderHC, 20)) {
            uint32_t* hc = (uint32_t*)builderHC;
            builderID = builderHC->m_ObjectID;
            L("[Phase1] HC: [0]=%08X [1]=%08X [2]=%08X  builderID=%08X`%08X\n",
              hc[0], hc[1], hc[2], (uint32_t)(builderID >> 32), (uint32_t)builderID);
        } else {
            L("[Phase1] builderHC=%p not readable\n", builderHC);
        }
        L("[Phase1] selector=%p builderHC=%p builder(masked)=%p\n", selector, builderHC, builder);

        // builder object was released after the player spawned -> reload it by ID
        if (!readable(builder, 8) && builderID) {
            bool doReload = GetPrivateProfileIntA("Coop", "ReloadBuilder", 1, ".\\AC4Coop.ini") != 0;
            if (doReload) {
                L("[Phase1] builder released; reloading by ID...\n");
                builder = reloadBuilder(builderID);
                L("[Phase1] reloaded builder=%p\n", builder);
            }
        }
        if (!readable(builder, 8) || !readable(selector, 8)) {
            L("[Phase1] builder/selector not ready\n"); return nullptr;
        }
        L("[Phase1] builder.vtbl=%p selector.vtbl=%p\n", *(void**)builder, *(void**)selector);

        // 3) duplicate the selector (BuildEntityWithSelector consumes/frees the one it gets).
        //    RowSelector::Duplicate needs an active DuplicationManager scope (Begin/End), exactly
        //    as the game wraps it in LoadEntityBuilderAndSelectorsForPlayer.
        DuplicationManager::Begin(1, 0);
        RowSelector* dupSel = nullptr;
        // If Duplicate faults (e.g. a stale selector right after world travel), End MUST still run or the
        // DuplicationManager stays unbalanced and every later respawn attempt corrupts. __finally ensures it.
        __try {
            dupSel = selector->Duplicate(0, 0);
        } __finally {
            DuplicationManager::End();
        }
        L("[Phase1] dupSelector=%p\n", dupSel);
        if (!readable(dupSel, 8)) { L("[Phase1] selector duplicate failed\n"); return nullptr; }

        // 4) BUILD the entity
        int rc = ((EntityBuilder*)builder)->BuildEntityWithSelector(&newEntity, dupSel);
        L("[Phase1] BuildEntityWithSelector rc=%d entity=%p\n", rc, newEntity);
        if (!readable(newEntity, 0x50)) { L("[Phase1] build produced no entity\n"); return nullptr; }
        L("[Phase1] entity.vtbl=%p\n", *(void**)newEntity);
        // hijack probe: did building change who the game considers "the player"?
        L("[Phase1] GetPlayer AFTER build = %p (orig=%p new=%p)\n",
          world->GetPlayer(), player, newEntity);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        L("[Phase1] EXCEPTION during resolve/build (code=0x%08X)\n", GetExceptionCode());
        return nullptr;
    }

    // Steps below mutate world state — gate behind ini so we can test build-only first.
    bool doIntegrate = Net::Enabled() ||
                       GetPrivateProfileIntA("Coop", "IntegratePuppet", 0, ".\\AC4Coop.ini") != 0;
    if (!doIntegrate) {
        // Not in world -> do NOT register it for driving (SetGlobalMatrixInternal on a
        // non-integrated entity is unsafe). Just report the successful build.
        L("[Phase1] built entity only (Coop.IntegratePuppet=0). Not adding to world.\n");
        return newEntity;
    }

    __try {
        World* world = World::GetCurrentWorld();
        Entity* player = world->GetPlayer();

        // 5) mark the puppet so BhvAssassin::AddToWorldInternal skips player/gamer/pad enrollment,
        //    then add to world (deferred engine command; processes next frame)
        g_blockReg = newEntity;
        world->AddEntity(newEntity);
        L("[Phase1] AddEntity issued; GetPlayer now = %p (orig=%p new=%p)\n",
          world->GetPlayer(), player, newEntity);

        // 6) position at player + offset (identity orientation, no physics for initial placement)
        HookMath::Vector4 ppos = player->GetPosition();
        newEntity->SetPosition(ppos + g_offset);

        // 7) OPTIONAL: bind to pad 1 (local only; a networked puppet is a remote proxy, never a pad player)
        if (!Net::Enabled() && GetPrivateProfileIntA("Coop", "RegisterPad1", 0, ".\\AC4Coop.ini") != 0) {
            PlayerPadManager* pm = PlayerPadManager::FromWorld(world);
            L("[Phase1] padManager=%p\n", pm);
            if (readable(pm, 0x20)) {
                pm->RegisterPlayer(newEntity, 1);
                L("[Phase1] RegisterPlayer(pad=1) done\n");
            }
        }
        g_puppet = newEntity;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        L("[Phase1] EXCEPTION during integrate (code=0x%08X)\n", GetExceptionCode());
    }
    return newEntity;
}

// ---- networking capture/apply (Front B) --------------------------------------------
// Read a player entity's full state (transform + locomotion) into a PlayerState.
bool Phase1::CaptureLocalState(PlayerState& s) {
    __try {
        World* world = World::GetCurrentWorld();
        if (!world)
            return false;

        if (inWhiteRoom(world))
            return false;

        s.worldId = world->GetObjectID();

        Entity* player = world->GetPlayer();
        if (!player)
            return false;

        s.matrix = player->m_GlobalMatrix;
        s.curSpeed = s.destSpeed = 0;
        s.sprint = s.curMvt = s.destMvt = 0;
        s.contextId = CTX_GROUND;

        // DIAGNOSTIC: real world-space speed from position delta (m_CurSpeedRatio reads 0 always on this build,
        // so it can't tell us if the player is moving). This is the ground truth of "am I actually running?".
        static float s_pvx = 0, s_pvy = 0, s_pvz = 0; static bool s_pvHave = false;
        float velMag = 0.0f;
        {
            float x = player->m_GlobalMatrix.M41, y = player->m_GlobalMatrix.M42, z = player->m_GlobalMatrix.M43;
            if (s_pvHave) { float dx=x-s_pvx, dy=y-s_pvy, dz=z-s_pvz; velMag = sqrtf(dx*dx+dy*dy+dz*dz); }
            s_pvx=x; s_pvy=y; s_pvz=z; s_pvHave=true;
        }

        Human* h = AIEntityHelper::GetHuman(player);
        HumanDataBundle* pB = h->m_Data.m_HumanDataBundle;
        s.contextId = h->GetCurrentContextID(MAIN_AI_TRACK);

        s.curSpeed = pB->m_GroundData.m_CurSpeedRatio;
        s.destSpeed = pB->m_GroundData.m_DestSpeedRatio;
        s.heading = pB->m_GroundData.m_DestHeading;
        s.isSet = pB->m_GroundData.m_IsSet;
        s.sprint = pB->m_GroundData.m_Sprint;
        s.breakHardOnEdge = pB->m_GroundData.m_BreakHardOnEdge;
        s.breakHard = pB->m_GroundData.m_BreakHard;
        s.curMvt = pB->m_GroundData.m_CurMvtType;
        s.destMvt = pB->m_GroundData.m_DestMvtType;
        s.humanGroundStage = (int32_t)pB->m_GroundData.m_HumanGroundStage;
        s.subState = (int32_t)pB->m_GroundData.m_SubState;

        s.weaponTotallyOut = h->m_WeaponTotallyOut;
        s.isInTree = h->m_IsInTree;
        s.isOnShip = h->m_IsOnShip;
        s.isOnOcean = h->m_IsOnOcean;
        s.doJumpToVoid = h->m_DoJumpToVoid;
        s.stalkingZoneCount = h->m_StalkingZoneCount;
        s.stalkingZoneBlendedCount = h->m_StalkingZoneBlendedCount;
        s.stalkingClipCount = h->m_StalkingClipCount;
        s.stalkingClipBlendedCount = h->m_StalkingClipBlendedCount;
        s.stalkingClipAllowFastStalkingCount = h->m_StalkingClipAllowFastStalkingCount;
        s.disturbedZoneCount = h->m_DisturbedZoneCount;

        // Current primary action (anim clip) the player is playing — the file-defined alias at Action+0x08.
        // Replicated so the puppet plays the same clip regardless of context (universal animation sync).
        s.actionAlias = 0;
        s.actionChannel = 0xFFFF;
        s.actionLayer = ACTLAYER_PRIMARY;
        s.blendCount = 0;
        for (int i = 0; i < BLEND_MAX; ++i) s.blendFactors[i] = 0.0f;
        s.timeScale = 1.0f;   // default = normal playback; NEVER send 0 (would freeze the puppet's anim)
        s.actionRatio = 0.0f;
        s.blendInLen = -1.0f;
        s.blendInStartFromEnd = 0.0f;
        s.blendInTargetStart = 0.0f;
        s.blendInFlags = 0;
        s.isTransitionAction = 0;
        {
            ActionComponent* ac = h->GetActionComponent();
            if (readable(ac, 0x40)) {
                s.timeScale = ac->GetTimeScale(0, ACTLAYER_PRIMARY);   // primary-channel clip speed (foot-speed warp)
                s.actionRatio = ac->GetCurrentActionRatio(0, ACTLAYER_PRIMARY);   // playback phase (seek on switch)
                // The blend-in the peer's engine actually used for its current action (incl. FSM-generated
                // custom transitions the puppet can't look up itself).
                char* ev = (char*)ac->GetCurrentEvaluator(0, ACTLAYER_PRIMARY);
                if (readable(ev, 0x2B0) && *(uint8_t*)(ev + 0xD4) == 4) {
                    s.blendInLen          = *(float*)(ev + 0x1C8);
                    s.blendInStartFromEnd = *(float*)(ev + 0x1CC);
                    s.blendInTargetStart  = *(float*)(ev + 0x1D0);
                    s.blendInFlags        = *(uint16_t*)(ev + 0x1DC);
                    s.isTransitionAction = *(uint8_t*)(ev + 0x258);
                }
                unsigned char mirrored = 0;
                void* act = ac->GetCurrentAction(&mirrored, 0, ACTLAYER_PRIMARY);
                if (readable(act, 0x14)) {
                    s.actionAlias = *(uint32_t*)((char*)act + 8);
                    void* bpc = *(void**)((char*)act + 0x0C);          // Action::m_BodyPartChannel
                    if (readable(bpc, 0x0A)) s.actionChannel = *(uint16_t*)((char*)bpc + 8);
                    s.actionLayer = *(int32_t*)((char*)act + 0x10);    // Action::m_Layer (ACTLayer)

                    // The walk/jog/run speed sub-blend within the current locomotion LOOP action. Blend factors
                    // are indexed on the CONTROLLER channel (channel 0 = the primary channel controller, the
                    // same channelID we pass to GetCurrentAction above), NOT the action's body-part channel
                    // (s.actionChannel, which is only for SetTargetAction/CAS routing). The engine's own
                    // parkour code sets these as SetBlendFactor(i, v, 0, ACTLAYER_PRIMARY). The discrete START
                    // clips are picked by the alias, so only the LOOP needs this blend to vary with speed.
                    // Capture the WHOLE array: the ground move cycle evaluates a 46-slot blend tree
                    // (UpdateGroundBlendTree -> SetTargetBlendFactors(46 floats, 0, PRIMARY)) and the gait
                    // weights live past slot 3 — syncing only 0..3 left the puppet on the default (walk).
                    int n = ac->GetBlendFactorCount(0, ACTLAYER_PRIMARY);
                    if (n > BLEND_MAX) n = BLEND_MAX;
                    if (n < 0) n = 0;
                    s.blendCount = (uint8_t)n;
                    for (int i = 0; i < n; ++i)
                        s.blendFactors[i] = ac->GetBlendFactor((unsigned char)i, 0, ACTLAYER_PRIMARY);
                }
            }
            static uint32_t lastCapAlias = 0;
            if (s.actionAlias != lastCapAlias) {
                L("[Net] cap SWITCH %u -> %u blendIn=%.2f type=%u flags=%04X tgt=%.2f trans=%u ratio=%.2f\n",
                  lastCapAlias, s.actionAlias, s.blendInLen, (unsigned)(s.blendInFlags & 7), (unsigned)s.blendInFlags,
                  s.blendInTargetStart, (unsigned)s.isTransitionAction, s.actionRatio);
                lastCapAlias = s.actionAlias;
            }
            static int bc = 0;
            if ((++bc % 30) == 0) {
                char nz[512];
                FormatBlendSlots(s.blendFactors, s.blendCount, nz, sizeof(nz));
                L("[Net] cap action=%u ch=%u ly=%d blend(n=%u)={%s} ts=%.2f VEL=%.3f\n",
                  s.actionAlias, s.actionChannel, s.actionLayer, (unsigned)s.blendCount, nz, s.timeScale, velMag);
            }
        }

        // Context-tagged payload. Swim: read the swim locomotion input from the FSM scope so the peer's
        // puppet can play the moving-swim anim and travel (ground rides the common fields above).
        if (s.contextId == CTX_SWIM) {
            HumanSwimScope* sc = swimScope(h);
            if (sc) {
                s.ctx.swim.destDir[0] = sc->m_DestDirection.x;
                s.ctx.swim.destDir[1] = sc->m_DestDirection.y;
                s.ctx.swim.destDir[2] = sc->m_DestDirection.z;
                s.ctx.swim.destDir[3] = sc->m_DestDirection.w;
                s.ctx.swim.destSpeedRatio     = sc->m_DestSpeedRatio;
                s.ctx.swim.curSpeedProportion = sc->m_CurSpeedProportion;   // eased current speed -> anim blend
                s.ctx.swim.sprint             = sc->m_Sprint;
                for (int i = 0; i < 8; ++i) s.ctx.swim.aiSwim[i] = sc->m_AISwim[i];  // swim anim-request floats
            } else {
                s.ctx.swim.destSpeedRatio = 0.0f;
                s.ctx.swim.curSpeedProportion = 0.0f;
                s.ctx.swim.sprint = 0.0f;
                for (int i = 0; i < 8; ++i) s.ctx.swim.aiSwim[i] = 0.0f;
            }
            // Character-controller motion vectors (the actual velocity/force/impulse the FSM & anim graph
            // read). GetCharacterComponent returns the biped controller (a CharacterController).
            CharacterController* pcc = (CharacterController*)h->GetCharacterComponent();
            if (readable(pcc, 0x600)) {
                vecToArr(s.ctx.swim.ccVelocity, pcc->m_Velocity);
                vecToArr(s.ctx.swim.ccForce,    pcc->m_Force);
                vecToArr(s.ctx.swim.ccImpulse,  pcc->m_Impulse);
            } else {
                for (int i = 0; i < 4; ++i) { s.ctx.swim.ccVelocity[i]=0; s.ctx.swim.ccForce[i]=0; s.ctx.swim.ccImpulse[i]=0; }
            }
            static int sl = 0;
            if ((++sl % 30) == 0)
                L("[Net] swim cap scope=%p spd=%.3f cur=%.3f vel=(%.2f,%.2f,%.2f)\n",
                  sc, s.ctx.swim.destSpeedRatio, s.ctx.swim.curSpeedProportion,
                  s.ctx.swim.ccVelocity[0], s.ctx.swim.ccVelocity[1], s.ctx.swim.ccVelocity[2]);
        }

        s.magic = NET_MAGIC;
        s.valid = 1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Apply a remote peer's state to the puppet (absolute position — shared world coords, no offset).
// Never nulls g_puppet on a transient fault (that would trigger endless respawns).
void Phase1::ApplyRemoteState(const PlayerState& s) {
    if (!g_puppet || !s.valid) return;
    __try {
        // Position + orientation of the entity: SetGlobalMatrixInternal(updatePhysics=true) sets the entity
        // matrix, updates the graphic node/skeleton and pushes into the physic components — this drives
        // rendering and rotation for every context. It does NOT move the character controller, though, which
        // is what FSM/physics-driven contexts (swim) actually position from — that's handled by the kamikaze
        // controller warp just below. s.matrix is pack(1)/const, so copy to an aligned local for the call.
        HookMath::Transform m = s.matrix;
        g_puppet->SetGlobalMatrixInternal(&m, true);
        Human* h = AIEntityHelper::GetHuman(g_puppet);
        HumanDataBundle* qB = h->m_Data.m_HumanDataBundle;
        bool bok = readable((void*)qB, 0x150);

        // ---- per-frame effective mode (per-context action ownership) -------------------------------------
        // Exactly ONE action-writer at all times so two threads never play an action at once (the crash):
        //   GROUND context  -> puppet's own FSM owns actions: behavior ENABLED, feed HumanGroundData, we do
        //                       NOT SetTargetAction (reproduces the MVP's correct locomotion gait).
        //   other contexts  -> WE own actions: behavior DISABLED + action replication (parkour/climb/etc.).
        // HybridLoco=0 restores the old global-flag behavior for A/B.
        bool hybrid     = (g_hybridLoco != 0);
        bool peerGround = (s.contextId == CTX_GROUND);
        bool aiOff   = hybrid ? !peerGround : (g_disablePuppetAI != 0);   // disable the puppet's behavior?
        bool actNow  = hybrid ? !peerGround : (g_driveActions   != 0);    // run our action replication?
        bool locoNow = hybrid ?  peerGround : (g_driveLoco       != 0);   // feed HumanGroundData (FSM anim)?
        bool braindead = aiOff && !actNow;                               // position-only (never true in hybrid)

        // Behavior ownership toggle: ENABLE when the FSM should own the puppet (ground), DISABLE when WE do
        // (off-ground). A toggle starts a short cooldown during which we suppress our own action writes, so the
        // game's worker-thread action update can quiesce before/after we take over -> the two never overlap.
        // Retries every frame until the deferred AddToWorld has actually set the behavior component up.
        {
            int wantBhv = aiOff ? 0 : 1;
            if (g_bhvState != wantBhv) {
                void* bhv = g_puppet->GetBehaviorComponent();
                if (readable(bhv, 8)) {
                    ((Component*)bhv)->SetEnabled(wantBhv != 0);
                    g_bhvState = wantBhv;
                    g_switchCd = 4;                            // ~4 frames of no-action-writes around the toggle
                    if (wantBhv == 0) g_casActivated = false;  // re-activate our CAS when we next own actions
                    L("[Net] puppet behavior %s (ctx=%d)\n", wantBhv ? "ENABLED" : "disabled", s.contextId);
                }
            }
        }
        bool writeActions = actNow && g_switchCd == 0;   // our action writes are gated on the settle cooldown
        if (g_switchCd > 0) --g_switchCd;

        // We are ALWAYS the position authority (SetGlobalMatrixInternal above): stop the character controller
        // from driving the entity so its physics can never fight the forced matrix — in both modes. In ground
        // mode the (enabled) FSM still animates the correct gait: UpdateMoveCycleNew derives the walk/jog/run
        // from the HumanGroundData intention we feed below, not from the puppet actually moving through space.
        {
            CharacterController* cc = (CharacterController*)h->GetCharacterComponent();
            if (readable(cc, 0x600)) cc->m_DriveEntity = false;
        }
        // Braindead only (no action replication AND no FSM): kill the ActionComponent update (flag @ +0x159)
        // so nothing poses the skeleton. Never reached in hybrid (there's always an animator).
        if (braindead) {
            ActionComponent* ac = h->GetActionComponent();
            if (readable(ac, 0x160)) *((unsigned char*)ac + 0x159) = 1;
        }

        if (locoNow) {   // feed HumanGroundData so the puppet's (enabled) FSM locomotes at the peer's gait
        qB->m_GroundData.m_CurSpeedRatio = s.curSpeed;
        qB->m_GroundData.m_DestSpeedRatio = s.destSpeed;
        qB->m_GroundData.m_DestHeading = s.heading;
        qB->m_GroundData.m_IsSet = s.isSet;
        qB->m_GroundData.m_Sprint = s.sprint;
        qB->m_GroundData.m_BreakHardOnEdge = s.breakHardOnEdge;
        qB->m_GroundData.m_BreakHard = s.breakHard;
        qB->m_GroundData.m_CurMvtType = s.curMvt;
        qB->m_GroundData.m_DestMvtType = s.destMvt;
        qB->m_GroundData.m_HumanGroundStage = (HumanGroundData::HumanGroundStage)s.humanGroundStage;
        qB->m_GroundData.m_SubState = (HumanGroundData::HumanGroundSubState)s.subState;
        h->m_WeaponTotallyOut = s.weaponTotallyOut;
        h->m_IsInTree = s.isInTree;
        h->m_IsOnShip = s.isOnShip;
        h->m_IsOnOcean = s.isOnOcean;
        h->m_DoJumpToVoid = s.doJumpToVoid;
        h->m_StalkingZoneCount = s.stalkingZoneCount;
        h->m_StalkingZoneBlendedCount = s.stalkingZoneBlendedCount;
        h->m_StalkingClipCount = s.stalkingClipCount;
        h->m_StalkingClipBlendedCount = s.stalkingClipBlendedCount;
        h->m_StalkingClipAllowFastStalkingCount = s.stalkingClipAllowFastStalkingCount;
        h->m_DisturbedZoneCount = s.disturbedZoneCount;
        } // locoNow
        // Context replication: make the puppet ENTER the peer's active context so its statechart plays
        // the matching parkour/climb/swim animation (transform is already synced). Edge-triggered on the
        // remote id so we mirror each context change once (re-issuing every frame would restart the anim).
        // TransitToContext exits the current context and enters the new one on the puppet's active track;
        // returning to ground (id 2) has no explicit context, so we pop via ExitCurrentContext instead.
        if (g_driveContext && readable(h, 0x50)) {
            int want = s.contextId;
            if (want != g_lastRemoteCtx) {
                int cur = h->GetCurrentContextID(MAIN_AI_TRACK);
                // A context is drivable if it's ground, or its bit is set in the allow-mask. The masked
                // (target-needing) contexts would crash a later engine update, so instead of entering them
                // we force the puppet back to ground — never leave it stranded in a stale parkour pose.
                bool allowed = (want == CTX_GROUND) ||
                               (want >= 0 && want < 32 && (g_ctxMask & (1u << want)));
                if (allowed) {
                    if (want != cur) {
                        if (want == CTX_GROUND) h->ExitCurrentContext();
                        else                    h->TransitToContext(want);
                        L("[Net] context %d -> %d (puppet)\n", cur, want);
                    }
                } else if (cur != CTX_GROUND) {
                    h->ExitCurrentContext();
                    L("[Net] context %d -> ground (puppet; %d masked)\n", cur, want);
                }
                g_lastRemoteCtx = want;
            }
        }
        // Swim locomotion drive: once the puppet is actually in the Swim context (transitioned above), the
        // FSM + scope exist, so push the peer's swim input onto them — this is what makes the puppet leave
        // the idle swim pose, play the moving-swim anim and travel. Position is already corrected via the
        // SetGlobalMatrixInternal capsule push at the top. Gated exactly like the swim position path.
        if (g_driveContext && (g_ctxMask & (1u << CTX_SWIM)) && s.contextId == CTX_SWIM) {
            // Authoritative position: the swim FSM positions the character through its CharacterController,
            // which the entity-matrix push doesn't move — so force the controller to the peer's position via
            // the kamikaze target + flag (field-write form of CharacterController::SetGlobalPositionKamikaze).
            // Swim-gated only: on ground the FSM is really moving the character and this fights it -> jitter.
            CharacterController* cc = (CharacterController*)h->GetCharacterComponent();
            if (readable(cc, 0x600)) {
                cc->m_KamikaziPosition.x  = s.matrix.M41;
                cc->m_KamikaziPosition.y  = s.matrix.M42;
                cc->m_KamikaziPosition.z  = s.matrix.M43;
                cc->m_KamikaziPosition.w  = 1.0f;
                cc->m_ConcatenateToAnim   = false;
                cc->m_UseVelocityPosition = false;
                cc->m_UseKamikazePosition = true;
                // Give the puppet the peer's real motion so the FSM/anim graph see movement (the kamikaze
                // position is authoritative; these feed the state/anim, not the final position).
                arrToVec(cc->m_Velocity, s.ctx.swim.ccVelocity);
                arrToVec(cc->m_Force,    s.ctx.swim.ccForce);
                arrToVec(cc->m_Impulse,  s.ctx.swim.ccImpulse);
            }
            HumanSwimScope* sc = swimScope(h);
            if (sc) {
                sc->m_DestDirection.x   = s.ctx.swim.destDir[0];
                sc->m_DestDirection.y   = s.ctx.swim.destDir[1];
                sc->m_DestDirection.z   = s.ctx.swim.destDir[2];
                sc->m_DestDirection.w   = s.ctx.swim.destDir[3];
                sc->m_DestSpeedRatio    = s.ctx.swim.destSpeedRatio;
                // Also drive the eased CURRENT state — the FSM won't advance these itself on a teleported
                // puppet (no real velocity), so without them the facing goes stale and the stroke anim
                // never blends in. m_CurSpeedProportion is the stroke blend; m_Sprint the fast-swim flag;
                // m_AISwim[] are the per-gait swim anim-request floats that select which stroke plays.
                sc->m_CurSpeedProportion = s.ctx.swim.curSpeedProportion;
                sc->m_Sprint             = s.ctx.swim.sprint;
                for (int i = 0; i < 8; ++i) sc->m_AISwim[i] = s.ctx.swim.aiSwim[i];
            }
            static int sw = 0;
            if ((++sw % 30) == 0)
                L("[Net] swim drv scope=%p destSpd=%.3f curSpd=%.3f sprint=%.0f ai=[%.2f %.2f %.2f %.2f %.2f]\n",
                  sc, s.ctx.swim.destSpeedRatio, s.ctx.swim.curSpeedProportion, s.ctx.swim.sprint,
                  s.ctx.swim.aiSwim[0], s.ctx.swim.aiSwim[2], s.ctx.swim.aiSwim[3], s.ctx.swim.aiSwim[4], s.ctx.swim.aiSwim[6]);
        }

        // Give the puppet the local player's CharacterActionSet ONCE so its actions resolve to the right
        // character's clip overrides (both are Edward on this machine) instead of the generic base. The
        // gameplay path that normally sets it is disabled on the puppet. One-shot: re-setting it every frame
        // resets the action to idle. The CAS reference lives at ActionComponent+0x18.
        if (writeActions && !g_casActivated) {
            ActionComponent* pac = h->GetActionComponent();
            World* casWorld = World::GetCurrentWorld();
            Entity* casLp = casWorld ? casWorld->GetPlayer() : nullptr;
            Human*  casLph = casLp ? AIEntityHelper::GetHuman(casLp) : nullptr;
            ActionComponent* lac = casLph ? casLph->GetActionComponent() : nullptr;
            if (readable(pac, 0x40) && readable(lac, 0x40)) {
                pac->ActivateCharacterActionSet((char*)lac + 0x18);
                g_casActivated = true;
                L("[Net] CAS set from local player (pac=%p lac=%p)\n", pac, lac);
            }
        }

        // Universal animation sync: play the peer's current action (anim clip) on the puppet via
        // SetTargetAction. Only issue when the peer's alias changed AND the puppet isn't ALREADY playing it:
        // re-forcing a target action that's already current re-runs its transition and intermittently CRASHES
        // (our tracked g_lastActionAlias resets on spawn/despawn, which is when a redundant re-force happened).
        // Checking the puppet's real current action is the reliable guard.
        if (writeActions && s.actionAlias != 0 && s.actionAlias != g_lastActionAlias) {
            ActionComponent* ac = h->GetActionComponent();
            if (readable(ac, 0x40)) {
                unsigned char mir = 0;
                void* cur = ac->GetCurrentAction(&mir, 0, s.actionLayer);
                uint32_t curAlias = readable(cur, 0x0C) ? *(uint32_t*)((char*)cur + 8) : 0;
                if (curAlias != s.actionAlias) {
                    // Pass the peer's real channel + layer (not 0xFFFF / a hardcoded layer) so the
                    // CharacterActionSetManager doesn't take the kit-lookup path that bails and drops the CAS
                    // override (-> base/Connor clip).
                    ac->SetTargetAction((int)s.actionAlias, 0, 0, (int)s.actionChannel, (int)s.actionLayer);
                    // Start the new clip at the peer's phase instead of frame 0 — the game does the same right
                    // after its own SetTargetAction calls. Without it every swap pops. processDisplacement=0
                    // (we own position), processEvents=0 (don't fire the skipped span's anim events).
                    if (g_seekOnSwitch && s.actionRatio > 0.0f && s.actionRatio < 1.0f)
                        ac->SetCurrentActionRatio(s.actionRatio, 0, false, false, ACTLAYER_PRIMARY);
                    if (s.blendInLen >= 0.0f) {
                        g_blendFixAlias = s.actionAlias;
                        g_blendFixLen   = s.blendInLen;
                        g_blendFixStartFromEnd = s.blendInStartFromEnd;
                        g_blendFixTargetStart  = s.blendInTargetStart;
                        g_blendFixFlags = s.blendInFlags;
                        g_blendFixWait  = 10;
                    }
                    L("[Net] action -> %u ch=%u ly=%d ratio=%.2f blendIn=%.2f (puppet, cur was %u)\n", s.actionAlias, s.actionChannel, s.actionLayer, s.actionRatio, s.blendInLen, curAlias);
                }
                g_lastActionAlias = s.actionAlias;
            }
        }

        // Blend-in override: once the puppet's current evaluator IS the action we just targeted, give its
        // blend-in the peer's real length (stored ActionBlend, used if Start() hasn't run yet, AND the runtime
        // length, used if it has). Only while the blend is still in progress, so we never rewind a finished one.
        if (writeActions && g_blendFixWait > 0) {
            ActionComponent* ac = h->GetActionComponent();
            char* ev = readable(ac, 0x40) ? (char*)ac->GetCurrentEvaluator(0, ACTLAYER_PRIMARY) : nullptr;
            void* evAct = (readable(ev, 0x2B0) && *(uint8_t*)(ev + 0xD4) == 4) ? *(void**)(ev + 0x2A8) : nullptr;
            if (readable(evAct, 0x0C) && *(uint32_t*)((char*)evAct + 8) == g_blendFixAlias) {
                // Blend-in descriptor = m_BlendInEvaluator (+0xE0).m_ActionBlend (+0x1C8): m_Length, +0x1CC
                // m_StartTimeFromEnd, +0x1D0 m_TargetStartTime, +0x1D4 ratio keys (array - NOT copied), +0x1DC
                // u16 m_Flags (bits 0-2 m_BlendType). Copy the peer's whole descriptor: without a found transition
                // the engine uses flags 0x600 = ACTBLENDTYPE_NONE, a hard cut whatever the length says.
                // ACTBLENDTYPE_AROLLBROLL_WITHRATIO (6) needs the ratio keys we don't copy -> plain AROLLBROLL (1).
                float    t       = *(float*)(ev + 0xE0);
                float    was     = *(float*)(ev + 0xE4);
                uint16_t wasFl   = *(uint16_t*)(ev + 0x1DC);
                uint16_t fl      = g_blendFixFlags;
                float    len     = g_blendFixLen;
                if ((fl & 7) == 6) fl = (uint16_t)((fl & ~7) | 1);
                bool write = true;
                if ((fl & 7) == 0) {
                    // Peer cut with NO blend — typically a transition clip handing off to its target, seamless on
                    // the peer because the clip ends on the target's start pose. Our switch lands whenever the
                    // packet does, so that match isn't guaranteed: never downgrade a blend the puppet found itself,
                    // and otherwise use a short AROLLBROLL (MinBlendIn) to hide the timing seam.
                    if ((wasFl & 7) != 0 || g_minBlendIn <= 0.0f) write = false;
                    else { fl = (uint16_t)((fl & ~7) | 1); len = g_minBlendIn; }
                }
                if (write && t < len) {
                    *(float*)(ev + 0x1C8)    = len;
                    *(float*)(ev + 0x1CC)    = g_blendFixStartFromEnd;
                    *(float*)(ev + 0x1D0)    = g_blendFixTargetStart;
                    *(uint16_t*)(ev + 0x1DC) = fl;
                    *(float*)(ev + 0xE4)     = len;   // runtime length, in case Start() already ran
                } else {
                    write = false;
                }
                L("[Net] blendIn fix alias=%u len %.2f -> %.2f type %u -> %u (t=%.2f)%s\n",
                  g_blendFixAlias, was, len, (unsigned)(wasFl & 7), (unsigned)(fl & 7), t, write ? "" : " [kept]");
                g_blendFixWait = 0;
            } else {
                --g_blendFixWait;
            }
        }

        // Blend factors (the walk/jog/run speed sub-blend within the current LOOP action) — set EVERY frame
        // since they vary continuously with the peer's speed. Indexed on the primary CONTROLLER channel 0 +
        // ACTLAYER_PRIMARY (same as the capture / the engine's own SetBlendFactor calls), NOT s.actionChannel
        // (the body-part channel used for SetTargetAction routing).
        // ROOT CAUSE of "always slowest walk": a plain SetBlendFactor is TRANSIENT — the puppet's action update
        // resets it to the slot default before sampling the clip, so our value never took. Confirmed via log:
        // the LOCAL capture reads real, varying blend factors (e.g. [0,0.04,0.95,0]) — the capture was fine;
        // the write just didn't stick. Fix (mirrors the engine's own locomotion): mark each slot
        // ACTPERSIST_ALWAYS(=4) so it survives the per-frame reset, THEN write the peer's factor. Done every
        // frame because the controller/evaluator is recreated on each SetTargetAction (fresh default persistence).
        if (writeActions && s.actionAlias != 0) {
            ActionComponent* ac = h->GetActionComponent();
            if (readable(ac, 0x40)) {
                // Full array, bounded by BOTH the peer's slot count and the puppet's own controller size
                // (sized per action; the engine drops writes past it anyway).
                int n = s.blendCount;
                int pn = ac->GetBlendFactorCount(0, ACTLAYER_PRIMARY);
                if (pn < n) n = pn;
                for (int i = 0; i < n; ++i) {
                    ac->SetBlendFactorPersistence((unsigned char)i, ACTPERSIST_ALWAYS, 0, ACTLAYER_PRIMARY);
                    ac->SetBlendFactor((unsigned char)i, s.blendFactors[i], 0, ACTLAYER_PRIMARY);
                }
                // Playback time scale (clip speed) — the missing gait lever: read-back proved the action AND
                // blend factors already match the peer, so the only remaining difference is how fast the clip
                // plays. Persist it (like the blend) so the puppet's action update doesn't snap it back to 1.0.
                // Guard against a bogus/zero wire value that would freeze the anim.
                if (s.timeScale > 0.01f) {
                    ac->SetTimeScalePersistence(ACTPERSIST_ALWAYS, 0, ACTLAYER_PRIMARY);
                    ac->SetTimeScale(s.timeScale, 0, ACTLAYER_PRIMARY, false);
                }
                // DIAGNOSTIC: read back the puppet's OWN state right after we wrote it — (1) is it playing the
                // action we sent (alias match), (2) did the blend stick (readback == sent), (3) did the time
                // scale stick. One line tells us where the chain breaks.
                static int rc = 0;
                if ((++rc % 30) == 0) {
                    unsigned char mir = 0;
                    void* pcur = ac->GetCurrentAction(&mir, 0, ACTLAYER_PRIMARY);
                    uint32_t pAlias = readable(pcur, 0x0C) ? *(uint32_t*)((char*)pcur + 8) : 0;
                    float rb[BLEND_MAX] = {};
                    int pn = ac->GetBlendFactorCount(0, ACTLAYER_PRIMARY);
                    if (pn > BLEND_MAX) pn = BLEND_MAX;
                    for (int i = 0; i < pn; ++i) rb[i] = ac->GetBlendFactor((unsigned char)i, 0, ACTLAYER_PRIMARY);
                    float pupTS = ac->GetTimeScale(0, ACTLAYER_PRIMARY);
                    char rxs[512], pus[512];
                    FormatBlendSlots(s.blendFactors, s.blendCount, rxs, sizeof(rxs));
                    FormatBlendSlots(rb, pn, pus, sizeof(pus));
                    L("[Net] PUP act=%u (rx=%u) rxBlend(n=%u)={%s} pupBlend(n=%d)={%s} rxTS=%.2f pupTS=%.2f\n",
                      pAlias, s.actionAlias, (unsigned)s.blendCount, rxs, pn, pus, s.timeScale, pupTS);
                }
            }
        }
        static int lc = 0;
        if ((++lc % 30) == 0)
            L("[Net] apply seq=%u pos=(%.1f,%.1f,%.1f) spd=%.2f bundle=%d\n",
              s.seq, g_puppet->m_GlobalMatrix.M41, g_puppet->m_GlobalMatrix.M42, g_puppet->m_GlobalMatrix.M43, s.curSpeed, bok ? 1 : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int ec = 0; if ((++ec % 30) == 0) L("[Net] ApplyRemoteState EXCEPTION (code=0x%08X)\n", GetExceptionCode());
    }
}

// Remove the puppet from the world when the peer leaves our world or disconnects. World::RemoveEntity
// is the deferred counterpart to World::AddEntity (processes next frame). Clears g_puppet immediately
// so nothing drives the entity after we've asked for its removal. Main-thread only (like spawn).
void Phase1::DespawnPuppet() {
    if (!g_puppet) return;
    Entity* dead = g_puppet;
    g_puppet = nullptr;      // stop driving it right away, before the deferred removal runs
    g_blockReg = nullptr;    // clear the pad-suppression marker
    g_lastRemoteCtx = -1;    // next puppet starts fresh (re-apply the first remote context)
    g_lastActionAlias = 0xFFFFFFFF;  // re-apply the peer's action onto the next puppet
    g_bhvState = -1;                 // re-establish the next puppet's behavior state (toggle from unknown)
    g_switchCd = 0;
    g_casActivated = false;          // re-activate its CharacterActionSet
    __try {
        World* world = World::GetCurrentWorld();
        if (!world)
            return;

        world->RemoveEntity(dead);

        L("[Net] puppet despawned (%p)\n", dead);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        L("[Net] DespawnPuppet EXCEPTION (code=0x%08X)\n", GetExceptionCode());
    }
}

// ---- main-thread execution via a per-frame inline hook -----------------------------
static volatile long g_spawnReq = 0;     // set by hotkey (any thread), consumed on the main thread
static volatile long g_dumpReq  = 0;     // set by hotkey (F7): dump player Human* + swim chain next frame
static uint32_t      g_frameJmpBack = 0;  // resume addr after the stolen instruction (base+RVA+6)

// The Human-update hook fires for EVERY human each frame; only act for the player's human so our
// logic runs exactly once per frame (spawn/send/apply must not run dozens of times per frame).
static bool isPlayerUpdate(uintptr_t b, Human* updatedHuman) {
    __try {
        World* world = World::GetCurrentWorld();
        if (!world)
            return false;

        Entity* player = world->GetPlayer();
        if (!player)
            return false;

        return AIEntityHelper::GetHuman(player) == updatedHuman;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- snapshot interpolation (main-thread only; no locking needed) ------------------
// Buffer the last N received remote states with their local arrival time, then each frame render
// the puppet at (now - g_interpDelayMs), lerping between the two snapshots that bracket that time.
struct Snap { PlayerState st; uint64_t t; };
static const int SNAP_MAX = 32;
static Snap     g_snaps[SNAP_MAX];
static int      g_snapCount = 0;   // filled entries (<= SNAP_MAX)
static int      g_snapHead  = 0;   // ring index of the next write slot
static uint32_t g_lastSnapSeq = 0;
static bool     g_haveSnapSeq = false;

static void norm3(float* v) {
    float len = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (len > 1e-6f) { v[0] /= len; v[1] /= len; v[2] /= len; }
}
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// Blend two states. Start from the newer (copies seq/worldId/contextId/flags/bytes/valid — these are
// discrete and must NOT be interpolated), then lerp the continuous fields: position, orientation
// (per-row matrix nlerp in the game's native basis), speeds and heading.
static void lerpState(const PlayerState& a, const PlayerState& b, float t, PlayerState& o) {
    o = b;
    if (a.worldId != b.worldId) return;   // spanning a world change -> just show the newer state

    // Position: straight lerp of the translation row.
    float px = lerpf(a.matrix.M41, b.matrix.M41, t);
    float py = lerpf(a.matrix.M42, b.matrix.M42, t);
    float pz = lerpf(a.matrix.M43, b.matrix.M43, t);

    // Orientation: interpolate the rotation basis (rows 0-2) DIRECTLY in the game's native matrix space —
    // per-component lerp each row then renormalize (nlerp). Do NOT round-trip through the engine quat<->
    // matrix ops (FromMatrix/ToMatrix): they use a different basis convention and left the puppet rotated
    // 90 deg (lying on the floor, then culled). Across one interp interval the rotation delta is tiny, so
    // nlerped rows stay effectively orthonormal, and this stays in the exact basis the working direct-copy
    // apply path uses. M14/M24/M34 keep b's values (0 for an affine transform, copied by o = b above).
    o.matrix.M11 = lerpf(a.matrix.M11, b.matrix.M11, t);
    o.matrix.M12 = lerpf(a.matrix.M12, b.matrix.M12, t);
    o.matrix.M13 = lerpf(a.matrix.M13, b.matrix.M13, t);
    norm3(&o.matrix.M11);
    o.matrix.M21 = lerpf(a.matrix.M21, b.matrix.M21, t);
    o.matrix.M22 = lerpf(a.matrix.M22, b.matrix.M22, t);
    o.matrix.M23 = lerpf(a.matrix.M23, b.matrix.M23, t);
    norm3(&o.matrix.M21);
    o.matrix.M31 = lerpf(a.matrix.M31, b.matrix.M31, t);
    o.matrix.M32 = lerpf(a.matrix.M32, b.matrix.M32, t);
    o.matrix.M33 = lerpf(a.matrix.M33, b.matrix.M33, t);
    norm3(&o.matrix.M31);
    o.matrix.M41 = px; o.matrix.M42 = py; o.matrix.M43 = pz; o.matrix.M44 = 1.0f;

    o.curSpeed  = lerpf(a.curSpeed,  b.curSpeed,  t);
    o.destSpeed = lerpf(a.destSpeed, b.destSpeed, t);

    // Heading is a direction: component-lerp xyz then renormalize (keeps .w from the newer snapshot).
    o.heading.x = lerpf(a.heading.x, b.heading.x, t);
    o.heading.y = lerpf(a.heading.y, b.heading.y, t);
    o.heading.z = lerpf(a.heading.z, b.heading.z, t);
    norm3(&o.heading.x);

    // Blend factors are continuous, but only comparable when both snapshots are on the SAME action+channel
    // (they index that action's clips). Same action -> lerp for a smooth speed ramp; different action ->
    // keep b's (the alias we're switching to, copied by o = b) so we don't cross-blend unrelated clips.
    if (a.actionAlias == b.actionAlias && a.actionChannel == b.actionChannel && a.blendCount == b.blendCount) {
        for (int i = 0; i < b.blendCount; ++i) o.blendFactors[i] = lerpf(a.blendFactors[i], b.blendFactors[i], t);
        o.timeScale = lerpf(a.timeScale, b.timeScale, t);   // clip speed is continuous too
        // Phase: lerp unless the clip looped between snapshots (b < a), then keep b's (o = b) — never
        // interpolate backwards across the wrap.
        if (b.actionRatio >= a.actionRatio) o.actionRatio = lerpf(a.actionRatio, b.actionRatio, t);
    }
}

// Append a freshly-received packet. Dedupes by seq; a backwards seq (peer reconnect) resets the buffer.
static void PushSnapshot(const PlayerState& s, uint64_t now) {
    if (g_haveSnapSeq) {
        if (s.seq == g_lastSnapSeq) return;            // duplicate of the newest — GetRemote holds it
        if (s.seq <  g_lastSnapSeq) { g_snapCount = 0; g_snapHead = 0; }  // reset/reconnect
    }
    g_snaps[g_snapHead].st = s;
    g_snaps[g_snapHead].t  = now;
    g_snapHead = (g_snapHead + 1) % SNAP_MAX;
    if (g_snapCount < SNAP_MAX) ++g_snapCount;
    g_lastSnapSeq = s.seq; g_haveSnapSeq = true;
}

// Produce the interpolated state for render time (now - delay). Holds the newest state when render
// time runs past it (packet loss/stall) and clamps to the oldest before the buffer fills.
static bool ComputeInterpolated(uint64_t now, PlayerState& out) {
    if (g_snapCount == 0) return false;
    int newestIdx = (g_snapHead - 1 + SNAP_MAX) % SNAP_MAX;
    int oldestIdx = (g_snapHead - g_snapCount + SNAP_MAX) % SNAP_MAX;
    uint64_t renderT = now > (uint64_t)g_interpDelayMs ? now - (uint64_t)g_interpDelayMs : 0;

    if (g_snapCount == 1 || renderT >= g_snaps[newestIdx].t) { out = g_snaps[newestIdx].st; return true; }
    if (renderT <= g_snaps[oldestIdx].t)                     { out = g_snaps[oldestIdx].st; return true; }
    for (int i = 0; i < g_snapCount - 1; ++i) {
        int ai = (oldestIdx + i)     % SNAP_MAX;
        int bi = (oldestIdx + i + 1) % SNAP_MAX;
        uint64_t ta = g_snaps[ai].t, tb = g_snaps[bi].t;
        if (renderT >= ta && renderT <= tb) {
            float t = (tb > ta) ? (float)(renderT - ta) / (float)(tb - ta) : 1.0f;
            lerpState(g_snaps[ai].st, g_snaps[bi].st, t, out);
            return true;
        }
    }
    out = g_snaps[newestIdx].st; return true;         // shouldn't reach; be safe
}

// True while the engine is streaming/loading a world. Guards puppet construction so we never build an
// entity mid-load (EntityBuilder during a load transition is unsafe). Static read of g_LoadingThread.
static bool engineLoading() {
    __try { return LoadingManager::IsLoading(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Runs on the main thread once per frame (only for the player's Human update).
static void __cdecl OnFrame(Human* updatedHuman) {
    if (!isPlayerUpdate(base(), updatedHuman)) return;

    // Start the UDP link on the first REAL in-world frame, not at DLL attach: the launch chain's
    // transient processes bind the port and die (the live game's bind then fails with 10048 and
    // it gets no socket). They never reach a player frame, so binding here means only the live
    // game ever opens the socket, and there is no corpse socket to steal its datagrams.
    static bool s_netStarted = false;
    if (!s_netStarted) { s_netStarted = true; Net::Init(); }

    if (g_dumpReq) { g_dumpReq = 0; Phase1::DumpPlayerInfo(); }   // F7: works in net + local modes
    if (Net::Enabled()) {
        // Capture only succeeds in a real playable world (not title/whiteroom). Require it to be
        // stable for a bit before spawning, so we never spawn during loads/menus.
        static int      stable       = 0;
        static uint64_t lastFreshMs  = 0;   // last time a fresh same-world packet arrived
        static uint32_t lastRawSeq   = 0;   // last raw packet seq seen (to detect fresh packets)
        static int      despawnGrace = 0;   // frames the puppet has wanted removal
        PlayerState local;
        bool localOk = Phase1::CaptureLocalState(local);
        if (localOk) { static uint32_t seq = 0; local.seq = ++seq; Net::Send(local); if (stable < 100000) ++stable; }
        else stable = 0;

        uint64_t now = GetTickCount64();
        bool loading = engineLoading();   // engine streaming/tearing down a world

        // Pull the newest raw packet; track freshness (seq advanced) and whether it's in OUR world.
        PlayerState raw;
        bool gotRaw = Net::GetRemote(raw) && raw.valid;
        bool rawSameWorld = gotRaw && localOk && raw.worldId == local.worldId;
        if (gotRaw && raw.seq != lastRawSeq) {
            lastRawSeq = raw.seq;
            if (rawSameWorld) lastFreshMs = now;        // a fresh same-world packet keeps the puppet alive
            if (g_interpEnabled) PushSnapshot(raw, now);
        }
        // Peer is "present" only with a recent fresh same-world packet (covers world change + disconnect,
        // where GetRemote keeps returning a stale last packet forever).
        bool peerPresent = rawSameWorld && (now - lastFreshMs) < 2000;

        // ---- puppet lifecycle ----
        // Liveness by vtable: g_puppet still points at the entity we built only while its vtbl pointer
        // is unchanged. A world change FREES it (memory unmapped, or reused with a different vtbl) so we
        // drop the dangling ref; a pause/menu leaves it INTACT so we preserve it. This distinguishes
        // "gone" from "paused" without relying on whiteroom/world-id detection (the pause menu defeats
        // that — its active world reads as a different real world, which previously mis-dropped on pause).
        bool puppetAlive = false;
        if (g_puppet) {
            __try { puppetAlive = readable(g_puppet, 8) && (*(void**)g_puppet == g_puppetVtbl); }
            __except (EXCEPTION_EXECUTE_HANDLER) { puppetAlive = false; }
        }
        if (g_puppet) {
            if (!puppetAlive) {                          // entity was freed (world change / teardown)
                L("[Net] puppet entity gone -> drop ref\n");
                g_puppet = nullptr; g_blockReg = nullptr; g_puppetWorldId = 0; g_puppetVtbl = nullptr;
                g_lastRemoteCtx = -1;                    // fresh puppet next time -> re-apply first context
                g_lastActionAlias = 0xFFFFFFFF;
                g_bhvState = -1; g_switchCd = 0;
                g_casActivated = false;
                g_spawnTries = 0; g_spawnCd = 180;       // let the new world's builder settle before respawn
                g_snapCount = 0; g_snapHead = 0; g_haveSnapSeq = false;
                despawnGrace = 0;
            } else if (!localOk) {
                despawnGrace = 0;                        // pause/menu/load, puppet intact -> preserve
            } else if (peerPresent) {
                despawnGrace = 0;                        // healthy: our world, peer present
            } else if (++despawnGrace > 180) {          // ~3s peer absent while we're live -> remove
                Phase1::DespawnPuppet();                 // peer left our world / disconnected
                despawnGrace = 0; g_spawnTries = 0; g_puppetVtbl = nullptr;
                g_snapCount = 0; g_snapHead = 0; g_haveSnapSeq = false;   // reset interp buffer for next peer
            }
        }

        // ---- spawn + drive: only while stably in our world, peer present, not loading ----
        // (localOk is false during a pause/menu, so this is skipped then; skipping during a load also
        //  avoids touching the puppet while the world is tearing down / the builder is repopulating.)
        if (localOk && peerPresent && stable > 60 && !loading) {
            if (!g_puppet) {
                if (g_spawnCd > 0) --g_spawnCd;
                else {
                    // Keep retrying (throttled) rather than giving up after 5: after world travel the
                    // cached builder/selector globals may only become usable later. Each attempt is
                    // SEH-guarded + dup-balanced, so persistent retry is safe. If it never succeeds the
                    // globals are permanently stale (need the live-selector path).
                    ++g_spawnTries; g_spawnCd = 120;                 // ~2s between attempts
                    if (g_spawnTries <= 5 || (g_spawnTries % 15) == 0)
                        L("[Net] peer in world (id=%08X%08X) -> spawning puppet (try %d)\n",
                          (uint32_t)(local.worldId >> 32), (uint32_t)local.worldId, g_spawnTries);
                    Phase1::TrySpawnPuppet();
                    if (g_puppet) {                       // record identity for the liveness check
                        g_puppetWorldId = local.worldId;
                        g_lastRemoteCtx = -1;             // drive the peer's current context onto the new puppet
                        g_lastActionAlias = 0xFFFFFFFF;   // and the peer's current action
                        g_bhvState = -1; g_switchCd = 0;  // re-establish the new puppet's behavior state
                        g_casActivated = false;           // re-activate its CharacterActionSet
                        __try { g_puppetVtbl = *(void**)g_puppet; } __except (EXCEPTION_EXECUTE_HANDLER) { g_puppetVtbl = nullptr; }
                        L("[Net] spawn succeeded, g_puppet=%p\n", g_puppet);
                    }
                }
            }
            if (g_puppet) {
                // Render a delayed, interpolated state; with interp off, use the raw latest packet.
                PlayerState remote; bool haveRemote;
                if (g_interpEnabled) haveRemote = ComputeInterpolated(now, remote);
                else { remote = raw; haveRemote = gotRaw; }
                if (haveRemote && remote.valid && remote.worldId == local.worldId)
                    Phase1::ApplyRemoteState(remote);
            }
        }
        return;
    }
    if (g_spawnReq) { g_spawnReq = 0; Phase1::TrySpawnPuppet(); }
}

// Inline-hook detour: do our per-frame work, then reproduce the stolen instruction and jump back.
// esi holds the Human* being updated; preserved across our work by pushad/popad.
static __declspec(naked) void FrameDetour() {
    __asm {
        pushad
        pushfd
        push esi                // updatedHuman (esi = Human* being updated)
        call OnFrame
        add  esp, 4
        popfd
        popad
        mov  ecx, [esi+0x108]   // reproduce stolen instruction
        jmp  g_frameJmpBack
    }
}

// --- generic trampoline hook helpers ---
static void patchJmp(uintptr_t at, void* to, size_t len) {
    DWORD old = 0;
    VirtualProtect((void*)at, len, PAGE_EXECUTE_READWRITE, &old);
    *(uint8_t*)at = 0xE9;
    *(uint32_t*)(at + 1) = (uint32_t)((uintptr_t)to - at - 5);
    for (size_t i = 5; i < len; ++i) *(uint8_t*)(at + i) = 0x90;
    VirtualProtect((void*)at, len, old, &old);
}
// Copy stolenLen bytes of `target` into fresh RWX memory + jmp back; returns the callable original.
static void* buildTrampoline(uintptr_t target, size_t stolenLen) {
    uint8_t* t = (uint8_t*)VirtualAlloc(nullptr, stolenLen + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return nullptr;
    for (size_t i = 0; i < stolenLen; ++i) t[i] = *(uint8_t*)(target + i);
    t[stolenLen] = 0xE9;
    *(uint32_t*)(t + stolenLen + 1) = (uint32_t)((target + stolenLen) - (uintptr_t)(t + stolenLen) - 5);
    return t;
}

// BhvAssassin::AddToWorldInternal calls GetUnusedPadID, but on PC it hands back the ACTIVE pad
// (one "valid" pad, unused-search fails -> defaults to the active slot), so the puppet enrolls onto
// the real player's pad+gamer and hijacks it. Force a genuinely-empty slot for the puppet instead;
// since gamerID==padID, a distinct pad also makes GamerCollection ADD a new gamer rather than reuse.
typedef char (__fastcall *GetUnusedPad_t)(void* padMgr, void* edx, uint8_t* out);
static GetUnusedPad_t g_origGetUnusedPad = nullptr;

static char __fastcall MyGetUnusedPadID(void* padMgr, void* edx, uint8_t* out) {
    if (g_blockReg) {                                  // a puppet exists -> give it its own slot
        for (int i = 0; i < 8; ++i) {
            if (((void**)padMgr)[i] == nullptr) {
                *out = (uint8_t)i;
                char msg[64]; wsprintfA(msg, "[Phase1] GetUnusedPadID -> forced empty pad %d\n", i);
                OutputDebugStringA(msg);
                return 1;
            }
        }
    }
    return g_origGetUnusedPad(padMgr, edx, out);        // real player / fallback
}

static void InstallGetUnusedPadHook() {
    uintptr_t fn = base() + 0x344CF0;                  // PlayerPadManager::GetUnusedPadID
    g_origGetUnusedPad = (GetUnusedPad_t)buildTrampoline(fn, 6);
    patchJmp(fn, (void*)&MyGetUnusedPadID, 6);
    OutputDebugStringA("[Phase1] GetUnusedPadID hook installed\n");
}

// Keep the game running when it loses focus (so a backgrounded 2nd instance doesn't pause).
// sub_4054D0(active): active==0 is the focus-loss/deactivate path — swallow it.
typedef int (__cdecl *Deactivate_t)(char);
static Deactivate_t g_origDeactivate = nullptr;
static int __cdecl MyDeactivate(char active) {
    if (active == 0) return 0;           // ignore alt-tab deactivation
    return g_origDeactivate(active);
}
static void InstallNoPauseHook() {
    uintptr_t fn = base() + RVA_Deactivate;
    g_origDeactivate = (Deactivate_t)buildTrampoline(fn, 7);
    patchJmp(fn, (void*)&MyDeactivate, 7);
    OutputDebugStringA("[Phase1] no-pause-on-focus-loss hook installed\n");
}

// True once the local player entity is live in a real (non-whiteroom) world. Used to gate the spawn
// block: the player + essential/mission entities spawn while the player is still ABSENT (boot, world
// load, fast-travel, death respawn), so we must let those through or the load hangs. Ambient
// population (crowd/blob NPCs/guards/ships) spawns continuously AFTER the player is present, so we
// only decline spawns once the player exists.
static bool playerInWorld() {
    __try {
        World* world = World::GetCurrentWorld();
        if (!world)
            return false;

        if (inWhiteRoom(world)) return false;

        Entity* player = world->GetPlayer();
        if (!player)
            return false;

        void* h = AIEntityHelper::GetHuman(player);
        return readable(h, 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// (1) Universal spawn gate: decline SpawningRequest::Spawn (RVA 0x39B8D0) — the entry EVERY spawning
// strategy funnels through (crowd, blob NPCs, guards, sphere, naval) — but ONLY while the player is
// present (see playerInWorld). __thiscall(this), no stack args, returns char in al. Intercepted as
// __fastcall(this, edx) — the dummy edx absorbs the register thiscall leaves unused; `return 0` maps
// to al=0, the function's own "declined" path, which callers handle. Blocking unconditionally hung
// boot because the player/essential entities spawn through here too, hence the playerInWorld gate.
const uintptr_t RVA_SpawnRequestSpawn = 0x39B8D0;
typedef char (__fastcall *SpawnReq_t)(void* thisPtr, void* edx);
static SpawnReq_t g_origSpawnReq = nullptr;

static char __fastcall MySpawnRequest(void* thisPtr, void* edx) {
    if (g_disableSpawns && playerInWorld()) return 0;   // decline ambient spawns during live gameplay
    return g_origSpawnReq(thisPtr, edx);                // allow during boot/load/respawn (player absent)
}

static void InstallSpawnRequestHook() {
    uintptr_t fn = base() + RVA_SpawnRequestSpawn;
    // Steal 5 bytes: push esi; mov esi,ecx; mov eax,[esi] (56 8B F1 8B 06) — clean, no relative refs.
    g_origSpawnReq = (SpawnReq_t)buildTrampoline(fn, 5);
    patchJmp(fn, (void*)&MySpawnRequest, 5);
    OutputDebugStringA("[Phase1] spawn suppression hook installed (SpawningRequest::Spawn)\n");
}

// (2) Extra: no-op CrowdHerder::UpdateSpawningNeeds (RVA 0x1578B50), the type-4 crowd spawn issuer.
// __thiscall(this, fractionArray, needDelta) — 2 stack args, callee-cleaned (ret 8). Intercepted as
// __fastcall so ecx=this maps; the dummy edx absorbs the unused reg, so the two stack args land as
// a2/a3, and an early return compiles to `ret 8`. Stops the herd crowd before it builds requests.
const uintptr_t RVA_CrowdSpawnNeeds = 0x1578B50;
typedef void (__fastcall *CrowdSpawnNeeds_t)(void* thisPtr, void* edx, int fractionArray, float needDelta);
static CrowdSpawnNeeds_t g_origCrowdSpawnNeeds = nullptr;

static void __fastcall MyCrowdSpawnNeeds(void* thisPtr, void* edx, int fractionArray, float needDelta) {
    if (g_disableSpawns) return;   // block new crowd spawns entirely
    g_origCrowdSpawnNeeds(thisPtr, edx, fractionArray, needDelta);
}

static void InstallCrowdSpawnHook() {
    uintptr_t fn = base() + RVA_CrowdSpawnNeeds;
    // Steal 6 bytes: push ebp; mov ebp,esp; sub esp,4Ch (55 8B EC 83 EC 4C) — clean, no relative refs.
    g_origCrowdSpawnNeeds = (CrowdSpawnNeeds_t)buildTrampoline(fn, 6);
    patchJmp(fn, (void*)&MyCrowdSpawnNeeds, 6);
    OutputDebugStringA("[Phase1] crowd-spawn suppression hook installed (UpdateSpawningNeeds)\n");
}

void Phase1::RequestSpawn() { g_spawnReq = 1; }
void Phase1::RequestDump()  { g_dumpReq  = 1; }

// F7 dump (main thread): print the local player's Human* and every hop of the swim chain so the layout
// can be walked/verified in Cheat Engine. Each deref is readable()-guarded and the whole thing is
// SEH-wrapped, so it prints as far as the chain is valid and never faults the game. Swim fields only read
// sane once the player is actually IN the water (contextId 10); on the ground the swim context/FSM/scope
// may be null/unallocated — that's expected.
void Phase1::DumpPlayerInfo() {
    __try {
        World*  world  = World::GetCurrentWorld();
        Entity* player = world ? world->GetPlayer() : nullptr;
        Human*  h      = player ? AIEntityHelper::GetHuman(player) : nullptr;
        L("[Dump] world=%p player=%p HUMAN=%p\n", world, player, h);
        if (!readable(h, 0x50)) { L("[Dump] human not readable\n"); return; }

        L("[Dump]   contextId=%d  &m_HumanContextBundle=%p  groundBundle=%p\n",
          h->GetCurrentContextID(MAIN_AI_TRACK), &h->m_HumanContextBundle, h->m_Data.m_HumanDataBundle);

        void* swimCtx = nullptr; void* fsm = nullptr;
        HumanSwimScope* sc = swimScope(h, &swimCtx, &fsm);   // guards the m_Swim read internally
        L("[Dump]   m_Swim(HumanSwim)=%p  +0x30 FSM=%p  +0x0C scope=%p\n", swimCtx, fsm, sc);
        if (sc)
            L("[Dump]   scope+0xFF0 dir=(%.3f,%.3f,%.3f,%.3f)  +0x1000 spd=%.3f\n",
              sc->m_DestDirection.x, sc->m_DestDirection.y, sc->m_DestDirection.z, sc->m_DestDirection.w,
              sc->m_DestSpeedRatio);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        L("[Dump] EXCEPTION (code=0x%08X)\n", GetExceptionCode());
    }
}

void Phase1::InstallFrameHook() {
    g_driveLoco = GetPrivateProfileIntA("Coop", "DriveLocomotion", 0, ".\\AC4Coop.ini");
    g_driveContext = GetPrivateProfileIntA("Coop", "DriveContext", 0, ".\\AC4Coop.ini");
    g_driveActions = GetPrivateProfileIntA("Coop", "DriveActions", 1, ".\\AC4Coop.ini");
    g_disablePuppetAI = GetPrivateProfileIntA("Coop", "DisablePuppetAI", 1, ".\\AC4Coop.ini");
    g_hybridLoco = GetPrivateProfileIntA("Coop", "HybridLoco", 1, ".\\AC4Coop.ini");
    g_seekOnSwitch = GetPrivateProfileIntA("Coop", "SeekOnSwitch", 1, ".\\AC4Coop.ini");
    g_minBlendIn = GetPrivateProfileIntA("Coop", "MinBlendInMs", 100, ".\\AC4Coop.ini") / 1000.0f;
    // DriveContextMask: read as a string so hex ("0x440") or decimal both parse. Empty -> keep the safe
    // default (InAir+Swim). strtoul base 0 auto-detects the 0x prefix.
    {
        char maskStr[32] = {0};
        GetPrivateProfileStringA("Coop", "DriveContextMask", "", maskStr, sizeof(maskStr), ".\\AC4Coop.ini");
        if (maskStr[0]) g_ctxMask = (unsigned)strtoul(maskStr, nullptr, 0);
        L("[Phase1] DriveContext=%d ctxMask=0x%X\n", g_driveContext, g_ctxMask);
    }
    // DisableSpawns is the new key; fall back to the old DisableCrowd for back-compat.
    g_disableSpawns = GetPrivateProfileIntA("Coop", "DisableSpawns", 0, ".\\AC4Coop.ini")
                   || GetPrivateProfileIntA("Coop", "DisableCrowd", 0, ".\\AC4Coop.ini");
    g_interpEnabled = GetPrivateProfileIntA("Net", "Interpolation", 1, ".\\AC4Coop.ini");
    g_interpDelayMs = GetPrivateProfileIntA("Net", "InterpDelayMs", 100, ".\\AC4Coop.ini");
    InstallGetUnusedPadHook();
    InstallNoPauseHook();
    if (g_disableSpawns) { InstallSpawnRequestHook(); InstallCrowdSpawnHook(); }
    uintptr_t b = base();
    uint8_t* at = (uint8_t*)(b + RVA_HumanUpdate);
    g_frameJmpBack = (uint32_t)(b + RVA_HumanUpdate + HU_STEAL_LEN);
    DWORD old = 0;
    VirtualProtect(at, HU_STEAL_LEN, PAGE_EXECUTE_READWRITE, &old);
    at[0] = 0xE9;                                            // jmp rel32 -> FrameDetour
    *(uint32_t*)(at + 1) = (uint32_t)((uint8_t*)&FrameDetour - at - 5);
    at[5] = 0x90;                                            // nop the 6th stolen byte
    VirtualProtect(at, HU_STEAL_LEN, old, &old);
    OutputDebugStringA("[Phase1] frame hook installed on Human update (per-frame, main thread)\n");
}
