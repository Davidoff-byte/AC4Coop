# Work ledger

A running record of what was done, what is waiting and what comes next, so anyone can pick the work up.
Newest entries first. What is true about the mod itself lives in `docs/STATUS.md`; the rules live in `AGENTS.md`.

Last updated 2026-11-12. `main` at `a6935d2`.

## Waiting on someone

| item | what it needs | who |
|---|---|---|
| stray branch `origin/fix/net-hardening-blendcount` | it reappeared on 2026-11-12 at `cddbc41`, already fully in `main`. Someone pushed an old local copy. Delete the local branch (`git branch -D fix/net-hardening-blendcount`) and then the remote one | whoever pushed it |

## Next: the lobby-code stack

Full design: `docs/design/lobby-code.md`. Decided 2026-11-11: **serverless** (the
code is the host's public IP:port; UPnP opens the port; STUN detects CGNAT), **one host + one guest**, the current
manual `PeerIP` setup stays the default.

| # | slice | state |
|---|---|---|
| 1 | design doc | merged (#4) |
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
- **Check:** CRC-16/CCITT-FALSE of those 6 bytes (polynomial `0x1021`, init `0xFFFF`, no reflection, no final
  xor; `"123456789"` → `0x29B1`). Keep the low 12 bits.
- **Value:** `(48-bit big-endian bytes << 12) | check` = 60 bits.
- **Text:** 12 symbols, 5 bits each, most significant first, from Crockford base32 `0123456789ABCDEFGHJKMNPQRSTVWXYZ`.
  Displayed `XXXX-XXXX-XXXX`.
- **Decode:** accept either case. Ignore `-` and spaces. Map `O`→`0` and `I`/`L`→`1`. The result must be exactly
  12 symbols and the check must match, else return "invalid".
- **API:** `bool LobbyCode::Encode(uint32_t ipv4_hostorder, uint16_t port, char out[15])` and
  `bool LobbyCode::Decode(const char* text, uint32_t* ipv4_hostorder, uint16_t* port)`.
  Python: `encode(ip: str, port: int) -> str`, `decode(text: str) -> tuple[str, int] | None`, plus a CLI
  (`py tools\lobby_code.py encode 1.2.3.4 47801` / `decode XXXX-XXXX-XXXX`).
- **Selftest:** at `Net::Init`, next to `SelfTestWire()`, run every vector below plus the negative cases and
  log one line: `[Lobby] code selftest: PASS n=<count>` or `FAIL <which>`. The Python file runs the same vectors
  with `--selftest`. Both read `tools/testdata/lobby_codes.txt`, or the C++ copy is generated from it, so the
  two implementations cannot drift.

Reference vectors (computed 2026-11-12 with the reference implementation of exactly the rules above):

| address | code |
|---|---|
| `127.0.0.1:47801` | `FW00-00DT-Q7MB` |
| `192.168.1.20:47801` | `R2M0-255T-Q5JA` |
| `203.0.113.7:47801` | `SC07-21XT-Q5F7` |
| `0.0.0.0:0` | `0000-0000-03GG` |
| `255.255.255.255:65535` | `ZZZZ-ZZZZ-ZYEF` |

Must decode to the same address: `sc07-21xt-q5f7` (lower case) and `SC07 21XT Q5F7` (spaces).
Must be rejected: `SC07-21XT-Q5F` (11 symbols), `SC07-21XT-Q5FU` (`U` is not in the alphabet).

Typo protection, measured over 2,000 random addresses with the reference implementation: 0 of 744,000
single-symbol substitutions and 0 of 21,320 adjacent swaps decode. A good selftest also checks that one
substituted symbol is rejected.

**Decided 2026-11-12: 12 characters.** A 10-character code with a 2-bit check was rejected because about 1 typo
in 4 decoded to a wrong address (measured: `SC07-21XT-Q0` → `203.0.113.7:47800`).

## Setting up (one-time per machine)
- Game: `AC4BFSP.exe` 45,056,040 B, sha256 `bc11ad7924e1eca7363ee86aa3332c05386d0a05896c0b7370131daee9ab5150`.
  On the owner's machine: `G:\Games\Assassins Creed IV Black Flag` (the mod is deployed there) and an
  identical copy at `E:\Games\…`.
- Set `AC4_GAME_DIR` and `AC4_KNOWN_EXE` as user environment variables, then check with `py tools\paths.py`.
- Build: `MSBuild AC4Coop.sln /nologo /v:m /p:Configuration=Release /p:Platform=x86 /p:PlatformToolset=v143`.
- Checks before every PR: `py tools\spine_gate.py` (passes 44/44 since #5) and, if a patch site changed,
  `py tools\verify_signatures.py`. There is no CI, by choice.
- Do not let Git rewrite line endings in `decomp/`: receipts hash exact bytes, and `.gitattributes` marks the
  folder `-text`. If `decomp.py check` reports "candidate source drifted" with no content change, check line
  endings first (this is what #5 fixed).

## Done

| date | PR | what |
|---|---|---|
| 2026-11-12 | (this PR) | lobby code fixed at 12 characters with a 12-bit CRC check; spec and vectors updated |
| 2026-11-12 | #6 merged | this ledger |
| 2026-11-12 | #5 merged | 6 `decomp/candidates` files restored to the CRLF bytes their receipts hash. `decomp.py check` 6 failing → 0 of 44; `spine_gate.py` OK |
| 2026-11-12 | #4 merged | lobby-code design, `AGENTS.md` wording (Ubisoft services are out of scope; our peer link is in), no CI by choice |
| 2026-11-11 | #3 merged | `docs/STATUS.md` added; 30 stale plans/journals/handoffs moved to `docs/archive/`; bus runtime files, `.pyc` files and a stale `tools/` copy untracked or archived; scratch scripts removed or kept in `tools/oneoff/` |
| 2026-11-11 | #2 merged | `AGENTS.md`, `CLAUDE.md`, PR template: task branches, one concern per PR, address-cited RE claims, separate build / solo / two-machine verdicts |
| 2026-11-11 | #1 merged | `main` brought up to the live-tests stack (75 commits: udp-bind, net-hardening, skel-attach, BoardableTest). The four branches were deleted |

Nothing in this ledger was run in the game. All of it is docs, repository hygiene or the one byte fix in #5.
The last pinned build is `cddbc41`. It has a clean build and a passing gate, but no two-machine run
(see `docs/STATUS.md` and the README).
