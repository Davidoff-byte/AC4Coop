# guard-ai UNVERIFIED sweep (2026-11-16)

Directive: resolve statically or mark properly. 77 markers across seven files. Most are "puppet value UNVERIFIED" and
are runtime-only by nature; those are marked as such below and are not defects. Two were read now and one is wrong.

## Resolved / corrected now

| where | marker | outcome |
|---|---|---|
| fightmanager.md:26 | "+0x40 embedded object; vt+48 called every tick (UNVERIFIED: what it is)" | **[DECOMP] clarified, and the doc conflates two objects.** At `sub_1626AB0` 0x01626C2E the call is `(*(vt[12]))(fm+0x40)` - the **FightManager's own** `+0x40` sub-object, updated once per manager tick. A **record's** `+0x40` is a different thing: the target entity (`sub_15DABC0` reads `[rec+40h]` and treats it as an entity). Identity of the sub-object's class is still unread. |
| fightmanager.md:55 | "+0x44 compared with a Team* (meaning UNVERIFIED)" | **[DECOMP] "Team*" is unsupported.** In `sub_1614EE0` the test is `*(v11+68) != v9` where `v9` is a **candidate entity** from the array `sub_15F04D0` filled with entities. So `+0x44` is compared against an entity, not a team. What it holds is still unknown; leading candidate is "the entity this record already refers to", UNPROVEN. |

## Runtime-only by nature (no static resolution possible)

These say "puppet value" and need a body in the world; they stay UNVERIFIED until a live pass:
fightmanager.md:238, :239, :242, :245, :247, :249, :253, :256, :262, :284, :287; decision-tree.md:60, :91;
combat-nav.md:188, :263, :276, :309, :319, :323, :325; statechart.md:205, :207, :210; perception.md:240, :245, :249,
:251, :258, :273, :275.

## Negatives already carrying METHOD / COVERED / NAMES (kept, not re-run)

- combat-nav.md:299 (no other writer found - scan pattern stated), :261 (statechart `+0xA` writer UNPROVEN, pattern
  stated), :202/:204 (slot callers beyond the pattern), perception.md:180 (LOS-flag family), :213 (COVERED/SKIPPED
  stated), :219, :225, statechart.md:174 (function list stated), fightmanager.md:228 (undefined code not swept - band
  stated).

## Still open, statically readable, not yet read

- fightmanager.md:117 `sub_16186F0` (per-entry delayed events) - body unread.
- fightmanager.md:271 FM vtable slots 14/15/17 via `sub_1622450` - bodies unread.
- fightmanager.md:274 `sub_15F77E0` / `sub_15DF210` purpose (format `%d` only).
- decision-tree.md:51 `w0 0x10000000` = "DoesPacingAllowNPCActions" - naming only, never proven.
- perception.md:152 CLConflict ctor null-handles meaning.

## Note on the marker convention itself

BRIEF.md:11-17 already requires METHOD/COVERED/NAMES for negatives. The sweep shows the *positive* side has no equivalent:
"puppet value" markers carry no plan for obtaining the value. They now have one list above, so a single live pass can
close them all instead of each living in a different paragraph.

## Open items read (same batch)

- **fightmanager.md:274 sub_15F77E0 - resolved.** It is a *naming* helper: builds the world/player chain, then
  switch (a3) calls sub_407A00("CelestialNavigation") and similar per-case strings (0x015F783D onward). So the
  "format %d only, purpose UNVERIFIED" note was half right: the strings are names, the %d callers are separate.
- **fightmanager.md:117 sub_16186F0 - partially resolved.** Entry takes a critical section on 	his+2012
  (sub_D587F0) before walking entries, so it is the serialised drain of the per-entry delayed-event list. What the
  events *do* still needs the two callbacks read.
- **fightmanager.md:271 sub_1622450 - NOT a slot dispatcher.** [DECOMP] 8496 bytes, and there is no switch (a2)
  in the body (regex over the decompile: 0 cases). Purpose remains UNVERIFIED; the "slots 14/15/17" wording in the doc
  is an assumption about the caller, not something read.
- **perception.md:152** (CLConflict ctor null-handles) not read this batch.
