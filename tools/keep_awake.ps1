# Keeps the display and the system awake while it runs, and wakes a sleeping monitor with a 1 px mouse nudge.
# With the monitor asleep, Windows Graphics Capture / Desktop Duplication deliver 0 frames (ffmpeg gfxcapture hangs) and
# the two AC4 copies run at ~30 fps instead of ~60 (2026-11-28), so runs taken asleep are not comparable.
#   Start-Process powershell -ArgumentList '-NoProfile','-File','tools\keep_awake.ps1','-Minutes','30' -WindowStyle Hidden -PassThru
# Stop it by killing the process; the execution state ends with its thread.
param([int]$Minutes = 60)
Add-Type -Namespace KeepAwake -Name Native -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint flags);
[DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, System.UIntPtr extra);
'@
$flags = [uint32]0x80000003   # ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED
[void][KeepAwake.Native]::SetThreadExecutionState($flags)
[KeepAwake.Native]::mouse_event(1, 1, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 50
[KeepAwake.Native]::mouse_event(1, -1, 0, 0, [UIntPtr]::Zero)
$end = (Get-Date).AddMinutes($Minutes)
while ((Get-Date) -lt $end) { Start-Sleep 30; [void][KeepAwake.Native]::SetThreadExecutionState($flags) }
