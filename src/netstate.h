#pragma once
#include <cstdint>
#include "HookMath.h"

static const int BLEND_MAX = 46;   // blend-factor slots on the wire (ground move-cycle tree = SimpleAnimBlender<46>)

// Per-context drive blocks (see phase1.cpp Capture/Apply). Ground locomotion rides the common fields
// below; contexts whose state lives elsewhere carry it here, selected by PlayerState::contextId.
#pragma pack(push, 1)

// Swim (contextId 10): the swim locomotion input read/written on HumanSwimScope (see HumanSwimFSM.h).
// Plain floats (not HookMath::Vector4) to avoid 16-byte alignment inside the packed wire struct.
struct SwimBlock {
    float    destDir[4];         // HumanSwimScope::m_DestDirection (x,y,z,w)
    float    destSpeedRatio;     // HumanSwimScope::m_DestSpeedRatio (0 = idle, up to 1)
    float    curSpeedProportion; // HumanSwimScope::m_CurSpeedProportion (eased current speed, drives stroke blend)
    float    sprint;             // HumanSwimScope::m_Sprint (shift-held, 0 or 1)
    float    aiSwim[8];          // HumanSwimScope::m_AISwim (Wait/Turn/MoveVerySlow/Slow/Fast/CatchBreath/UnderWater/DivingBell)
    // CharacterController (biped) motion vectors — the actual velocity the FSM/anim graph read (0 on a
    // teleported puppet). Syncing these lets the puppet's swim state see "real" movement.
    float    ccVelocity[4];      // CharacterController::m_Velocity
    float    ccForce[4];         // CharacterController::m_Force
    float    ccImpulse[4];       // CharacterController::m_Impulse
};

union ContextBlock {
    SwimBlock swim;
    uint8_t   _reserve[128]; // stable footprint as future context blocks are added
};

// The full transferable player state (Front A proved these reproduce movement).
struct PlayerState {
    uint32_t magic;                 // NET_MAGIC
    uint32_t protoVer;              // NET_PROTO_VER — struct/wire-layout version (both peers MUST match)
    uint32_t seq;                   // sender frame counter
    uint64_t worldId;               // ManagedObject::GetObjectID(world) — must match to show a puppet
    uint32_t sceneKey;              // packed world-cell of the player (0 = unknown) — interior/exterior gate
    uint32_t sceneCellId;           // engine's own last-load cell id (diagnosis only)
    HookMath::Transform    matrix;  // global matrix (rows 0-2 rotation, row 3 translation)
    float    curSpeed;              // HumanGroundData.m_CurSpeedRatio
    float    destSpeed;             // HumanGroundData.m_DestSpeedRatio
    HookMath::Vector4    heading;   // HumanGroundData.m_DestHeading
    bool  isSet;                    // HumanGroundData.m_IsSet
    bool  sprint;                   // HumanGroundData.m_Sprint
    bool  breakHardOnEdge;          // HumanGroundData.m_BreakHardOnEdge
    bool  breakHard;                // HumanGroundData.m_BreakHard
    uint8_t  curMvt;                // HumanGroundData.m_CurMvtType
    uint8_t  destMvt;               // HumanGroundData.m_DestMvtType
    int32_t  contextId;             // AIActor::GetCurrentContextID(human, 1): active Human context on the main
                                    // AI track. 2=ground(default), 5=Ladder, 6=InAir, 7=Ledge, 8=Walling,
                                    // 10=Swim, 30=Climbing, 31=Hanging (shared engine enum, identical PS3<->PC).
    int32_t  subState;
    int32_t  humanGroundStage;

    bool     weaponTotallyOut;
    bool     isInTree;
    bool     isOnShip;
    bool     isOnOcean;
    bool     doJumpToVoid;
    int32_t  stalkingZoneCount;
    int32_t  stalkingZoneBlendedCount;
    int32_t  stalkingClipCount;
    int32_t  stalkingClipBlendedCount;
    int32_t  stalkingClipAllowFastStalkingCount;
    int32_t  disturbedZoneCount;

    uint32_t actionAlias;           // ActionComponent current action alias on ACTLAYER_PRIMARY (the anim clip
                                    // the peer is playing) — replayed on the puppet via SetTargetAction so it
                                    // animates like the peer regardless of context. 0 = none.
    uint32_t actionChannel;         // the action's BodyPartChannel id (*(u16*)(*(Action+0x0C)+8)). Passed to
                                    // SetTargetAction as the channel instead of 0xFFFF: 0xFFFF makes the
                                    // CharacterActionSetManager re-derive it via a kit lookup that bails (no
                                    // CAS override -> Connor's base clip); the real channel takes that path.
    int32_t  actionLayer;           // the action's ACTLayer (Action::m_Layer @ 0x10); passed as the
                                    // SetTargetAction layer instead of a hardcoded ACTLAYER_PRIMARY.
    uint8_t  blendCount;            // valid slots in blendFactors (the primary controller's array size, clamped)
    float    blendFactors[BLEND_MAX];      // ch0/PRIMARY controller blend factors — the FULL array. The ground move
                                    // cycle is a 46-slot blend tree (SimpleAnimBlender<46>); the walk/jog/run
                                    // gait lives past slot 3, which is why syncing only 4 slots stayed walking.
    float    actionRatio;           // current action's normalized playback position [0,1] (GetCurrentActionRatio
                                    // ch0/PRIMARY). On an action switch the puppet seeks to it so the new clip
                                    // starts in phase instead of at frame 0 (the "snappy" swaps).
    float    blendInLen;            // peer's current evaluator blend-in length (m_BlendInEvaluator.m_ActionBlend.
                                    // m_Length). FSM-generated transitions (crouch->stand etc.) carry a custom
                                    // ActionTransition our transition-less SetTargetAction can't find (-> default
                                    // 0.2s = snappy); we copy this onto the puppet's new evaluator. <0 = unknown.
    float    blendInStartFromEnd;   // ...m_ActionBlend.m_StartTimeFromEnd
    float    blendInTargetStart;    // ...m_ActionBlend.m_TargetStartTime
    uint16_t blendInFlags;          // ...m_ActionBlend.m_Flags (low 3 bits = m_BlendType). The engine's default for
                                    // a transition-less SetTargetAction is flags 0x600 = ACTBLENDTYPE_NONE -> hard cut,
                                    // so the TYPE must be copied too, not just the length.
    uint8_t  isTransitionAction;    // peer's current evaluator is a transition clip (diagnostic)
    float    timeScale;           // ActionComponent primary-channel playback time scale (clip speed). The
                                    // loop's foot-speed rides this; without it the puppet plays the right clip
                                    // slow. Default 1.0 (NEVER 0 — that freezes the anim). GetTimeScale/SetTimeScale.

    // Context-tagged payload: the active context's own drive data, selected by `contextId`. Each context
    // whose motion/animation isn't covered by the common ground fields gets a block here; the reserve
    // keeps the packet size stable as blocks are added so both ends stay wire-compatible. Only the block
    // matching `contextId` is meaningful on any given packet.
    ContextBlock ctx;

    // ---- ship sync (filled when the player is aboard their ship) ---------------------
    bool     shipValid;    // player is aboard a ship -> the fields below are meaningful
    float    shipPos[3];   // ship world position (x,y,z) = m_GlobalMatrix.M41/42/43
    float    shipQuat[4];  // ship rotation quaternion (x,y,z,w), from the 3x3 at +0x10
    float    shipHP;       // AUTHORITATIVE ship hull HP for the player lane: *(float*)(GcLShip+0xD0)
                           // (max at GcLShip + 0xB4), read through the object's vtable slot 0x1B4 and
                           // applied through slot 0x12C. Verified - see ops/evidence/hull-hp-found.md.
                           // (The entity lane's legacy int copy, EntityState::hullHP, was removed at proto 11.)
    float    shipMaxHP;    // peer's ship MAX hull HP: *(float*)(GcLShip+0xB4). Unlike shipHP this is a
                           // class-level value (one writer, vtable slot 0x130), i.e. the ship's identity
                           // fingerprint as far as damage behaviour goes. It rides the wire so the guest
                           // can choose WHICH local ship to duplicate for the ghost (proxy route): the
                           // selection keys on {maxHP, subEntityCount, flags} - see
                           // ops/evidence/ghost-identity-implementation.md. 0.0f = unknown (not aboard,
                           // or the GcLShip could not be resolved).
    uint32_t shipClass;    // peer's ship CLASS index, read off the ship ENTITY (not GcLShip): exactly the
                           // engine's sub_11AA850 -  f = *(u32*)(entity+0x9C);
                           //   return ((f & 7)==3 && (f & 0x1F8)==0xE8) ? ((f>>9)&0x3F) : 0;
                           // The tag/subtype gate is mandatory: without it +0x9C is arbitrary data
                           // (ops/evidence/ship-type-entity-field.md). Mapping (sub_1045CD0,
                           // ops/evidence/ship-class-index.md): 1 GunBoat 2 Schooner 3 Brig 4 Frigate
                           // 6 ManOfWar 7 Jackdaw 8-11 SuperShip 12 Qar 13 Revenge 14 Begin
                           // 15 NossoSenhor 16 Eldorado; 0 = gate failed / unknown. Lets the guest
                           // proxy match the peer's class EXACTLY instead of by nearest hull tier.

    uint8_t  valid;                 // 1 = a live player was captured
};
#pragma pack(pop)
// Machine-checked wire-layout guard: pack(1) caps natural alignment at 1, but the align(16) members
// (Transform, Vector4) keep 16-byte alignment, so the struct alignment is 16. shipClass (u32) lands at
// +560 inside what was 15 B of tail pad, so adding it does NOT grow the struct: still 576 B. The peer9
// model and ops/tools/wire_model_test.py assert the same number; if any field here changes the size,
// this fails the build and the peer/tools must move in lockstep.
static_assert(sizeof(PlayerState) == 576, "PlayerState wire size changed - update peer9.py and wire_model_test.py");

static const uint32_t NET_MAGIC = 0x43344341; // 'AC4C'
// Wire/struct-layout version. PlayerState is #pragma pack(1): adding a field shifts every following
// offset, so both peers MUST run the same build. Bump this whenever the layout changes.
static const uint32_t NET_PROTO_VER = 14;  // 14: entity header += u32 hostMs (36 B); PlayerState, LobbyPacket and
                                           //     the record format unchanged
                                           // 13: guard animation block (EF_ANIM, GuardAnim) on the entity lane;
                                           //     PlayerState and LobbyPacket unchanged
                                           // 12: lobby handshake (LobbyPacket below); PlayerState/entity layouts unchanged
                                           // 11: entity chunking + worldId, hullHP removed, shipMaxHP
                                           // (GcLShip+0xB4) + shipClass (entity+0x9C) added; 10: float shipHP (GcLShip+0xD0)

// ---- lobby handshake (docs/design/lobby-code.md, slice 3) ---------------------------------------
// [Net] Mode=host|join only. The guest sends HELLO to the address in its lobby code until WELCOME;
// the host locks g_peer to the first HELLO's source (IP AND port, the guest's NAT mapping) and
// replies WELCOME. A HELLO with a foreign protoVer gets REJECT carrying the host's version.
// This 20-byte layout is FROZEN across protocol versions: it is how two different builds find out
// they differ, so it must parse on both. Never add a field; use a new magic instead.
static const uint32_t NET_LOBBY_MAGIC = 0x4C344341; // 'AC4L'
enum LobbyType : uint32_t {
    LOBBY_HELLO   = 1,   // guest -> host, every 250 ms until WELCOME/REJECT or 15 s
    LOBBY_WELCOME = 2,   // host -> guest, echoes the guest's nonce
    LOBBY_REJECT  = 3,   // host -> guest, protoVer mismatch (protoVer field = the host's version)
    LOBBY_PUNCH   = 4,   // host -> guest's endpoints (relay rendezvous): opens the host's NAT towards the
                         // guest; carries nothing, every receiver ignores it (docs/design/relay.md)
};
#pragma pack(push, 1)
struct LobbyPacket {
    uint32_t magic;       // NET_LOBBY_MAGIC
    uint32_t protoVer;    // sender's NET_PROTO_VER
    uint32_t type;        // LobbyType
    uint32_t nonce;       // chosen by the guest per join attempt; WELCOME/REJECT echo it
    uint32_t codeCheck;   // HELLO: low 12 bits = the check of the code the guest typed. Logged, not
                          // enforced: the code carries no secret (lobby-code.md "The code").
};
#pragma pack(pop)
static_assert(sizeof(LobbyPacket) == 20, "LobbyPacket is a frozen wire layout");
// Why adding shipMaxHP (and now shipClass) to 11 needs NO further bump: 11 has never run on a second
// machine (no released build speaks it), so there is no deployed proto-11 peer to stay compatible with.
// The layout guard is already structural - the receive path requires n == sizeof(PlayerState), so a
// stale build is rejected rather than mis-parsed; bumping again would only rename the number with
// nothing on the other end.

// ---- entity sync (host-authoritative "synced worlds") -------------------------------
// One replicated entity's transform. `key` is the index in the host's enumeration; `kind` (EntityKind
// below) names the list that index is into: 1 = ship (filtered from m_EntityManager.m_Entities by
// vtable 0x01E4A128), 2 = guard (soldier), 3 = one-shot kill event (not a list index).
// pos = m_GlobalMatrix.M41/M42/M43; quat = the 3x3 rotation (HookMath::FromMatrix).
static const uint32_t NET_ENTITY_MAGIC = 0x45344341; // 'AC4E'
static const int      MAX_SYNC_ENTITIES = 128;

// The entity kind carried by EntityState::kind and by the compact wire record. This is the ONLY
// definition: 0 is reserved and no lane may publish it (net.cpp rejects it with DEC_KIND, as it does
// any value above EK_KILL), so the receive side can never hand an unknown kind to the apply path.
enum EntityKind : uint8_t {
    EK_UNDEFINED = 0,   // reserved / undefined - rejected on receive, never sent
    EK_SHIP      = 1,   // ship        -> phase1.cpp ApplyShips
    EK_GUARD     = 2,   // guard NPC   -> phase1.cpp ApplyGuards
    EK_KILL      = 3,   // one-shot death event -> ProcessKillEvents / Net::GetKillEntities
};

#pragma pack(push, 1)
// Proto 13: the animation state of a guard, read on the host with the same engine calls CaptureLocalState uses
// for the player (phase1.cpp CaptureGuardAnim) and applied on the guest the way ApplyRemoteState drives the
// puppet. Quantised for the wire; `valid` = 0 means "not captured" and is never sent.
static const int GUARD_ANIM_BLEND_MAX = 16;   // non-zero blend factors carried per guard (the 46-slot ground tree
                                              // has 1-8 non-zero slots in every capture logged so far)
struct GuardAnim {
    uint8_t  valid;           // 1 = captured on the host
    uint16_t channel;         // Action::m_BodyPartChannel (*(u16*)(*(act+0x0C)+8)); 0xFFFF = unknown
    int16_t  layer;           // Action::m_Layer (*(i32*)(act+0x10))
    uint16_t ratioQ;          // GetCurrentActionRatio(0, PRIMARY) * 65535, clamped [0,1]
    uint16_t timeScaleQ;      // GetTimeScale(0, PRIMARY) * 256 (q8.8), clamped [0, 255.99]
    int16_t  blendInMs;       // current evaluator +0x1C8 (blend-in length, s) * 1000; -1 = none
    uint16_t blendInFlags;    // evaluator +0x1DC
    float    blendInStartFromEnd;   // evaluator +0x1CC
    float    blendInTargetStart;    // evaluator +0x1D0
    uint8_t  nBlend;          // entries used in blendSlot/blendVal
    uint8_t  blendSlot[GUARD_ANIM_BLEND_MAX];   // controller slot index (GetBlendFactor(i, 0, PRIMARY))
    uint8_t  blendVal[GUARD_ANIM_BLEND_MAX];    // factor * 255, rounded (factors are clamped to [0,1])
};
// Wire size of the EF_ANIM block: 2+2+2+2+2+2+4+4+1 fixed, then nBlend x (slot u8, value u8).
static const int GUARD_ANIM_WIRE_FIXED = 21;

struct EntityState {
    uint32_t key;          // index in the source list selected by `kind`
    uint8_t  kind;         // EntityKind: 1 = ship, 2 = guard, 3 = kill. 0/reserved is NEVER valid here.
    uint8_t  dead;         // death state: 1 = dead (*(*(e+0xE8)+0x1C)+0xB8 & 0xC == 4)
    uint8_t  stage;        // HumanGroundStage (Fight=4 = aggro, Navigation=1 = calm)
    uint8_t  _pad;
    float    pos[3];       // world position (x,y,z)
    float    quat[4];      // world rotation (x,y,z,w)
    uint32_t actionAlias;  // current action clip (file alias); 0 = none (guards only)
    uint64_t objId;        // host-assigned stable identity (HandleContainer::m_ObjectID); 0 = unknown
    uint16_t hp;           // host-authoritative Life (guards); 0xFFFF = dead
    uint16_t arch;         // archetype id at behaviour+0x14: lets the peer match class-to-class
    GuardAnim anim;        // proto 13: guards only (EF_ANIM)
};

struct EntityPacket {
    uint32_t magic;        // NET_ENTITY_MAGIC
    uint32_t protoVer;     // NET_PROTO_VER (both peers MUST match)
    uint32_t seq;          // sender frame counter
    uint32_t hostMs;       // proto 14: sender capture time (EntityWireHeader::hostMs); 0 = not stamped
    uint32_t count;        // valid entries in ents[]
    EntityState ents[MAX_SYNC_ENTITIES];
};
#pragma pack(pop)

// ---- Phase 2: compact entity wire ------------------------------------------------------------
// EntityPacket above remains the canonical DECODED form, so every consumer (ApplyShips/ApplyGuards/
// ProcessKillEvents) is unchanged. On the wire each entity is:
//     u32 key, u8 kind, u8 idKind, u16 fieldMask, u64 wireId,     <- 16-byte fixed part
//     then only the fields whose bit is set (fixed order below).
// `wireId` carries the entity's stable identity: the real objId when idKind == 0, otherwise a value
// synthesised from kind+key. A field is omitted when it equals the sender's shadow copy, so an
// UNCHANGED entity is omitted entirely (0 bytes) and a changed one costs at least 16 bytes - never
// the 7 the old prose claimed (there is an idKind byte and an 8-byte wireId in the fixed part).
// `count` keeps its old meaning: it is the number of element records the caller captured / the
// number written on the wire, not the number of field groups.
// Proto 11 (X15/X5): the header now also carries `worldId` (the sender's world; a mismatched snapshot
// is dropped) and `chunkIndex`/`chunkTotal`, so a snapshot whose encoded records exceed 1400 B is split
// into several UDP datagrams and reassembled on the receive side (a partial set is never applied).
// Old (proto <= 10) peers are rejected by the protoVer check rather than mis-parsed (both lanes).
enum EntityField : uint16_t {
    EF_POS    = 1u << 0,   // float pos[3]
    EF_QUAT   = 1u << 1,   // float quat[4]
    EF_ACTION = 1u << 2,   // u32 actionAlias
    // bit 3 retired: EF_HULLHP (i32 hullHP) was captured+transmitted and never read (authority-model
    // C3). Removed at proto 11; bit 3 is NEVER reused by a future field.
    EF_HP     = 1u << 4,   // u16 hp
    EF_ARCH   = 1u << 5,   // u16 arch
    EF_DEAD   = 1u << 6,   // u8  dead
    EF_STAGE  = 1u << 7,   // u8  stage
    EF_OBJID  = 1u << 8,   // u64 objId - declared only so the set is complete. NOT a legal mask bit:
                           // objId rides the record's wireId (idKind == 0), and net.cpp rejects a
                           // mask containing this bit.
    EF_ANIM   = 1u << 9,   // proto 13: GuardAnim block, GUARD_ANIM_WIRE_FIXED + 2*nBlend bytes
    EF_ALL    = 0x03F7u,   // every live bit (bit 3 retired). A LEGAL wire mask is
                           // EF_ALL & ~EF_OBJID == 0x02F7; never send EF_ALL itself.
};

#pragma pack(push, 1)
struct EntityWireHeader {
    uint32_t magic;       // NET_ENTITY_MAGIC
    uint32_t protoVer;    // NET_PROTO_VER
    uint32_t seq;         // sender frame counter; every chunk of one snapshot shares this seq
    uint32_t epoch;       // shadow generation: both peers drop their delta base when this changes
    uint32_t count;       // number of records in the FULL snapshot (all chunks together)
    uint64_t worldId;     // sender's World::GetObjectID(); the receiver rejects a snapshot whose
                          // world differs from the local one (world-transitions X5).
    uint16_t chunkIndex;  // this chunk's 0-based index
    uint16_t chunkTotal;  // chunks in this snapshot (1 = single-chunk, the common case)
    uint32_t hostMs;      // proto 14: sender's capture time (QPC ms, wraps at 2^32); the receiver buffers and
                          // interpolates on it instead of on its own receive/tick time
};
#pragma pack(pop)
static const int ENTITY_WIRE_HEADER_SIZE = 36;
// Fixed part (key+kind+idKind+mask+wireId = 16) + every optional field. The true worst case is
// 16 + 38 = 54 (objId is the wireId inside the fixed part, not an extra field); 62 is deliberate
// headroom - do not "correct" this constant below 54.
static const int ENTITY_WIRE_RECORD_MAX  = 16 + 46 + GUARD_ANIM_WIRE_FIXED + 2 * GUARD_ANIM_BLEND_MAX;   // proto 13
