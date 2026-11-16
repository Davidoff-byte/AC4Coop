#pragma once
// [Coop] AttackForward (default 0, read once at load): a host guard's attack on the puppet is re-posted on the puppet's
// owner as a native FightDamageEvent from the owner's linked copy of that guard to the owner's own player, so the owner's
// engine resolves warning (phase 0: defence / counter, sub_185DA80) and hit (phase 1: sub_18644D0 block, parry, damage,
// hurt reaction) itself. Docs: docs/evidence/attack-forward-2026-11-16.md.
//
// The fields are the ones the engine's melee attack poster writes (sub_16551A0 + its init sub_18BD690, ida_batch fa2/fa3):
//   sub_18BD690(fde, type, subtype, f336, &dir(+0x30), &pos(+0x40), b331, b328, attacker)  thiscall, retn 20h
//   then +0x60 +0x146 +0x147 +0x110 +0x14A +0x142 +0x145 +0x149 +0x124 +0x128 +0x120(float) +0x50 +0x169 +0x143 +0x144
//        +0x11C +0x114 +0x130(dword) +0x134(byte) +0x5C, and +0x54 = phase (1 = hit).
// Handles (+0x4, +0x58 attacker, +0x118, +0xA4) and the embedded objects (+0x70, +0xD0, +0xD8) are NOT copied: the owner's
// own constructor (sub_10EFC30) and sub_18BD690 make them for the owner's entities.
#include <cstdint>

#pragma pack(push, 1)
struct AtkFields {
    uint8_t  phase, type, sub, b331, b328, b322, b323, b324, b325, b326, b327, b329, b330, b361, b308, b1C;
    uint32_t f336, f60, f5C, f50, f110, f124, f128, f120, f11C, f114, f130;
    float    dir[4], pos[4];
    int32_t  dmg;   // hit phase: the engine's base damage (sub_199A160, as the PDR relay scores it); the owner's fallback
};
#pragma pack(pop)

// player_damage_probe.cpp (the H1 hook's side)
bool AttackForward_On();                               // [Coop] AttackForward as read at load
void AttackForward_Capture(const void* fde, AtkFields& out);   // fill from a posted FightDamageEvent
// Post a native FightDamageEvent built from `a` (attacker = the owner's copy, victim = the owner's player). Game thread.
bool AttackForward_Post(void* victim, void* attacker, const AtkFields& a);

// phase1.cpp
void Phase1_PuppetAttackPosted(uintptr_t attacker, const AtkFields& a);   // host, H1 (any thread)
