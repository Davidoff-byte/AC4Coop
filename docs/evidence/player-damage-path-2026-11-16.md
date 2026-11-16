# Player damage path: a strike landing on a Human, through to the player's life write (static, 2026-11-16)

Static RE only. AC4BFSP.exe md5 `2058342866688f780c8b34526a65bc35`, headless idat 9.3 on a private copy of the `.i64`
(DBFL_KILL, nothing saved). Every VA below was read with `ida_batch.py` (disasm / decompile / xrefs / bytes) in this
session. Each claim quotes the instruction or decompile line it rests on. Hypotheses are marked **UNVERIFIED**.
Nothing here was probed live. AC4BFMP.exe was **not** opened.

## Q1. The chain for the real player (the strike, then the life write)

Class names come from the classes' own name strings (vtable slot getter, then class info, then descriptor `name`):
`0x269BBF8` = **CSrvPlayerHealth**, `0x26979F0` = **CSDamage**, `0x26F8DF8` = **CSHumanFightInteractions**,
`0x2682278` = CSAnimalFightInteractions. Both fight classes derive from **CSNPCFightInteractions** (base id 0x8F9486B3,
ctor `0x1995810`, called from `0x1809B99` and `0xF79319`).

| # | VA | what it does (quoted) |
|---|---|---|
| 1 | `0x64A020` / `0x64D8C0` | Event post: `mov edx,[eax+24h]; call edx` (event->vt[9](receiver) = `0x64A080` → `0x5220E0`), then `test eax,eax; jz` (0x64A035 / 0x64D8D8). If a listener exists, the event is queued on global `dword_2ABEAC4`. `0x5220E0`: `add ecx,0ECh; call sub_5210A0` (component lookup, class info `off_279BF68`, cached at entity+0xEC), `add eax,0Ch`. The FightDamageEvent (FDE) is posted to the victim (f7 evidence). |
| 2 | `0x186B370` | **CSHumanFightInteractions** vtable slot 18 (`0x26F8E40`). `mov eax,[esi+54h]; test eax,eax; jnz` : FDE+0x54 == 0 → `sub_185DA80(FDE)`; == 1 → `sub_18644D0(this, FDE)`. The FDE-keyed listener lives at CSNPCFightInteractions+0x5C (ctor `*(this+23) = off_270B1DC`, id getter `0x197C7A0` → `return 334741081` = 0x13F3BE59). |
| 3 | `0x18644D0` | Hit resolution on the victim. Victim = `*(this+0x74)+8` (`v5 = *(a1+116)`). Early returns: `+0x74 → +0x24` null, `vt[72](96)` null, victim null or `victim[23]` (entity+0x5C) null (decompile lines 143-155). It handles player victims explicitly: `(v135[39] & 7) == 1` (entity+0x9C kind 1). Damage = `v131 = *(a3 + 12)` (FDE+0xC), recomputed by `sub_199A160(...)` (line 357). Then `sub_1998D40(a3, v131, v124)` (line 615) or `sub_1998D40(a3, v105, 0)` (line 783). |
| 4 | `0x1998D40` | Builds the **DamageEvent**. `__userpurge`: ecx = victim-side component, `[ebx+8]` = FDE, `+0xC` = final damage, `+0x10` = skip flag; `retn 0Ch` at 0x19990BE. If `!a5`: `v21 = sub_1029240()` (new 0x28 DamageEvent, ctor `0x1B70940`, vtable `0x27214B8`), then `sub_1B86E40(contact=FDE+0x20, damage, attacker=FDE+0x58 handle, FDE+0x1C bit0, bit1, 35, 0, 0)`. It copies FDE+0x10 to DE+0x10 and FDE+0x18 to DE+0x18, then holds `*(entity+0xD4)` (CS) around `mov ecx,esi; call sub_64D8C0` (0x19990A6) with receiver = `sub_702660(ecx)`. |
| 5 | `0x173E190` | **CSDamage** listener body. Its delegate sits at CSDamage+0x5C (ctor `0x1137370`: `sub_1725460(a1+23)`, vtable `0x26EE204`, slot 2). The listener sub-object's id getter `0x1116510` returns `1743972456` (DamageEvent). Gates: `cmp eax,67F2E868h` (DamageEvent) / `cmp eax,9B804F9Ch` (ExplosionDamageEvent), else `jnz 0x173E770` (0x173E1C2). Victim `esi = [[ebx+4]+8]`, `jz` (0x173E1D0). `cmp [esi+5Ch],0; jz` (0x173E1DA). `Entity_IsAlive(esi)` or `sub_15CFF50(esi)`, else `sub_133F5A0` and return (0x173E1FB). Handle `var_14 = [[ebx+4]+1Ch]` (0x173E25A). Current life comes from `sub_18B9EA0(&var_14,&var_20)` (0x173E397). |
| 6 | `0x173E7C0` | Damage computation = delegate vt[0] (`call edx` at 0x173E3C5). `thiscall(this, event, victim, bool* killed, int* newLife, bool)`, `retn 14h`. Attacker = event+4 handle (`v11`). Gate `sub_133F630(victim, attacker, ev+0x20, ev+0x10, a6)`: if it is false, newLife = current. Damage `v14 = ev+0xC`, recomputed by `sub_199A160` only when an attacker exists and attack type (ev+8) is not 35. **Player branch** at 0x173E994: `mov eax,[ecx+9Ch]; and al,7; cmp al,1; jnz 0x173EA7C` → `sub_5220D0(victim)` (component cached at entity+0xE3, class `off_2752DE0`), `+0x1C` = `sc`; `sub_1482400(&sc, dmg, &lethal)` (0x173E9BB). If lethal, `*newLife = -1` and `*killed = 1` (0x173E9EC, 0x173EA39), unless `var_1` or attack type 27/29. Otherwise `sub_1482210(&sc, dmg, newLife, ev+0x20 != 15)` (0x173EA68) and `*killed = 0`. |
| 7 | `0x1482400` / `0x1482210` | Statechart events `dword_4DF9278` / `dword_4DF926C` (handler loop `sub_A00CB0` + `sub_9F6260`). In the CSrvPlayerHealth handler table (pairs handler / `&eventId` at `0x28CE240..`), these ids are copied into `dword_4DE7FE0` / `dword_4DE7F84` (`sub_114FAB0`). |
| 7a | `0x1148A60` | lethal query: `*a3 = GetLife(this[3]) < a2`. |
| 7b | `0x1172F70` | **The player damage amount** (non-lethal). Skipped if `health+0x62 bit0`. Armour items lose durability (`sub_19DB860(3,…)`, `v11 - a2`), a pad rumble call (`vt+272(2, …)`). Then `*a3 = Life - a2`, clamped to `[0, GetMaxLife - *(health+0x64)]`; returns 2. **It never produces a negative value**: death only comes from 7a. |
| 8 | `0x173E76B` (also 0x173E587) | `mov edx,[ebp+var_10]; push edx; lea ecx,[ebp+var_14]; call sub_18BA200`. Posts the new absolute life. |
| 9 | `0x18BA200` | SetLife request. Prologue `53 8B DC` (aligned frame); `mov eax,[ecx]; test eax,eax; jnz` else `mov eax,3 … retn 4`. Then `mov ecx, dword_4E20518` … `call sub_A00CB0`, loop `call dword ptr [ebp-10h]` with `push [ebx+8]` (the value), `retn 4`. |
| 10 | `0x11488D0` | CSrvPlayerHealth handler for `dword_4DE7FC0` (= `dword_4E20518`, `sub_114FAB0`). Table pair `011488D0 04DE7FC0` at `0x28CE2A0`. `mov ecx,[ecx+0Ch]; push eax; call sub_1C08EE0` (return address 0x11488DF = the f7 positive-control caller). |
| 11 | `0x1C08EE0` | `(*(this)+0x40)(this, v)`, then if `v > word[+0x60]`, clears `+0x62 bit3`. |
| 12 | `0x115ACC0` | **CSrvPlayerHealth::SetLife**. Vtable slot 16 (`0x269BC38`, the only xref). Detailed in Q3/Q4. |

**Static only**: that guards' hits on Edward take steps 2→12 is shown by the code, not by a probe. Steps 10-12 match
the f7 live control (return address 0x11488DF, 20 per hit). Steps 2-9 were not hooked live.

**Best receiving-side interception point (before the life write):** `0x1998D40`. It has, at one site:
(a) the victim (`sub_702660(ecx)`, the `[ebp-0Ch]` pushed at 0x19990A3);
(b) the **final damage** after `sub_199A160` and block/parry resolution (`[ebx+0Ch]`);
(c) the attacker (FDE+0x58 handle, read at the `sub_1B86E40` call);
plus contact type (FDE+0x20), subtype (+0x10) and flags (+0x1C).
It sits before any DamageEvent exists. Returning early for a puppet victim creates no DamageEvent and applies no life
change, and the real player passes through untouched. Second choice: `0x173E7C0` (vt[0], `retn 14h`), where
`*newLife = current; *killed = 0` would suppress damage. That only helps if the puppet gets that far.

## Q2. Where the chain stops for the puppet

Static facts that narrow it:
- **CSDamage ignores FightDamageEvent.** `sub_A1D0D0` = `*(vt[5](this)+20)`. For an FDE, vt[5] `0x18A61C0` returns
  class info `0x2A23C20`, and `+0x14` = `0x2A23C34` = **0x13F3BE59**. That matches neither `cmp` at 0x173E1AF / 0x173E1BD,
  so the FDE goes straight to 0x173E770. Health only changes through a separate **DamageEvent** posted by step 4.
- So the f7 observation ("102 FDE posts to the puppet, no life write") means the puppet never reached step 4 (no
  DamageEvent was posted), **or** it reached step 4 and the later steps failed. The two DamageEvent posts go through
  vt[9] too (`0x27214B8` slot 9 = `0x64A080`; `0x64D8C0`: `mov edx,[eax+24h]; call edx`). So a DamageEvent post to the
  puppet would have hit the f7 hook as vtable **0x027214B8**. The f7 doc reports only FDE vtables and does not state the
  DamageEvent count; the raw log of run 22156 was not found. **UNPROVEN** which branch applies.
- Candidate stop branches, in order (each one needs one live hit counter with victim == puppet):
  | gate | VA | condition that stops |
  |---|---|---|
  | listener lookup | `0x64A035` / `0x64D8D8` `jz` | `0x5220E0(puppet)` = 0: no component of class `off_279BF68`, so the event is dropped (f7 hooked the *entry* of 0x64A080 and did not record the return) |
  | FDE phase | `0x186B380` `jnz` | FDE+0x54 != 1, so the hit path is not taken |
  | fight service | `0x18644D0` lines 145-155 | `+0x74→+0x24` null, `vt[72](96)` null, or puppet+0x5C null |
  | skip flag | `0x1998D40` `if (!a5)` | no DamageEvent |
  | CSDamage | `0x173E1D0` / `0x173E1DA` / `0x173E1F8` | owner entity null, puppet+0x5C null, not alive |
  | player branch | `0x173E99E` → `0x1482400` / `0x1482210` return 3 | `*(sub_5220D0(puppet)+0x1C)` has no CSrvPlayerHealth handlers; newLife stays the current value |
  | set request | `0x18BA21F` / no handler for `dword_4E20518` | the puppet's AI handle `*([this+4]+0x1C)` is null or has no health binding (f7: puppet health object "not located") |
- The repeated post of one FDE object is not evidence of "never consumed". `sub_10EFC30` allocates FDEs (0x170) into a
  shared list `dword_2AC1FBC` (the same list `sub_1029240` uses for DamageEvents), and why one object was posted 102
  times is **not established**.

## Q3. Applying a damage amount to the local real player

Signatures (all verified as stated):
- `GetEntityAI` `0x6F5F70`: `cdecl(Entity*)` → `*(*(e+0xE8)+0x1C)` (decompile).
- `RequestSetLife` `0x18BA200`: `thiscall(void** pAI, int newLife)`, `retn 4`; returns 3 if `*pAI == 0`, otherwise the
  last handler status. Engine idiom, verbatim: `0x1C11770` does `v9[0] = sub_6F5F70(ent); sub_18BA200(v9, -1)` (kill);
  `0x1B78190` does `a2 = sub_6F5F70(ent); sub_18BA200(&a2, 1)`.
- `GetLife` query `0x18B9EA0`: `thiscall(void** pAI, int* out)`; returns 3 if there is no service.
- `0x1148A60` (lethal) / `0x1172F70` (damage) are reached through `0x1482400(&sc, dmg, bool*)` and
  `0x1482210(&sc, dmg, int* newLife, bool contactNot15)` with `sc = *(sub_5220D0(e)+0x1C)`. `sub_5220D0`:
  `push ecx; add ecx,0E3h; call sub_521010` (thiscall, ecx = entity).
- `CSrvPlayerHealth::SetLife` `0x115ACC0`: `thiscall(CSrvPlayerHealth*, int v)`, `retn 4`. It stores old life at +0xD0.
  If `+0xD4 bit3` is set (`test cl,8`), life is set to **max** (invulnerable). Otherwise life = `min(max, max(v,0))`
  is written as int16 at +0x5C (0x115AD07). If `v < 0`: `+0xD4 |= 1` and `sub_A014A0(this, dword_4DE7E7C, 0)`. It
  maintains +0xD4 bits 1/2 against the threshold at +0x78. If life dropped: `sub_A014A0(this, dword_4DE7E4C, 0)` and
  `sub_1135B40(54,0)`; at low life `sub_1135B40(39,0)`. It always ends with `sub_1150FE0` →
  `sub_FA5A20(old, life, max, 0, 0, +0x64)` (an event post; HUD meaning **UNVERIFIED**). Note the int16 stores the
  clamped value (`max(v,0)`): a dead player reads life 0 with dead bit +0xD4 bit0.
- Post a DamageEvent: `sub_1029240()` (cdecl, returns a new DamageEvent) → `sub_1B86E40(ev, contact, dmg, attackerEnt,
  b0, b1, 35, 0, 0)` (thiscall, 8 stack args per decompile; **return size not read**) → `EnterCS(*(ent+0xD4))`;
  `sub_64D8C0(ev, ent)` (thiscall, `retn 4`); leave the CS. This is exactly what 0x1998D40 does at 0x1999040-0x19990A6.

**Recommended owner-side apply (A, most native): post a DamageEvent to the local player**, mirroring 0x1998D40. Then
the full CSDamage path runs on the owner: gates, lethal decision (7a), armour and damage (7b), the kill branch
(`sub_1732CE0` 0x173E452, `sub_1135B90` 729-733, `sub_17105D0`, `sub_18BA410(&ai,1)`), and
`sub_18BA200` → SetLife. Pass a null attacker unless the guest has a linked copy of the host guard.
0x173E190 handles a null attacker (`mov [ebp+var_8],0` at 0x173E23B), and 0x173E7C0 then uses the raw ev+0xC
(the `sub_199A160` recompute needs `v11 != 0`). `sub_A0BDE0(…,0,0)` (the handle made from a null attacker inside
`sub_1B86E40`) was **not read**.

**Fallback (B, fewest parts):** `ai = GetEntityAI(p)`, `sc = *(sub_5220D0(p)+0x1C)`, `cur` via `0x18B9EA0`;
`0x1482400(&sc,dmg,&lethal)`; `nl = lethal ? -1 : cur`, and if not lethal `0x1482210(&sc,dmg,&nl,1)`; then
`0x18BA200(&ai, nl)`. This skips the CSDamage kill-branch steps listed above.

Threads: both post paths take locks (`sub_D587F0` on `dword_2AC1FA0`, on the entity CS, and on `unk_2AC1FCC` in the
allocator), so a post from another thread is guarded. `0x18BA200` runs the handlers **synchronously** on the caller's
thread. The thread the engine dispatches `dword_2ABEAC4` from was **not traced**. Call both A and B from the game
thread (the mod's player-frame hook, as `pilot lifehit` does for NPCs).

## Q4. Player death

- The lethal decision is made before the write. `0x1148A60`: `life < damage` → 0x173E7C0 sets `*newLife = -1`,
  `*killed = 1`. `damage == life` gives 0 and the player stays alive (0x1172F70 clamps at 0).
- Kill side in 0x173E190 (`var_1 != 0`, 0x173E44A..0x173E764): `sub_1732CE0(contact, attacker, victim)`. For kind 1:
  `sub_1135B90(victim, 729…733)` by attacker type (switch at 0x173E5BB). Then `sub_17105D0(victim)`,
  `sub_18BA410(&ai,1)` (0x173E604), and finally `sub_18BA200(&ai, -1)` (0x173E76B).
- In SetLife `0x115ACC0`: `v < 0` → `or cl,1; mov [esi+0D4h],cl` and `sub_A014A0(this, dword_4DE7E7C, 0)`
  (0x115AD0F-0x115AD22). `dword_4DE7E7C = sub_9F17B0(&unk_331634B)`. Its statechart transition is registered in
  `sub_114FAB0` (`sub_9F1610(0, v8, dword_4DE7E7C, dword_4DE7E7C, v28)`). `sub_11488F0` raises the same event (or
  `dword_4DE7E80`) on request. What the dead state does next (desync screen, respawn) was **not traced**. The f7 live
  control saw Edward 5 → −1 = dead via this path.
- So the owner's death is native as long as the damage arrives through A (or B with `-1`). Never write +0x5C directly.

## Recommended implementation

1. **Probe first (one run, read-only hit counters, victim == puppet):** entry of `0x186B370` (victim `[[ecx+74h]+8]`,
   log FDE+0x54), entry of `0x1998D40` (log `[ebx+0Ch]` damage, `[ebx+10h]` skip), entry of `0x173E190`, and
   `0x64A080`'s return value. The first counter that stays at 0 is the Q2 answer. Run the same counters on Edward as the
   positive control.
2. **Puppet side (machine H), if the probe shows 0x1998D40 is reached:** detour `0x1998D40` (prologue
   `53 8B DC 83 EC 08 83 E4 F0 83 C4 04` = 12 bytes, no relative branch; check the steal). When `sub_702660(ecx)` ==
   puppet: send the request and `retn 0Ch` without calling the original. If it is **not** reached: intercept the FDE post
   (`0x64A020`/`0x64D8C0`, vt `0x2700720`, receiver puppet, FDE+0x54 == 1). That carries only the pre-resolution FDE+0xC
   (no block/parry, no `sub_199A160`) and needs de-duplication (the same object is re-posted).
3. **Owner side (machine G):** apply A (post a DamageEvent to the local player) on the game thread. Fallback: B.
4. **Payload** (`IC_*`, owner = the puppet's player): `u32 seq`, `u32 attackerObjId` (host guard id, 0 if none),
   `u16 damage` (final, step 4 a4), `u8 contact` (FDE+0x20), `u8 subtype` (FDE+0x10), `u8 flags` (FDE+0x1C bits 0,1,4,5).
   The owner decides lethal itself (7a). The puppet then mirrors the owner's health and death on the existing lanes.

## Risks
- **No flinch.** The hurt reaction is **UNVERIFIED** to come from CSHumanFightInteractions (`0x18644D0`, e.g.
  `sub_185FAC0`), before step 4. A and B give health, death and the HUD event, not necessarily the hit animation.
  Getting it natively would mean posting an FDE (0x170, copy ctor `0x1C213F0`) to the player; the layout was not RE'd.
- **Invulnerable states are honoured:** `+0x62 bit0` skips 0x1172F70, and `+0xD4 bit3` makes SetLife restore max.
- **Double damage on G:** G's local copies of host guards must not also damage G's player through the native path, or
  each hit lands twice.
- `sub_1B86E40` maps attack type 35 from DE+0x10, but 0x1998D40 sets +0x10 **after** the Init call. Mirror the same
  order so the owner sees the same mapping (the decompile shows the order; the effect was not checked).

## Negatives / gaps
- "Only CSNPCFightInteractions-family listens for FDE." METHOD: 4-byte image scan for 0x13F3BE59 (2 hits: the immediate
  in getter 0x197C7A0, descriptor 0x2A23C34) and xrefs to 0x197C7A0 (2: listener vtables 0x270B1DC, 0x2727FCC base).
  COVERED: code immediates and data dwords, AC4BFSP.exe. NOT COVERED: class-info pointer compares (xrefs to
  0x2A20304: getter, clone 0x18AF810, registry 0x1A66EA0 only), listeners keyed by the base DamageEvent id receiving
  subclasses, AC4BFMP.exe. NAMES: from the image's own class strings. → **UNPROVEN** as a negative.
- That the puppet's kind (entity+0x9C &7) is 1 is **not checked**; it decides the player branch at 0x173E99E.
- `this[1]` of the CSDamage delegate (`+8` entity, `+0x1C` handle, `+0x24`) is **UNVERIFIED** to be the AIComponent.
  It is also unproven that `*(sub_5220D0(e)+0x1C)` and `GetEntityAI(e)` are the same object. B uses each one where the
  engine uses it.
- The meaning of FDE+0x54 (0 → `sub_185DA80`, 1 → hit) and of `sub_18B9FE0`'s flag (`var_1` / `v36`) was not established.
