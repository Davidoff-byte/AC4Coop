# S-boot (docs/v2/ARCHITECTURE.md s9): two copies on this PC boot into the world with an ASI and stay alive.
#   tools\scenarios\S-boot.ps1 -Asi bin\v2\DevTools\AC4Coop.asi [-Ini v2\AC4Coop.ini] [-Minutes 10] [-SaveFrom <dir with *.save>]
# -Ini installs that ini for the run (restored after) with [Pilot] Enabled=1, so the pilot can drive the boot.
# PASS iff, on both copies:
#   B1 the log names the build ("[Build] asi=<sha256 of -Asi>")
#   B2 "[Init] done: N hook site(s), 0 refused" and the manifest has every expected site (-Sites)
#   B3 "[Tick] in world" within 4 min of start, via tools\pilot.py boot
#   B4 median "[Stats] tick hz" >= 20 over the soak
#   B5 both processes alive after -Minutes, no AC4Coop_crashctx_<pid>_*.txt, no dump
# Saves, ini and ASI are backed up first and restored in every case. Started from the exe, never Steam. Rig lock held.
# The display is kept awake for the run (tools\keep_awake.ps1): asleep, the copies run at ~30 fps instead of ~60.
param(
  [Parameter(Mandatory)][string]$Asi,
  [int]$Minutes = 10,
  [string]$SaveFrom = '',
  [string]$Ini = '',
  [string[]]$Sites = @('tick/HumanUpdate', 'boot/CreateMutexW', 'boot/WindowStyle', 'boot/NoPause', 'boot/BinkOpen', 'boot/TitleStartUpdate', 'boot/TitleSlotHover'),
  [string]$GameDir = $(if ($env:AC4_GAME_DIR) { $env:AC4_GAME_DIR } else { "D:\SteamLibrary\steamapps\common\Assassin's Creed IV Black Flag" })
)
$ErrorActionPreference = 'Stop'
$tools = Split-Path (Split-Path $MyInvocation.MyCommand.Path)
$lock = 'D:\AC4-RE\rig.lock'
$exe = Join-Path $GameDir 'AC4BFSP.exe'; $ini = Join-Path $GameDir 'AC4Coop.ini'; $gameAsi = Join-Path $GameDir 'AC4Coop.asi'
$saves = Join-Path $GameDir 'save3dmgames'
if (Get-Process AC4BFSP -ErrorAction SilentlyContinue) { throw 'AC4BFSP already running' }
if (Test-Path $lock) { throw "rig locked: $(Get-Content $lock)" }
$asiHash = (Get-FileHash $Asi -Algorithm SHA256).Hash.ToLower()
$bk = "D:\AC4-RE\deploy-backups\$(Get-Date -Format yyyyMMdd-HHmmss)-S-boot"
New-Item -ItemType Directory -Force $bk | Out-Null
Set-Content $lock "S-boot $Asi $(Get-Date -Format s)"
$results = [ordered]@{}; $ids = @()
$awake = Start-Process powershell -ArgumentList @('-NoProfile', '-File', (Join-Path $tools 'keep_awake.ps1'), '-Minutes', ($Minutes + 20)) -WindowStyle Hidden -PassThru
try {
  Copy-Item $ini, $gameAsi $bk -Force
  if (Test-Path $saves) { Copy-Item $saves (Join-Path $bk 'save3dmgames') -Recurse -Force }
  if ($SaveFrom) { Copy-Item (Join-Path $SaveFrom '*.save') $saves -Force }
  Copy-Item $Asi $gameAsi -Force
  if ($Ini) {
    # [Pilot] Enabled=1 in the installed copy; no BOM (GetPrivateProfileStringA would not see the first section).
    $sec = ''; $lines = foreach ($l in (Get-Content $Ini)) {
      if ($l -match '^\s*\[(.+)\]') { $sec = $Matches[1] }
      if ($sec -eq 'Pilot' -and $l -match '^\s*Enabled\s*=') { 'Enabled=1' } else { $l }
    }
    [IO.File]::WriteAllLines($ini, [string[]]$lines, (New-Object Text.UTF8Encoding $false))
    "ini $Ini -> game ([Pilot] Enabled=1)"
  }
  "asi $($asiHash.Substring(0,16)) -> game; backup $bk"
  $a = Start-Process -FilePath $exe -WorkingDirectory $GameDir -PassThru; Start-Sleep 6
  $b = Start-Process -FilePath $exe -WorkingDirectory $GameDir -PassThru
  $ids = @($a.Id, $b.Id); "copies: $($ids -join ', ')"
  $t0 = Get-Date
  foreach ($id in $ids) {
    $ok = $false
    for ($t = 1; $t -le 4 -and -not $ok; $t++) {
      $out = Join-Path $env:TEMP "s_boot_$id.txt"
      $bp = Start-Process py -ArgumentList @('-3.14', "`"$(Join-Path $tools 'pilot.py')`"", '--pid', $id, 'boot', '--pulses', '4', '--wait', '6', '--fps', 0) -RedirectStandardOutput $out -NoNewWindow -PassThru
      if (-not $bp.WaitForExit(120000)) { $bp | Stop-Process -Force }
      $last = (Get-Content $out -ErrorAction SilentlyContinue | Select-Object -Last 1) -join ''
      "boot $id try $t : $last"; $ok = $last -match '^in world'
    }
  }
  & (Join-Path $tools 'arrange_windows.ps1') $a.Id $b.Id -Resize | Out-Null
  $deadline = $t0.AddMinutes($Minutes)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep 20
    $alive = @($ids | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })
    if ($alive.Count -lt 2) { "copy exited at $([int]((Get-Date) - $t0).TotalSeconds) s"; break }
  }
  foreach ($id in $ids) {
    $log = Join-Path $GameDir "phase1_log_$id.txt"
    $txt = if (Test-Path $log) { Get-Content $log } else { @() }
    $b1 = [bool]($txt | Select-String -SimpleMatch "[Build] asi=$asiHash")
    $init = $txt | Select-String '\[Init\] done: (\d+) hook site\(s\), (\d+) refused' | Select-Object -Last 1
    $missing = @($Sites | Where-Object { $o, $n = $_ -split '/'; -not ($txt | Select-String -Pattern ('\[Hooks\]\s+' + [regex]::Escape($o) + '\s+' + [regex]::Escape($n) + '\s')) })
    $b2 = $init -and $init.Matches[0].Groups[2].Value -eq '0' -and $missing.Count -eq 0
    $b3 = [bool]($txt | Select-String '\[Tick\] in world')
    $hz = @($txt | Select-String '\[Stats\] tick hz=([\d.]+)' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value } | Sort-Object)
    $med = if ($hz.Count) { $hz[[int]($hz.Count / 2)] } else { 0 }
    $b4 = $med -ge 20
    $crash = @(Get-ChildItem (Join-Path $GameDir "AC4Coop_crashctx_${id}_*.txt") -ErrorAction SilentlyContinue).Count
    $dump = Test-Path "$env:LOCALAPPDATA\CrashDumps\AC4BFSP.exe.$id.dmp"
    $b5 = [bool](Get-Process -Id $id -ErrorAction SilentlyContinue) -and $crash -eq 0 -and -not $dump
    $results["$id"] = "B1=$b1 B2=$b2 (init: $($init.Line -replace '.*\[Init\] ', ''); missing: $($missing -join ',')) B3=$b3 B4=$b4 (median hz $med, n $($hz.Count)) B5=$b5 (crash $crash dump $dump)"
  }
} finally {
  Get-Process AC4BFSP -ErrorAction SilentlyContinue | ForEach-Object { [void]$_.CloseMainWindow() }; Start-Sleep 8
  Get-Process AC4BFSP -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 2
  Copy-Item (Join-Path $bk 'AC4Coop.ini') $ini -Force; Copy-Item (Join-Path $bk 'AC4Coop.asi') $gameAsi -Force
  if (Test-Path (Join-Path $bk 'save3dmgames')) { Copy-Item (Join-Path $bk 'save3dmgames\*') $saves -Recurse -Force }
  Remove-Item $lock -ErrorAction SilentlyContinue
  if ($awake) { $awake | Stop-Process -Force -ErrorAction SilentlyContinue }
  "restored ini/asi/saves; lock released"
}
$pass = $true
foreach ($k in $results.Keys) { "$k : $($results[$k])"; if ($results[$k] -match '=False') { $pass = $false } }
if ($results.Count -lt 2) { $pass = $false }
"S-boot " + $(if ($pass) { 'PASS' } else { 'FAIL' })
