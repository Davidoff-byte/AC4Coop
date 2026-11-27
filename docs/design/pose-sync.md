# Pose sync by bone name (F3, ledger row M2.b)

Status: **design, static only.** Nothing here has been built or run. Every engine fact below carries an
IDA receipt from `AC4BFSP.exe` (IDA input md5 `2058342866688f780c8b34526a65bc35`, imagebase `0x400000`,
the same build as the repo pin sha256 `bc11ad79…ab5150`). **`AC4BFMP.exe` was not opened** (RULES-RE
rule 4): every claim is SP-only, and none of them is an absence claim about the engine.
Method: headless `idat` batch (`disasm`, `decompile`, `xrefs` unpaged, `bytes`) on a private copy of
`ref/ida/AC4BFSP.exe.i64`, opened with `DBFL_KILL` (nothing saved). ABI claims quote the instruction, not
the decompiler.

Goal: the puppet's skeleton shows the peer's pose, matched **by bone name** (`crc32(HumanIK name)`
resolved per rig through `sub_44DB80`), never by bone index or by record position.

Prior art re-read first (rule 11): `ops/evidence/ac4-bone-name-map.md`, `spawned-body-animation.md`,
`skeleton-attach-model.md`, `pose-capture-prior-art.md`, `glide-live-2026-11-10.md`
(`known.py pose`, `known.py 44DB80`). Section 2 lists where this pass corrects them.

---

## 1. Verified engine facts

`comp` = the entity's `SkeletonComponent` (vtable `0x01E41E58`; found by the existing component walk,
`phase1.cpp` `FindComponentByVtable`). `pose` = one of its 0xC0-byte pose objects.

### 1.1 Name map: hash to bone index

| fact | receipt |
|---|---|
| `sub_44DB80` is `__thiscall`, ECX = `comp`, one stack arg (`crc32`), `retn 4`, result in AL | `0x44DB84 mov ecx,[ecx+128h]`; `0x44DBC8 retn 4` |
| map descriptor = `comp+0x128` (null: every lookup misses) | `0x44DB84`, `0x44DB8A test ecx,ecx; jz` |
| entries: base `desc+0x28`, u16 count `desc+0x2E`, 8 bytes each | `0x44DB8E mov eax,[ecx+28h]`, `0x44DB91 movzx ecx,word ptr [ecx+2Eh]`, `0x44DBA3 lea esi,[eax+ecx*8]` |
| sorted ascending by hash, lower_bound | `sub_1C67DF0`: `0x1C67DFC sar ecx,3`, `0x1C67E17 cmp [esi+eax*8],edi; jnb` |
| hit returns the **u8** at `entry+4` | `0x44DBBC cmp [eax],edi`, `0x44DBC0 mov al,[eax+4]` |
| miss returns `byte_1E70038` = `0xFF` | `0x44DBCB mov al, ds:byte_1E70038`; bytes `0x1E70038`: `FF` |
| 54 code callers, so the engine itself resolves bones this way | `xrefs_to 0x44DB80` = 54, unpaged |
| identity is zlib CRC-32 of the name: `Head 0x07C159A2`, `LeftHand 0xB675F36C`, `RightHand 0x75F94D30`, `Hips 0xDED10611`, `Reference 0x2C52CBB0`, `Spine 0x530EC1CB` | recomputed with `zlib.crc32` this pass; `Spine` also appears as an immediate in `sub_44FB70` (`1393476043` = `0x530EC1CB`) |

### 1.2 The pose objects of a SkeletonComponent

Built by `sub_44FEB0` (the rig build, reached from M3 `sub_45A830`):

| field | what | receipt |
|---|---|---|
| `comp+0x130` | a pose filled from the model's bones by `sub_44DBF0(desc, comp)` (rest pose: interpretation) | `0x44FECC mov ecx,[esi+128h]` … `0x44FED9 mov [esi+130h],eax` |
| `comp+0x134` | **LOCAL** pose (parent-relative), allocated once | `0x44FF25 call sub_76DF20` → `0x44FF34 mov [esi+134h],eax` |
| `comp+0x13C`, `+0x140`, `+0x144` | the three **WORLD** buffers | `0x44FFEC mov [esi+13Ch],edx` (= first WORLD), `0x44FFF2 mov [esi+140h],edi`, `0x4500A8 mov [esi+144h],edi` |
| `comp+0x138` | the **current** WORLD buffer, one of the three; it **rotates every tick** | `sub_42B970` (below) |
| every pose: `pose+0xA0` = `comp+0x128` (the same descriptor as the name map) | `0x44FF19 mov ecx,[esi+128h]; push ecx` → `sub_76DF20` forwards arg 0 (`0x76DF59 mov edx,[ebp+arg_0]; push edx`) → `sub_769200` `0x76924C mov [esi+0A0h],ecx` |
| `pose+0x90` = mode: **1 = packed i16, 0 = float**, chosen per rig by `desc+0x22 != 0` | `0x44FEED cmp [eax+22h],cl; setnz cl; mov edi,ecx` → pushed as `arg_8` → `0x76927D mov [esi+90h],edi` |
| `pose+0xA4` = record buffer, size `u16 [desc + 0x0C + 2*mode]` (or `+0x10` when `pose+0x8C` bit 29 is set) | `0x769255 test eax,20000000h` / `0x76925C movzx eax,word ptr [ecx+edi*2+0Ch]` / `0x769263 …+10h]` → `sub_623380` (allocates, tag `"::Anim::SkeletonStateData"`) → `0x769271 mov [esi+0A4h],eax` |
| LOCAL has `pose+0x8C` bit 27 (`0x08000000`) **set**; WORLD buffers have it **cleared** | set: `sub_769200 0x76923D or eax,8000000h`, and `sub_76FCA0` `|= 0x8000000` on LOCAL (`0x44FF47`). Cleared on `+0x138` / `+0x140` / `+0x144`: `0x44FF8E`, `0x44FFD6`, `0x450098 and dword ptr [..+8Ch],0F7FFFFFFh`; compose clears bits `0x18000000` on its destination only (`0x76917B`) |

### 1.3 Record addressing: through the descriptor, never by `index * stride`

`sub_758BE0(pose, idx)` is the engine's own "address of bone `idx`'s record" (`__thiscall`, `retn 4`):

```
0x758BE3 mov eax,[ecx+0A0h]          desc
0x758BE9 mov edx,[eax+38h]           u16 table A
0x758BF0 mov dx,[edx+esi*2]          a = A[idx]
0x758BF4 mov esi,[eax+58h]           u16 table B
0x758BFA add dx,2                    (u16)(a + 2)
0x758C01 movzx edx,word ptr [esi+edx*2]   node = B[a+2]
0x758C05 mov esi,[ecx+90h]           mode
0x758C0B lea edx,[edx+edx*2]         3*node
0x758C0E lea edx,[esi+edx*2]         mode + 6*node
0x758C11 movzx eax,word ptr [eax+edx*2+4]   off = u16 at desc+0x04-table + 12*node + 4 + 2*mode
0x758C16 add eax,[ecx+0A4h]          record = buffer + off
```

So the record is reached as `buffer + u16[ recs + 12*B[A[idx]+2] + 4 + 2*mode ]`, where `recs = *(desc+0x04)`
holds 12-byte node records whose u16 at `+0x04` is the float-mode offset and at `+0x06` the packed-mode
offset. Two u16 tables (`desc+0x38`, `desc+0x58`) sit between the bone index and the node record.
`idx` is in the same index space as the name map's u8 (`sub_450490` passes `sub_44DB80`'s result
straight to `sub_758C30`, `0x4504AA movzx edx,al; push edx`).

The engine's per-bone accessors all use this same chain (identical instruction sequence in each):

| accessor | does | packed (mode 1) | float (mode 0) | receipt |
|---|---|---|---|---|
| `sub_758C30(pose, idx, float4* q)` | read rotation | 4 x i16 at `rec+8`, x `1/32767` (`dword_1E41520` = `0x38000100`), renormalised | float4 at `rec+0x10` | `0x758C8B movzx ecx,word ptr [eax+edx*4+6]`, `0x758C96 movsx eax,word ptr [ecx+edx+8]`; `0x758D51 …[eax+edx*2+4]`, `0x758D5F movaps xmm0,[ecx+edx+10h]`; `retn 8` |
| `sub_75A2E0(pose, idx, float4* t)` | read translation | 3 x i16 at `rec+0`, x `xmmword_1E69BE0` = 1/2048 | float4 at `rec+0` | `0x75A330`, `0x75A344..0x75A364 mulps xmm3, ds:xmmword_1E69BE0`; `0x75A39C movaps xmm0,[edx]`; `retn 8` |
| `sub_759060(pose, idx, const float4* q)` | write rotation | `(int)(q * 32767.0)` (`dword_1E4DED4` = `46FFFE00` = 32767.0) to `rec+8..+0xE` | `movaps [rec+0x10]` | `0x75918F …+6]`, `0x75919E lea eax,[edx+eax+8]`, `0x7591BA..0x759204`; `0x75923D movaps [edx+eax+10h],xmm2`; `retn 8` |
| `sub_759250(pose, idx, const float4* t)` | write translation | `(int)(t * 2048.0)` (`xmmword_1E69BD0` = 2048.0) to `rec+0..+4`, `rec+6 = 0` | `movaps [rec+0]` | `0x7592D6 movaps xmm1, ds:xmmword_1E69BD0`, `0x75931C..0x75934C`; `0x759385 movaps [ecx],xmm0`; `retn 8` |

All four apply the pose's own root transform (`pose+0x00/+0x10`, quaternion conjugate via mask
`xmmword_1E6C430` = `FFFFFFFF x3, 0`) **only when `pose+0x8C` bit 27 is clear**
(`0x758D67..0x758D73`, `0x75A3A2..0x75A3AD`, `0x75908A..0x75909C`, `0x75927D..0x75928E`). On LOCAL the bit
is set (1.2), so on LOCAL they read and write the raw parent-relative record. The engine itself writes
LOCAL through `sub_759060` between the animation ticks and the compose: `sub_44FB70` (IK-style, three
hard-coded name hashes resolved inline) does `0x44FCDA mov esi,[esi+134h]` and then
`0x44FE36 mov ecx,esi; 0x44FE3F call sub_759060` (no `mov`/`lea` into ESI between those two addresses).

Constants (`bytes 0x1E69BA0 +96`): `1E69BA0` = (0,0,0,1) identity quaternion; `1E69BC0` = 0.5 x4 and
`1E69BF0` = 3.0 x4 (the rsqrt refinement); `1E69BD0` = 2048.0 x4; `1E69BE0` = 1/2048 x3, 0.

### 1.4 The per-tick order (one SkeletonComponent)

`SkeletonComponent` vtable slot 44 (`0x1E41F08` → `0x453740`; `xrefs_to 0x453740` = 1, that data cell):

```
sub_453740(comp)
  0x453743 cmp [esi+134h],0; jz  -> no LOCAL, nothing at all
  0x45374D lea ecx,[esi+104h]; call sub_D587F0      take the component lock (+0x104)
  0x45375C call sub_431E90(0)
     0x431ED8 call sub_42B970                      ROTATE the WORLD triple buffer, once per frame id
        (0x42B990 cmp comp+0x2C4 with *(dword_2ABEC00+4); 0x42BA92 stores the new id):
        +0x138 := next of {+0x13C, +0x140, +0x144} (0x42B9CB / 0x42BA01 / 0x42BA2B);
        flag 0x40000000 set on the ones not current;
        if owner+0xB0 != 0: r = sub_423260(owner) (a per-frame 0x80-byte record from a pool);
        0x42BA6E or [r+74h],4 ; 0x42BA72 mov [r+64h], new +0x138          (publish)
     0x431F07 movaps [esi+0D0h]..                  cache the owner's root transform
  0x453763 call sub_453330(comp)
     0x453345/0x453351 sub_75A260 root push into LOCAL and current WORLD
     0x453392 sub_6934B0 (+0x1FC), 0x4533C2 sub_5757C0 (+0x248), 0x453401 sub_583730   anim graph -> LOCAL
     0x45340D sub_42B0E0 (level >= 3), 0x453435 sub_44FB70 (IK, writes LOCAL via sub_759060)
     0x453465 sub_67E9A0, 0x453475 sub_42B130, 0x45347C sub_431620
     0x453488 mov eax,[esi+138h]   ; dst = current WORLD
     0x45348E mov ecx,[esi+134h]   ; this = LOCAL
     0x45349A push edx (level = (comp+0x88 >> 6) & 7); push eax
     0x45349C call sub_769150      ; COMPOSE LOCAL -> WORLD.  bytes E8 AF 5C 31 00 (-> 0x769150)
  0x45376A call sub_425E00(comp)
  0x453775 release the lock
```

`sub_769150(this=src, dst, level)` (`retn 8`): `0x769153 mov eax,[ecx+0A0h]`, `0x769159 movzx eax,word ptr
[eax+3Eh]` (bone count `desc+0x3E`), `cmovz esi,ecx` (null dst = in place), `call sub_7609D0(0, count, dst,
src, level)`. `sub_7609D0` dispatches on **dst** `+0x90`: 1 → `sub_759A80` (packed), 0 → `sub_759700`
(float), other → an inline float path. It walks the 7-byte entries at `desc+0x48` in compose order and
skips a bone whose `entry[4] >> 5` exceeds `level`. `sub_769150` has 3 callers (`xrefs_to`, unpaged):
`0x45349C` (above), `sub_43AE80` (`0x43299F`, a locked recompose with level 5 that runs only when
`(comp+0x88 & 0x38) == 0`), `sub_4134A0` (`0x4134E1`, in place: `sub_769150(a3, nullptr, 5)`).
`xrefs_to 0x453330` = 1 (`0x453763`), so the call at `0x45349C` runs exactly once per slot-44 tick.

### 1.5 What follows for the design

- **Apply into LOCAL, right before the compose at `0x45349C`.** LOCAL at that instant holds this tick's
  animation + IK result for the puppet; our write replaces it for matched bones, and the engine's own
  compose produces the WORLD buffer that `sub_42B970` published for this tick. The engine does the same
  kind of write at the same stage (`sub_44FB70`).
- **Capture from LOCAL at the same instant on the sender.** LOCAL is parent-relative, so it does not depend
  on where either body stands; the receiver's compose puts it on the puppet's own root.
- **Never cache `comp+0x138`.** It rotates every tick (`sub_42B970`).
- **Never compute a record address.** Use `sub_758C30` / `sub_75A2E0` / `sub_759060` / `sub_759250`
  (they apply the mode and the descriptor chain themselves). The compose loops do use `index*0x20` /
  `index*0x10` (`sub_759700`: `v9 = 32*a2 + base`; `sub_759A80`: `v10 = 16*a2 + base`), but in the
  **compose-order** index space of the 7-byte entries, which is not shown to equal the name-map index
  space. Section 7 logs both, as a runtime check, not an assumption.
- **Never cache a bone index.** The receiver re-resolves every hash with `sub_44DB80` on every apply
  (at most 255 binary searches over at most 255 entries per tick).

---

## 2. Corrections to earlier notes

| note | says | binary says |
|---|---|---|
| `ac4-bone-name-map.md` §7 step 4 / §6 caveat | read at `poseBase + nodeRecord[idx].offset` | the node record is `B[A[idx]+2]`, two u16 tables away (`desc+0x38`, `desc+0x58`); `sub_758BE0`, 1.3 |
| `ac4-bone-name-map.md` §7 step 1 | the rig object at `SkeletonComponent+0x134` carries `+0x90/+0xA0/+0xA4/+0x128` | `+0x128` is on the **component**; `+0x90/+0xA0/+0xA4` are on each **pose object**; the name-hash wrappers read the **current WORLD** (`0x4504AE`, `0x45046E mov ecx,[esi+138h]`), not LOCAL |
| `spawned-body-animation.md` §3 | `sub_769150(*(this+0x138), level)`, dst = `+0x138` | `this = +0x134` (`0x45348E mov ecx,[esi+134h]`), `arg0 = +0x138`; same direction, the decompiler dropped ECX |
| `spawned-body-animation.md` / ANIM-PIPELINE | `+0x13C/+0x140/+0x144` = pool | the WORLD **triple buffer**; `+0x138` is the current one and is re-pointed every tick (`sub_42B970`) |
| `spawned-body-animation.md` | float path indexes the 7-byte array; mode 1 vs other | three compose paths: 0 → `sub_759700`, 1 → `sub_759A80`, other → inline (`sub_7609D0`); a SkeletonComponent's poses are only ever mode 0 or 1 (`desc+0x22`, 1.2) |

---

## 3. The wire block

### 3.1 Why not inside `PlayerState`

`PlayerState` is frozen at 576 B (`netstate.h:138` static_assert) and the receive gate requires
`n == sizeof(PlayerState)` (`net.cpp:1441`). Even ignoring that, the room left under the 1400 B cap
(`net.cpp:192` `ENTITY_CHUNK_MAX`) is 824 B: 58 bones with rotation + translation, or 103 rotation-only.
The player rig samples at 233 bones (`pose-capture-prior-art.md`, from file sizes). So pose gets its own
datagram type, like the intent lane ('AC4I') on `feat/intent-lane`.

### 3.2 Layout (all little-endian, `#pragma pack(1)`, no padding)

```
NET_POSE_MAGIC = 0x50344341   'AC4P'

struct PoseWireHeader {            // 32 B, static_assert
    u32 magic;          // NET_POSE_MAGIC
    u32 protoVer;       // NET_PROTO_VER
    u32 seq;            // sender pose-frame counter; all chunks of one frame share it
    u32 hostMs;         // sender capture time (same clock as PlayerState::hostTimeMs)
    u32 schemaId;       // crc32 of the schema's hash array bytes (u32 x nameCount, as sent)
    u16 nameCount;      // N: hashes in the schema (1..255; the index is a u8, 0xFF = miss)
    u16 first;          // schema index of this chunk's first record
    u8  count;          // records in this chunk
    u8  kind;           // 1 = SCHEMA, 2 = FRAME
    u8  chunkIndex;     // 0-based
    u8  chunkTotal;     // chunks in this frame (schema: always 1)
    u8  flags;          // bit 0 PF_TRANS: frame records carry translation. Other bits reserved = 0, rejected
    u8  mode;           // sender's LOCAL pose+0x90 (0 float / 1 packed), diagnostic only
    u16 reserved;       // 0
};

SCHEMA payload: count x u32 crc32(name)       // the sender's own map, desc+0x28 order (ascending)
FRAME  payload: count x record, schema order starting at `first`
   record (PF_TRANS)   = i16 q[4] (x,y,z,w; value * 32767) + i16 t[3] (value * 2048)   14 B
   record (!PF_TRANS)  = i16 q[4]                                                      8 B
```

The quantisation is the engine's own packed format (1.3: `x 32767`, `x 2048`), so a packed-mode rig
(mode 1) round-trips bit-exactly and a float-mode rig loses at most `1/65534` per quaternion component and
`1/4096` per translation component. A float translation outside `[-16, +16)` cannot be represented: the
sender clamps it and counts it (`clampT=` in the log, 7.1). Whether LOCAL translations reach that range is
**UNVERIFIED**; a non-zero `clampT` is the signal.

Receive checks (each rejected datagram is counted, never applied): magic, `protoVer == NET_PROTO_VER`,
`n == 32 + count * recSize` exactly, `kind` in {1,2}, `nameCount` in 1..255, `first + count <= nameCount`,
`chunkIndex < chunkTotal`, reserved bits zero; a FRAME whose `schemaId` differs from the last SCHEMA
received is dropped (`poseNoSchema` counter).

### 3.3 Size budget

Per datagram: header 32 B + payload, cap 1400 B (same as the entity chunks; plus 16 B when relayed,
`net.cpp:1925`, under `Relay::kMaxDatagram` 1500).

| datagram | max records | bytes |
|---|---|---|
| SCHEMA | 255 (342 would fit) | `32 + 4N`: N=233 → **964**, N=255 → **1052** (always 1 chunk) |
| FRAME, PF_TRANS (14 B) | 97 (`32 + 97*14 = 1390`) | N=233 → 3 chunks (97+97+39) = **3358 B**; N=255 → 3 chunks = **3666 B** |
| FRAME, rotation only (8 B) | 171 (`32 + 171*8 = 1400`) | N=233 → 2 chunks (171+62) = **1928 B**; N=255 → 2 chunks = **2104 B** |

Rate (`[Coop] PoseHz`, default 10): N=233 with translation 33.6 kB/s at 10 Hz, 67.2 kB/s at 20 Hz;
rotation-only 19.3 / 38.6 kB/s. SCHEMA is resent on change and once per second (+0.96 kB/s). For scale:
the existing player lane measured ~33 kB/s (`docs/STATUS.md`). N is the sender's map count
`desc+0x2E`; its live value is **UNVERIFIED** (233 is the sampled bone count `desc+0x3E`, not the map
count), so the first run logs both (7.1).

Chunks are applied independently (each carries `first`/`count`); a lost chunk leaves those bones on
the previous frame's values for one period. No reassembly buffer, no reliability: the newest `seq` per
chunk index wins, an older one is dropped (`poseStale` counter).

---

## 4. Capture site (sender)

1. **Where:** a call-site redirect at `0x45349C` (`E8 AF 5C 31 00` → our thunk; verify those 5 bytes
   before patching and refuse otherwise). At that instruction, before the call pushes `0x4534A1`, `ESI = comp` (set `0x453331 mov esi,ecx`,
   never written again in `sub_453330`; ESI is callee-saved), `ECX = [comp+0x134]` (LOCAL), `[esp] =
   [comp+0x138]`, `[esp+4] = level`. The thunk saves registers, calls the C handler with `(ESI, ECX)`,
   restores them and **jumps** to `0x769150` (the original return address `0x4534A1` stays on the stack;
   `sub_769150` pops its 8 bytes of arguments itself, `retn 8`).
2. **Filter:** `comp == g_poseCaptureComp` (the local player's SkeletonComponent, published by the main
   thread) **and** `*(comp+0x8) == player entity` (owner, read from the live component) **and**
   `ECX == *(comp+0x134)`.
3. **What:** for every entry `i` of the player's own map (`desc = *(comp+0x128)`, entries at
   `*(desc+0x28)`, count `*(u16*)(desc+0x2E)`, read fresh each tick): `hash = entry.u32`,
   `idx = entry.u8` (this tick's value, not cached); `sub_758C30(LOCAL, idx, &q)`,
   `sub_75A2E0(LOCAL, idx, &t)`; quantise into the capture buffer. Guard: if LOCAL `+0x8C` bit 27 is
   clear, the accessors would apply the root (1.3), so capture stops and counts `rootBit0`.
4. **Hand-off:** the handler `TryEnterCriticalSection`s a private pose lock; if busy it skips this tick
   (`capBusy`). The main thread (OnFrame) copies the newest capture and sends SCHEMA/FRAME at `PoseHz`.
   The handler never logs, allocates or blocks.

## 5. Apply site (receiver, the puppet)

1. **Where:** the same thunk at `0x45349C`, **before** jumping to the compose.
2. **Filter:** `comp == g_poseApplyComp` (the puppet's SkeletonComponent, found with
   `FindComponentByVtable(g_puppet, 0x01E41E58)`) **and** `*(comp+0x8) == g_puppet` **and**
   `ECX == *(comp+0x134)`. The main thread clears `g_poseApplyComp` before any puppet despawn, so a
   recycled address cannot receive a pose for more than the owner check allows.
3. **Which buffer:** **LOCAL (`comp+0x134`)**, through `sub_759060` (rotation) and `sub_759250`
   (translation, only if PF_TRANS). Not WORLD: WORLD is re-pointed every tick and is the compose's output;
   writing it would be overwritten by the compose that follows, or would bypass the hierarchy.
4. **Float vs packed:** decided by the engine, not by us: the setters branch on the puppet LOCAL's own
   `+0x90` (1.3). The wire carries the engine's packed scale, so a mode-1 puppet stores the exact wire
   integers back (`(int)(q*32767)` of `q = i16/32767`).
5. **Per record:** `idx = sub_44DB80(comp, hash)` **every tick**; `0xFF` → skip (counted as unmatched).
   Normalise `q` before `sub_759060` (the float path stores it as given, `0x75923D`).
6. **Root bit guard:** LOCAL `+0x8C` bit 27 must be set (expected from 1.2); if clear, apply stops and
   counts `rootBit0`, because the setters would then un-rotate by the puppet's root.
7. **Under which lock:** the engine already holds `comp+0x104` (`0x45374D`); our handler adds only a
   `TryEnter` on the private pose lock (skip and count `applyBusy` when busy).
8. **What stays the engine's:** the puppet's root transform (`+0xD0`, from its entity, driven by
   `ApplyRemoteState`), bones the peer did not send, the compose, the buffer rotation and the publish.
   The action/blend drive (`DriveActions`) keeps running; for matched bones its LOCAL output is
   overwritten each tick.

Interpolation: v1 applies the newest complete-enough data (per chunk). Buffering on `hostMs` the way
`PuppetHostTime` does for the transform is a follow-up, gated on the v1 oracle.

---

## 6. Ini flags (default off until verified live)

`[Coop] PoseSync=0` (capture + send + apply), `PoseHz=10`, `PoseTrans=1` (PF_TRANS),
`PoseLoopback=0` (solo: apply the local player's own capture to the local puppet, no network).

---

## 7. Oracle (written before building)

### 7.1 Log lines

```
[Pose] schema tx id=%08X n=%u mapCount=%u boneCount(desc+0x3E)=%u mode=%u rootBit=%u      (sender, on change)
[Pose] matched=%u/%u schema=%08X puppetMap=%u mode=%u rootBit=%u unmatched=[%08X …≤8]    (receiver, on schema change + every 5 s)
[Pose] tick applied=%u rx=%u stale=%u noSchema=%u rejects=%u capBusy=%u applyBusy=%u clampT=%u rootBit0=%u
[Pose] readback ok=%u/3 maxDot=%.6f maxDt=%.6f                    (Head, LeftHand, RightHand)
[Pose] layout idx=%u off=%04X flat=%04X same=%u                   (once per rig, diagnostic only)
```

`matched=M/N`: N = the schema's `nameCount`; M = hashes for which `sub_44DB80(puppetComp, hash) != 0xFF`
on the receiver, evaluated at log time. `readback`: inside the handler, right after writing, read the same
three bones back with `sub_758C30` / `sub_75A2E0` on LOCAL; ok when `|dot(q_read, q_wire)| >= 1 - 1e-4`
and `|t_read - t_wire| <= 1/2048 + 1e-6` per component. `layout`: `off = sub_758BE0(LOCAL, idx) - buffer`
vs `flat = idx * (mode ? 0x10 : 0x20)`, logged for the record only (it decides nothing; section 1.5).

### 7.2 PASS / FAIL

| # | check | PASS | level |
|---|---|---|---|
| P1 | same rig (PoseLoopback=1, solo, puppet built from the player's selector) | `matched=N/N`, N = sender `mapCount` > 0 | solo |
| P2 | the probe can fail (positive control for "matched") | peer9 replays the P1 schema (N hashes, captured from the log) plus `0xDEADBEEF` → `matched=N/N+1`, `unmatched=[DEADBEEF]` | solo |
| P3 | the write lands | `readback ok=3/3` on every tick with a frame; with `PoseSync=0` (negative control) no `applied`, no readback line | solo |
| P4 | visible | peer9 schema `{Head}` only, FRAME with a fixed 90-degree rotation about one local axis (all other bones are not sent, so they stay the puppet's own): the head is visibly fixed off-axis while the body animates. "I looked at it" is the weakest oracle and is stated as such; P3 + 7.3 carry the claim | solo |
| P5 | no engine state damaged | 10 min with PoseSync=1: 0 crashes, `rootBit0=0`, `rejects=0`, puppet `+0x138` always one of `{+0x13C, +0x140, +0x144}` | solo, then 2PC |
| P6 | two machines | P1 + P3 on both, identical ASI hash; `matched=N/N` when both wear the default outfit; any other outfit pair reports its own `M/N` and the unmatched hashes | 2PC |

FAIL is any row not met; a `matched` below N on the same rig is a FAIL of the name-map premise, not a
tolerance.

### 7.3 entdiff check

`ops/tools/entdiff.py` snapshots an entity and every component; `--deep 1` adds 0x100 B at each pointer a
component holds, which covers the LOCAL pose object (0xC0 B) at `comp+0x134` but not its record buffer
(two hops). The check:

1. `snap --deep 1` of the **puppet** in states A (`PoseSync=0`) and B (`PoseSync=1`, peer9 sending a
   constant frame), alternated A,B,A,B; `diff --min-states 4`.
2. Must hold: component count and order unchanged; the SkeletonComponent block's `+0x134`, `+0x128`,
   `+0x13C`, `+0x140`, `+0x144` words unchanged (we never reallocate); in the LOCAL block, `+0x90`,
   `+0xA0`, `+0xA4` unchanged. `+0x138` may differ between snapshots (rotation) but must equal one of the
   three buffer words.
3. Record content: an added `--follow 0x134:0xA4:<size>` option (size from `desc+0x0C+2*mode`, 1.2) dumps
   the LOCAL buffer; words that toggle with A/B must lie inside the records of matched bones (their
   `off` from the `[Pose] layout` line), and no word outside `[buffer, buffer+size)` may toggle.

---

## 8. Files a later implementation touches

| file | change |
|---|---|
| `src/netstate.h` | `NET_POSE_MAGIC`, `PoseWireHeader` (+ `static_assert == 32`), `POSE_KIND_*`, `PF_TRANS`, record sizes, `NET_PROTO_VER` bump + history line |
| `src/net.cpp`, `src/net.h` | `SendPoseSchema` / `SendPoseFrame` (chunking at 97 / 171 records), RecvThread dispatch on `'AC4P'` with the checks of 3.2, per-chunk newest-seq store, counters into `[Net] stats` |
| `src/phase1.cpp` | the `0x45349C` call redirect (verify `E8 AF 5C 31 00` first) and naked thunk, capture/apply handlers, `[Pose]` lines, ini flags in the config table (`phase1.cpp:10923`), clear `g_poseApplyComp` on despawn |
| `decomp/units.json` | a unit per direct engine call: `0x44DB80`, `0x758C30`, `0x75A2E0`, `0x759060`, `0x759250`, `0x758BE0`, `0x769150` (jump target); `py tools\spine_gate.py` must pass |
| `tools/verify_signatures.py` | the new patch site `0x45349C` |
| `tools/peer9.py` (repo) and `ops/tools/peer9.py` | SCHEMA/FRAME encoder for P2/P4. Both are stale today: repo copy `NET_PROTO_VER = 9`, ops copy `= 11`, `src` is at 14 |
| `ops/tools/wire_model_test.py` | `PoseWireHeader` layout + magic + proto equality against the Python model |
| `tools/wire_tap.py` (+ `tools/lobby_peer.py`, where it takes its constants) | classify and decode `'AC4P'` |
| `ops/tools/entdiff.py` | `--follow` for 7.3 step 3 |
| `AC4Coop.ini`, `docs/STATUS.md` | flags; status row once verified |

**Protocol number.** `feat/intent-lane` (`58a4835`, not merged) already takes **15** for the 'AC4I'
lane and edits `netstate.h`, `net.cpp`, `net.h`, `tools/wire_tap.py`, `tools/lobby_peer.py`: the same
files. Pose therefore takes **16** and is implemented on top of intent-lane once that merges. If pose
had to land first, it would take 15 and intent-lane would rebase to 16: decide in one place, never two
branches claiming the same number. The bump is not cosmetic: an older build silently ignores an
unknown magic (RecvThread only accepts `playerPkt || entityPkt`), so without it a mixed pair would run
with pose silently off; the lobby REJECT on `protoVer` makes the skew visible instead.

---

## 9. UNVERIFIED (to settle live or in a further static pass)

1. **Which thread runs slot 44.** Only the data xref `0x1E41F08` exists (virtual dispatch). The handler is
   written to be safe on any thread (TryEnter, no logging, no allocation), but the thread is not known.
2. **That the renderer consumes the buffer published by `sub_42B970`** (`sub_423260(owner)+0x64`). The
   write and publish are verified; the consumer was not read. P4 is the check.
3. **The map count `desc+0x2E` on the player and puppet rigs**, and whether it equals the bone count
   `desc+0x3E`. Budget section 3.3 is computed for 233 and the u8 maximum 255.
4. **LOCAL `+0x8C` bit 27 stays set at run time.** Set at creation (1.2); the only clears found are on
   other buffers, but no full writer sweep of `+0x8C` was done (it is a common offset). The `rootBit` guard
   covers it.
5. **LOCAL translations fit in `[-16, +16)` (unit of the engine's 1/2048 scale).** True by construction
   for a mode-1 rig; not known for a mode-0 rig. `clampT` is the signal.
6. **The puppet's mode equals the player's.** Both come from `desc+0x22` of their own rig; same model →
   same descriptor is expected, not measured. The design does not depend on it (each side converts
   through its own accessors).
7. **Name-map index space vs compose-order index space** (1.5): logged by `[Pose] layout`, never used.
8. **Effect of the overwrite on systems that read LOCAL after the compose** (none found on the slot-44 path
   after `0x45349C`; `sub_425E00` only touches flags). Other readers outside that path were not enumerated.
9. **`AC4BFMP.exe`**: not opened; nothing here is claimed for it.
10. Hard-coded IK hashes in `sub_44FB70` other than `Spine` (`0x8F39FA4E`, `0x1630ABF4`) were not named.

No negative result is claimed in this document, so no rule-6 block is needed; every "only" above is a
complete, unpaged `xrefs_to` count, quoted with its number.
