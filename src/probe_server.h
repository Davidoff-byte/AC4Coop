#pragma once
// [Probe] Enabled=1 (default 0): runtime probes on the live game without a rebuild. x86 port of the AU-RW ACU tap
// (tools/native_tap/tap.cpp, the user's own code): one local named pipe \\.\pipe\ac4probe-<pid>, one ASCII command per
// connection, one JSON line back, then the client sends "ACK" (tools/probe.py).
//
// Commands
//   STATUS                                   hooks, counters, event ring use
//   READ <hexVA> <len>                       SEH-guarded read, 1..65536 bytes, hex
//   POKE <hexVA> <hexbytes>                  SEH-guarded write (live modification - use with care)
//   HOOK <id> <hexVA> <expectedhex> <label> [exit]
//                                            MinHook detour of a function entry (id 0..63). The expected bytes must match
//                                            the live bytes or nothing is patched. Entry events record ecx, edx, eax,
//                                            eflags, the return address and 8 stack arguments. With `exit`, the return
//                                            address is swapped for an exit stub through a per-thread shadow stack, so
//                                            the callee still pops its own arguments (any calling convention) and the
//                                            exit event records eax/edx.
//   UNHOOK <id>
//   SNAP <id> entry|exit <snapId> <len> <start> [+hex|-hex|*]...
//                                            capture `len` (1..256) bytes at an address expression evaluated in the
//                                            hooked thread: start = ecx edx eax esp a0..a7 (stack args at entry;
//                                            at exit the entry ecx and a0..a7 are kept), ops: +k / -k add, * deref.
//   CLEARSNAPS | LOG <id> 0|1 | RESET
//   EVENTS [n]                               newest n (<=256) events as JSON, snapshots in hex
//   WWATCH <hexVA> <len>                     trace writes to [VA, VA+len) by page protection (<=8 ranges)
//   WHITS [n] | WSTOP                        newest write hits (eip, address, tid, 8 stack dwords) / stop tracing
//   UNLOAD                                   disable every hook and tracer; the module stays resident
namespace ProbeServer
{
    void Init();   // DllMain attach, after the build gate: starts the pipe thread when [Probe] Enabled=1
}
