# Place two AC4 game windows: the first pid top-right, the second bottom-left, always on top, without taking focus.
# The windows keep their own size unless -Resize, which sizes each client area to half the primary screen (960x540 on
# 1920x1080). Setting Assassin4.ini DisplayWidth/Height=960x540 does not work: the game rewrote it to 1920x1080 at start
# (2026-11-15), so the game renders at desktop size and the window scales it down.
# They stay normal bordered windows: drag them by the title bar.
# Usage: tools\arrange_windows.ps1 <topRightPid> <bottomLeftPid> [-Resize]
# Standing rule for same-PC two-copy runs (host top-right, guest bottom-left).
param([Parameter(Mandatory)][int]$TopRight, [Parameter(Mandatory)][int]$BottomLeft, [switch]$Resize)
Add-Type @'
using System; using System.Runtime.InteropServices;
public struct AwRect { public int L, T, R, B; }
public static class AwWin {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool AdjustWindowRectEx(ref AwRect r, uint style, bool menu, uint ex);
  [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out AwRect r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out AwRect r);
}
'@
# Physical pixels: per-thread awareness works even after the host process fixed its DPI mode (seen 2026-11-15: a
# process-wide SetProcessDPIAware that did not take gave 1280x720 at 150% scaling and 640x360 windows).
$prevCtx = [AwWin]::SetThreadDpiAwarenessContext([IntPtr](-4))   # DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
if ($prevCtx -eq [IntPtr]::Zero) { [AwWin]::SetProcessDPIAware() | Out-Null }
$sw = [AwWin]::GetSystemMetrics(0); $sh = [AwWin]::GetSystemMetrics(1)
$cw = [int]($sw / 2); $ch = [int]($sh / 2)
foreach ($pair in @(@($TopRight, 'topright'), @($BottomLeft, 'bottomleft'))) {
  $id = $pair[0]; $where = $pair[1]; $h = [IntPtr]::Zero
  for ($i = 0; $i -lt 60 -and $h -eq [IntPtr]::Zero; $i++) {
    $p = Get-Process -Id $id -ErrorAction SilentlyContinue
    if (-not $p) { throw "pid $id is not running" }
    $h = $p.MainWindowHandle; if ($h -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 500 }
  }
  if ($h -eq [IntPtr]::Zero) { throw "pid $id has no window" }
  $r = New-Object AwRect; [AwWin]::GetWindowRect($h, [ref]$r) | Out-Null
  $ow = $r.R - $r.L; $oh = $r.B - $r.T
  if ($Resize) {
    # measured frame (window minus client), so the client area comes out exactly $cw x $ch
    $c = New-Object AwRect; [AwWin]::GetClientRect($h, [ref]$c) | Out-Null
    $ow = $cw + ($ow - $c.R); $oh = $ch + ($oh - $c.B)
  }
  $x = if ($where -eq 'topright') { $sw - $ow } else { 0 }
  $y = if ($where -eq 'topright') { 0 } else { $sh - $oh }
  # HWND_TOPMOST (-1), SWP_NOACTIVATE (0x10); SWP_NOSIZE (0x1) unless -Resize
  $flags = if ($Resize) { 0x0010 } else { 0x0011 }
  $ok = [AwWin]::SetWindowPos($h, [IntPtr](-1), $x, $y, $ow, $oh, $flags)
  "pid $id -> $where at ($x,$y) ${ow}x${oh} ok=$ok"
}
