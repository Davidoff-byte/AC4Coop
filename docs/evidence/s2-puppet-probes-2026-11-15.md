# S2 probes on the co-op puppet: fight, combo, exit, and the AI-off question (2026-11-15 22:00-22:20)

Follows `s2-live-probes-2026-11-15.md`, where the fight state held for only one frame on Edward.
- Build: AC4BFSP.exe pinned; ASI 93a3afd8cee36283b76eea211845b6c3997c00d138d263ff3999cf58c9e0d984 (`75141e4`, tree clean);
  version.dll proxy.
- Two copies via `tools/two_copy.ps1 -Interactive`, `[Coop] NativeReplica=0 ActMatchProbe=1`: host A pid 21216, guest B pid
  32404.
- Probes were sent to B with `pilot.py hg --puppet`. They are served on the puppet's own Human update (`Pilot::OnPuppetFrame`
  from `phase1.cpp` OwnDrain, puppet branch) and logged as `[HG-P]` in `phase1_log_32404.txt`.
- The puppet is the host's body: puppet Human 3E2066A0; the guest's own player Human is 3AD83CA0.
- Enum values come from the PS3 DWARF (die offsets in parentheses):
  - WeaponType (0x487624f): Unarmed=0, Medium=1, ..., DualWieldPrimary=7, ...
  - ExitFightType (0x487b48a): Normal=0, Step2=1, Step2_180Turn=2.
  - ComboEndCondition (0x6216f09): Damage=0, Miss=1, MaxHits=2.

## Results (target G = Templar-role NPC 30201610 on B, about 7 m from the puppet)
| # | Call on the puppet | Result | Verdict |
|---|---|---|---|
| 1 | GetFightStage 300, GetHumanGroundStage 689 | 0x27, 1; ctx 4, path 1/2/3/4/5/6 (same as Edward) | sanity |
| 2 | Host walks 2.2 m (world intent) | puppet path stays 1/2/3/4/5/6 | Move covers walking |
| 3 | DoEnterFight 76 alone (negative control) | garbage eax, path unchanged | gated on its Can, as on Edward |
| 4 | CanEnterFight 75 → DoEnterFight 76 | Can 1, Do 1; path 1/2/163/166/167/168 and **still there 2 s and 5 s later**; GetFightStage 39 → 2 | **PASS; the fight holds on the puppet** (on Edward it lasted 1 frame) |
| 5 | PerformCombo 160 (G, 1, Damage, false) | path 167/168 → 173/178 → 173/180 (+19 frames) → 173/181 (+46) → back to 167/168; GetFightStage 4 → 7 → 2; puppet action alias changed from the peer's 50 to 378520822/378520826 (`[Net] PUP` lines) | **PASS: the puppet's own FSM played a combo** |
| 6 | Attack 154 (G, false, DualWieldPrimary=7) | returns 0; path stays 167/168 for about 2 s | **no effect observed** (S2: the request is gated on req+0xB7C==34, not set up or checked) |
| 7 | CanExitFight 112 → DoExitFight 113 (Normal) | Can 1; next frame path 1/2/3/4/5/6, stage 39 | **PASS** |
| 8 | Eye check: EnterFight + PerformCombo (G, 3, MaxHits, false), guest Edward moved 5 m from the puppet | guest-window screenshot 500 ms later shows the puppet in a strike pose toward an NPC | observed ("I looked at it", weakest oracle) |

## The AI-off question (S2 Q2)
- The puppet's behaviour component was disabled once (`[Net] puppet behavior disabled (ctx=4)`, log line 508) and not
  re-enabled while the probes ran (lines 1506-4238).
- With it disabled, three things were observed:
  - our hook inside the puppet's `Human::Update` still fired: every `--puppet` call was served there;
  - the HumanGround FSM kept ticking: the combo advanced through sub-states by itself over about 46 frames;
  - the fight state held.
- This **contradicts the S2 lead** that the behaviour component's actor update (0x54CB90) is what calls `Human::Update`, at
  least while the component is disabled through `SetEnabled(false)`.
- It means the existing behaviour-off switch already acts as an AI-off seam for the ground FSM: decisions are off and the
  native state machine still runs.
- Not shown: that no other AI path writes actions on the puppet. During these probes the action-copy layer still ran (it
  re-wrote blend factors; it writes the action on alias change).

## What this means for the plan
- The S7 targeted-action path works on a replica: enter fight with a target handle, combo, exit. The receiver needs the
  target entity's handle (`*(entity+0xC8)`) on its own side, which is where netIds come in (S3).
- The action-copy layer and the FSM both write the puppet's actions. A decision-driven context must suspend action copy
  while the FSM owns it (`storm-replication.md` 3.3 item 3).
- Attack (154) needs its request gate understood before use.

## Not done
Attack 154 effect; a damage or hit check (does the combo hit G on B, and what does the host see); climb on the puppet (needs
the climb candidate data, `s2-live-probes-2026-11-15.md`); how long a fight lasts with no further calls.
