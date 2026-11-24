# two_copy.ps1 - run host + guest as two AC4BFSP.exe copies on this PC (S0 test rig, docs/design/storm-replication.md).
#
#   tools\two_copy.ps1 [-Set Key=Value,...] [-Moves tools\two_copy_moves.py] [-Interactive] [-Fps 0] [-GameDir <dir>]
#
# Steps:
#  1. Close leftover copies.
#  2. Apply -Set keys to AC4Coop.ini.
#  3. Start copy A directly from the exe (never through Steam) with Mode=host, and read its LAN code from net_log.txt.
#  4. Switch the ini to Mode=join JoinCode=<code> and start copy B. Once B has logged its [Net] cfg, set the ini back to
#     the run settings.
#  5. Boot both into the world with pilot.py, with --fps 0 by default: the pilot frame-cap Present patch preceded both
#     freezes in docs/evidence/act-match-ab-2026-11-15.md.
#  6. Place the windows: host top-right, guest bottom-left, 960x540 (tools\arrange_windows.ps1 -Resize).
#  7. With -Moves: run the script on the host, then print the [ActMatch] and [Net] stats lines of both copies.
#     With -Interactive: stop here and leave both copies running for a person.
# The ini is restored to its starting bytes in every case (finally). A copy that stops responding with no CPU progress
# is dumped (tools\minidump.py) to D:\AC4-RE\dumps before the run aborts.
# Needs [Pilot] Enabled=1 in the ini, the proxy\version version.dll in the game folder, and py -3.14.
param(
  [string[]]$Set = @(),
  [string]$Moves = '',
  [switch]$Interactive,
  [int]$Fps = 0,
  [string]$GameDir = $(if ($env:AC4_GAME_DIR) { $env:AC4_GAME_DIR } else { "D:\SteamLibrary\steamapps\common\Assassin's Creed IV Black Flag" })
)
$ErrorActionPreference = 'Stop'
$tools = Split-Path $MyInvocation.MyCommand.Path
$ini = Join-Path $GameDir 'AC4Coop.ini'; $netlog = Join-Path $GameDir 'net_log.txt'; $exe = Join-Path $GameDir 'AC4BFSP.exe'
$dumpDir = 'D:\AC4-RE\dumps'
$orig = [IO.File]::ReadAllText($ini); $origHash = (Get-FileHash $ini).Hash

function Set-Key([string]$txt, [string]$key, [string]$val) {
  if ($txt -match "(?m)^$([regex]::Escape($key))=") { return $txt -replace "(?m)^$([regex]::Escape($key))=.*$", "$key=$val" }
  return $txt -replace "(?m)^\[Coop\]\s*$", "[Coop]`r`n$key=$val"   # unknown keys go to [Coop]
}
function Write-Ini([string]$txt) {
  for ($i = 0; $i -lt 20; $i++) { try { [IO.File]::WriteAllText($ini, $txt); return } catch { Start-Sleep -Milliseconds 250 } }
  throw "ini write failed"
}
function Test-Hung([int]$id) {
  $p = Get-Process -Id $id -ErrorAction SilentlyContinue
  if (-not $p) { return "pid $id exited" }
  $c1 = $p.CPU; Start-Sleep 3; $p.Refresh()
  if (-not $p.Responding -and ($p.CPU - $c1) -lt 0.5) {
    New-Item -ItemType Directory -Force $dumpDir | Out-Null
    $dmp = Join-Path $dumpDir "hang-$id-$(Get-Date -Format yyyyMMdd-HHmmss).dmp"
    # The game is 32-bit. A dump written by 64-bit Python holds only the WOW64 host stacks (run 28, 2026-11-24: "Guest (WoW)
    # stack is not available" on every thread), so the x86 debugger attaches non-invasively first and writes the 32-bit stacks.
    $cdb86 = "${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x86\cdb.exe"
    if (Test-Path $cdb86) {
      & $cdb86 -pv -p $id -c "~*kv 40; .dump /mt `"$dmp.x86.dmp`"; qd" 2>&1 | Out-File -Encoding utf8 "$dmp.stacks.txt"
      "  stacks: $dmp.stacks.txt"
    }
    & py -I (Join-Path $tools 'minidump.py') $id $dmp 2>&1 | ForEach-Object { "  dump: $_" }
    return "pid $id HUNG (not responding, cpu +$([math]::Round($p.CPU - $c1,2))s in 3s); dump $dmp"
  }
  return $null
}

$run = $orig
foreach ($kv in $Set) { $k, $v = $kv -split '=', 2; $run = Set-Key $run $k.Trim() $v.Trim() }
try {
  $old = Get-Process AC4BFSP -ErrorAction SilentlyContinue
  if ($old) {
    $old | ForEach-Object { [void]$_.CloseMainWindow() }; Start-Sleep 10
    Get-Process AC4BFSP -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 2
    "closed old copies: $($old.Id -join ',')"
  }
  Write-Ini ((Set-Key (Set-Key $run 'Mode' 'host') 'EntityHost' '1'))
  $off0 = (Get-Item $netlog).Length
  $a = Start-Process -FilePath $exe -WorkingDirectory $GameDir -PassThru; "A (host) pid=$($a.Id)"
  $code = $null
  for ($i = 0; $i -lt 90 -and -not $code; $i++) {
    Start-Sleep 1
    $fs = [IO.File]::Open($netlog, 'Open', 'Read', 'ReadWrite'); [void]$fs.Seek($off0, 'Begin')
    $new = (New-Object IO.StreamReader($fs)).ReadToEnd(); $fs.Close()
    if ($new -match 'host LAN code (\S+) \(') { $code = $Matches[1] }
  }
  if (-not $code) { throw "no host code from A in 90 s" }
  "host code $code"
  Write-Ini (Set-Key (Set-Key (Set-Key $run 'Mode' 'join') 'JoinCode' $code) 'EntityHost' '0')
  $b = Start-Process -FilePath $exe -WorkingDirectory $GameDir -PassThru; "B (guest) pid=$($b.Id)"
  $bLog = Join-Path $GameDir "phase1_log_$($b.Id).txt"
  for ($i = 0; $i -lt 90; $i++) { Start-Sleep 1; if ((Test-Path $bLog) -and (Select-String -Path $bLog -Pattern '\[Net\] cfg ' -Quiet)) { break } }
  Write-Ini ((Set-Key (Set-Key $run 'Mode' 'host') 'EntityHost' '1'))
  "B cfg: " + ((Select-String -Path $bLog -Pattern '\[Net\] cfg ').Line)

  foreach ($id in $a.Id, $b.Id) {
    $inWorld = $false
    for ($t = 1; $t -le 4 -and -not $inWorld; $t++) {
      $out = Join-Path $env:TEMP "two_copy_boot_$id.txt"
      $bp = Start-Process py -ArgumentList @('-3.14', "`"$(Join-Path $tools 'pilot.py')`"", '--pid', $id, 'boot', '--pulses', '4', '--wait', '6', '--fps', $Fps) `
            -RedirectStandardOutput $out -NoNewWindow -PassThru
      if (-not $bp.WaitForExit(120000)) { $bp | Stop-Process -Force; "boot $id try $t : pilot timed out" }
      $last = (Get-Content $out -ErrorAction SilentlyContinue | Select-Object -Last 1) -join ''
      "boot $id try $t : $last"; $inWorld = $last -match '^in world'
      foreach ($q in $a.Id, $b.Id) { $hung = Test-Hung $q; if ($hung) { throw $hung } }
    }
    if (-not $inWorld) { throw "pid $id never reached the world" }
  }
  & (Join-Path $tools 'arrange_windows.ps1') $a.Id $b.Id -Resize
  Start-Sleep 16   # puppets spawned; lazily read flags (e.g. NativeReplica on first apply) are read before the restore
  if ($Interactive) { "READY: host A=$($a.Id) (top-right), guest B=$($b.Id) (bottom-left)"; return }
  if ($Moves) {
    "--- moves on host A ($Moves)"
    & py -3.14 $Moves (Split-Path $tools) $a.Id 2>&1
    Start-Sleep 7
  }
  foreach ($q in $a.Id, $b.Id) { $hung = Test-Hung $q; if ($hung) { "AFTER MOVES: $hung" } }
  foreach ($id in $a.Id, $b.Id) {
    $role = if ($id -eq $a.Id) { 'host A (its puppet = B)' } else { 'guest B (its puppet = A, the mover)' }
    "=== $role pid=$id"
    Select-String -Path (Join-Path $GameDir "phase1_log_$id.txt") -Pattern '\[ActMatch\]|\[Net\] stats' | ForEach-Object { "  $($_.Line)" }
  }
  "DONE: host A=$($a.Id) guest B=$($b.Id)"
} catch {
  "RUN ABORTED: $_"
} finally {
  Write-Ini $orig
  "ini restored: $((Get-FileHash $ini).Hash -eq $origHash)"
}
