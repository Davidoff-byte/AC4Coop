#pragma once
// Boot patches (ported from v1, proven as infrastructure: s0-two-copy-rig, single-instance-guard-2026-11-15).
namespace v2::boot {

void InstallEarly();   // DllMain, before WinMain: unique mutex per process, windowed style
void InstallLate();    // init thread: no pause on focus loss, skip the boot videos, auto-continue the save

}  // namespace v2::boot
