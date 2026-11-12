# Work ledger

A running record of what was done, what is waiting and what comes next, so anyone can pick the work up.
Newest entries first. What is true about the mod itself lives in `docs/STATUS.md`; the rules live in `AGENTS.md`.

Last updated 2026-11-12. `main` at `99185c6`.

## Waiting on someone

| item | what it needs | who |
|---|---|---|
| PR #5 `fix/decomp-candidate-eol` | merge; it makes `spine_gate.py` pass (44/44) | anyone with write access |
| PR #4 `docs/lobby-code-design` | review and merge; the lobby design + "no CI by choice" in `AGENTS.md` | owner + codev |
| decision: code length (below) | pick 10 or 12 characters before slice 2 is merged | owner + codev |
| stray branch `origin/fix/net-hardening-blendcount` | it reappeared on 2026-11-12 at `cddbc41`, already fully in `main`. Someone pushed an old local copy. Delete the local branch (`git branch -D fix/net-hardening-blendcount`) and then the remote one | whoever pushed it |

Merge #5 before #4, or in either order: they touch different files.

## Next: the lobby-code stack

Full design: `docs/design/lobby-code.md` (on PR #4 until it merges). Decided 2026-11-11: **serverless** (the
code is the host's public IP:port; UPnP opens the port; STUN detects CGNAT), **one host + one guest**, the current
manual `PeerIP` setup stays the default.

| # | slice | state |
|---|---|---|
| 1 | design doc | PR #4, open |
| 2 | code format: encode/decode, C++ + Python, startup selftest | **next, not started**. Spec below |
| 3 | HELLO/WELCOME handshake, `[Net] Mode=manual\|host\|join`, host learns `g_peer`, proto 11→12, `peer9.py --join` | not started |
| 4 | UPnP port mapping + external IP, removed on exit | not started |
| 5 | STUN check, CGNAT detection and the user-facing messages | not started |
| 6 | code in the window title + clipboard; paste-to-join hotkey | not started |

One PR per slice. Each one builds, passes both checks, and keeps every new behaviour behind an ini flag defaulting
to off/`manual`.

### Slice 2 spec: the code format
Files: `src/lobby_code.h`, `src/lobby_code.cpp` (add both to `AC4Coop.vcxproj` and `.filters`),
`tools/lobby_code.py`, `tools/testdata/lobby_codes.txt`. There is no networking and no ini change.

- **Bytes:** the IPv4 address as 4 bytes in dotted order (`a.b.c.d` → `a` first), then the port as 2 bytes, big-endian.
- **Check:** CRC-8 of those 6 bytes, polynomial `0x07`, init `0x00`, no reflection, no final xor. Keep the low 2 bits.
- **Value:** `(48-bit big-endian bytes << 2) | check` = 50 bits.
- **Text:** 10 symbols, 5 bits each, most significant first, from Crockford base32 `0123456789ABCDEFGHJKMNPQRSTVWXYZ`.
  Displayed `XXXX-XXXX-XX`.
- **Decode:** accept either case. Ignore `-` and spaces. Map `O`→`0` and `I`/`L`→`1`. The result must be exactly
  10 symbols and the check must match, else return "invalid".
- **API:** `bool LobbyCode::Encode(uint32_t ipv4_hostorder, uint16_t port, char out[13])` and
  `bool LobbyCode::Decode(const char* text, uint32_t* ipv4_hostorder, uint16_t* port)`.
  Python: `encode(ip: str, port: int) -> str`, `decode(text: str) -> tuple[str, int] | None`, plus a CLI
  (`py tools\lobby_code.py encode 1.2.3.4 47801` / `decode XXXX-XXXX-XX`).
- **Selftest:** at `Net::Init`, next to `SelfTestWire()`, run every vector below plus the negative cases and
  log one line: `[Lobby] code selftest: PASS n=<count>` or `FAIL <which>`. The Python file runs the same vectors
  with `--selftest`. Both read `tools/testdata/lobby_codes.txt`, or the C++ copy is generated from it, so the
  two implementations cannot drift.

Reference vectors (computed 2026-11-12 with the reference implementation of exactly the rules above):

| address | code |
|---|---|
| `127.0.0.1:47801` | `FW00-00DT-Q7` |
| `192.168.1.20:47801` | `R2M0-255T-Q4` |
| `203.0.113.7:47801` | `SC07-21XT-Q7` |
| `0.0.0.0:0` | `0000-0000-00` |
| `255.255.255.255:65535` | `ZZZZ-ZZZZ-ZW` |

Must decode to the same address: `sc07-21xt-q7` (lower case) and `SC07 21XT Q7` (spaces).
Must be rejected: `SC07-21XT-Q` (9 symbols), `SC07-21XT-QU` (`U` is not in the alphabet).

**Open decision, code length.** With 2 check bits about 1 typo in 4 still decodes, to a wrong address.
Measured: `SC07-21XT-Q0` (last symbol mistyped) decodes to `203.0.113.7:47800`. In practice the join then
times out rather than connecting somewhere else, because the handshake (slice 3) only succeeds against a
running host on the same build. Options:
- keep **10 characters** and accept it, or
- go to **12 characters** (`XXXX-XXXX-XXXX`), with a 12-bit check (CRC-16/CCITT low 12 bits) that catches
  essentially every typo.

Decide before slice 2 merges. If it changes, regenerate the vectors and update `docs/design/lobby-code.md`.

## Setting up (one-time per machine)
- Game: `AC4BFSP.exe` 45,056,040 B, sha256 `bc11ad7924e1eca7363ee86aa3332c05386d0a05896c0b7370131daee9ab5150`.
  On the owner's machine: `G:\Games\Assassins Creed IV Black Flag` (the mod is deployed there) and an
  identical copy at `E:\Games\…`.
- Set `AC4_GAME_DIR` and `AC4_KNOWN_EXE` as user environment variables, then check with `py tools\paths.py`.
- Build: `MSBuild AC4Coop.sln /nologo /v:m /p:Configuration=Release /p:Platform=x86 /p:PlatformToolset=v143`.
- Checks before every PR: `py tools\spine_gate.py` (needs #5 merged to pass) and, if a patch site changed,
  `py tools\verify_signatures.py`. There is no CI, by choice.
- Do not let Git rewrite line endings in `decomp/`: receipts hash exact bytes, and `.gitattributes` marks the
  folder `-text`. If `decomp.py check` reports "candidate source drifted" with no content change, check line
  endings first (this is what #5 fixed).

## Done

| date | PR | what |
|---|---|---|
| 2026-11-11 | #5 (open) | 6 `decomp/candidates` files restored to the CRLF bytes their receipts hash. `decomp.py check` 6 failing → 0 of 44; `spine_gate.py` OK |
| 2026-11-11 | #4 (open) | lobby-code design, `AGENTS.md` wording (Ubisoft services are out of scope; our peer link is in), no CI by choice |
| 2026-11-11 | #3 merged | `docs/STATUS.md` added; 30 stale plans/journals/handoffs moved to `docs/archive/`; bus runtime files, `.pyc` files and a stale `tools/` copy untracked or archived; scratch scripts removed or kept in `tools/oneoff/` |
| 2026-11-11 | #2 merged | `AGENTS.md`, `CLAUDE.md`, PR template: task branches, one concern per PR, address-cited RE claims, separate build / solo / two-machine verdicts |
| 2026-11-11 | #1 merged | `main` brought up to the live-tests stack (75 commits: udp-bind, net-hardening, skel-attach, BoardableTest). The four branches were deleted |

Nothing in this ledger was run in the game. All of it is docs, repository hygiene or the one byte fix in #5.
The last pinned build is `cddbc41`. It has a clean build and a passing gate, but no two-machine run
(see `docs/STATUS.md` and the README).
