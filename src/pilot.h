#pragma once
// [Pilot] Enabled=1 (default 0): drive Edward from outside the game without touching the user's desktop, and publish
// what is around him every frame. Design follows AU-RW's ACU focus shim / native probe (in-process virtual pad,
// focus keeper, shared-memory status); this is an independent implementation for AC4 x86.
//
//  - Virtual pad: the game's XInput imports (xinput1_3 ordinals 2 GetState, 3 SetState, 4 GetCapabilities) are
//    patched in the IAT. Pad [Pilot] PadIndex (default 0) reports "connected" from process start; its state comes
//    from the shared section Local\ac4pilot-pad-<pid> (PilotPad below) while `enabled` is non-zero, otherwise it is
//    neutral. No game code bytes are changed. Refused when [Coop] VPadEnable=1 (that hook owns the same import).
//  - Focus keeper ([Pilot] KeepFocus=1, default 0): GetForegroundWindow / GetActiveWindow (IAT) return the game
//    window and the window procedure drops WM_ACTIVATEAPP(FALSE), WM_ACTIVATE(WA_INACTIVE) and WM_KILLFOCUS, so the
//    game keeps running unfocused. The user's mouse and keyboard are never blocked or forged.
//  - State: on every PLAYER Human frame (game thread, phase1 OnFrameInner) the player entity, its world matrix and
//    every entity of World::m_CharacterList (position, +0x9C kind/role word, AI component, fighter record status/role)
//    are written to Local\ac4pilot-state-<pid> (PilotState) under a sequence lock.
//  - Control (Local\ac4pilot-ctl-<pid>, PilotCtl): teleport the player (game thread, Entity::SetGlobalMatrixInternal)
//    and set a frame cap. The cap paces IDXGISwapChain::Present (vtable slot 8 of the swap-chain class, found through a
//    throwaway swap chain, as in AU-RW's ACU focus shim); [Pilot] FpsCap=N (default 0 = off). A background run without a
//    cap stalled the owner's PC in ACU (AU-RW README), so the client's boot sets one.
// Client: tools/pilot.py (status, pad, boot, goto, scan, mem, teleport, fps).
#include <cstdint>

namespace Pilot
{
#pragma pack(push, 1)
    struct PilotPad
    {
        uint32_t magic;      // 'AC4D' = 0x44344341, written by the game when the section is created
        uint32_t enabled;    // client: non-zero = serve this state, 0 = neutral pad
        uint32_t seq;        // client: bump on every change (becomes XINPUT_STATE.dwPacketNumber)
        uint16_t buttons;    // XINPUT_GAMEPAD.wButtons
        uint8_t  lt, rt;     // triggers 0..255
        int16_t  lx, ly, rx, ry;
        uint32_t reserved[4];
    };

    struct PilotChar
    {
        uint32_t ent, aic, rec;   // entity, *(ent+0xE8), *(aic+0x98)
        float    pos[3];          // m_GlobalMatrix M41..M43
        uint32_t w9c;             // *(ent+0x9C): kind = &7, role = (&0x1F8)>>3
        int32_t  status, role;    // rec+0x2C (FighterStatus), rec+0x18 (-1 when no record)
        uint32_t flags;           // 1 = player
    };

    enum { kMaxChars = 256 };

    struct PilotState
    {
        uint32_t magic;           // 'AC4S' = 0x53344341
        uint32_t version;         // 1
        volatile uint32_t seq;    // odd while the game thread writes, even when stable
        uint32_t frame;           // player frames written
        uint32_t tickMs;          // GetTickCount() of the last write
        uint32_t world, player, human;
        float    m[16];           // player m_GlobalMatrix, row-major, translation in m[12..14]
        uint32_t nChars, nTotal;  // written / size of m_CharacterList
        uint32_t padPolls, padServed, padIndex, focusDrops;
        PilotChar chars[kMaxChars];
    };
    // Commands from the client, executed by the game (Local\ac4pilot-ctl-<pid>). The client fills cmd/f[] and then bumps
    // `req`; the game sets status and copies req into `done` when finished.
    enum { kCmdTeleport = 1, kCmdFpsCap = 2 };
    struct PilotCtl
    {
        uint32_t magic;           // 'AC4C' = 0x43344341
        volatile uint32_t req, done;
        uint32_t cmd;
        int32_t  status;          // 0 ok, -1 no player/world, -2 exception, -3 unknown command, -4 not available
        float    f[8];            // teleport: x, y, z, yaw (radians, NaN = keep); fps cap: f[0]
        uint32_t fpsCap;          // current cap (0 = off), [Pilot] FpsCap at start
        volatile uint32_t presents, fps;   // IDXGISwapChain::Present calls / measured rate (when the Present patch is in)
        uint32_t presentPatched;
        uint32_t reserved[8];
    };
#pragma pack(pop)

    void Init();                        // DllMain attach, after the build gate
    void Poll();                         // poll thread, ~20 Hz: retries the IAT patch and the window subclass
    void OnPlayerFrame(void* human);     // game thread, once per player Human update
}
