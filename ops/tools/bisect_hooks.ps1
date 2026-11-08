# Second bisect: the DllMain-time hooks, gated at runtime by [Coop] InstallHooks / Hook*.
# One build, six variants, no menu input. Records survival per variant.
$ErrorActionPreference = 'Continue'
$g   = 'D:\SteamLibrary\steamapps\common\Assassin''s Creed IV Black Flag'
$ini = Join-Path $g 'AC4Coop.ini'
$log = 'D:\AC4-RE\bisect-hooks.log'
New-Item -ItemType Directory -Force -Path 'D:\AC4-RE' | Out-Null
function W($m) { $l = "$(Get-Date -Format 'HH:mm:ss') $m"; $l | Tee-Object -FilePath $log -Append }

$variants = @(
  @{ name='H0 all hooks on';        set=@{ InstallHooks=1; HookFrame=1; HookWindowed=1; HookPad=1; HookMultiInstance=1 } },
  @{ name='H1 no hooks at all';     set=@{ InstallHooks=0 } },
  @{ name='H2 frame hook off only'; set=@{ InstallHooks=1; HookFrame=0; HookWindowed=1; HookPad=1; HookMultiInstance=1 } },
  @{ name='H3 windowed off only';   set=@{ InstallHooks=1; HookFrame=1; HookWindowed=0; HookPad=1; HookMultiInstance=1 } },
  @{ name='H4 pad hook off only';   set=@{ InstallHooks=1; HookFrame=1; HookWindowed=1; HookPad=0; HookMultiInstance=1 } },
  @{ name='H5 mutex hook off only'; set=@{ InstallHooks=1; HookFrame=1; HookWindowed=1; HookPad=1; HookMultiInstance=0 } }
)

$orig = Get-Content $ini -Raw
function Set-Ini([hashtable]$kv) {
  $t = $orig
  foreach ($k in $kv.Keys) {
    if ($t -match "(?m)^$k=") { $t = $t -replace "(?m)^$k=.*$", "$k=$($kv[$k])" }
    else { $t = $t -replace '(?m)^\[Coop\]', "[Coop]`r`n$k=$($kv[$k])" }
  }
  [System.IO.File]::WriteAllText($ini, $t, (New-Object System.Text.UTF8Encoding($false)))
}

Get-Process AC4BFSP -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
Start-Sleep -Seconds 3
W "=== hook bisect start ==="

foreach ($v in $variants) {
  Set-Ini $v.set
  W "--- $($v.name)"
  $before = (Get-ChildItem $g -Filter 'phase0_log_*.txt' | Measure-Object).Count
  Start-Process 'steam://rungameid/242050' | Out-Null
  $seen = $false
  for ($i = 0; $i -lt 20; $i++) {
    Start-Sleep -Seconds 5
    $p = Get-Process AC4BFSP -EA SilentlyContinue
    if ($p) { $seen = $true } elseif ($seen) { break }
  }
  $p = Get-Process AC4BFSP -EA SilentlyContinue
  if ($p) { W "    SURVIVED  pid $($p.Id)" } else { W "    CRASHED / never started" }
  Get-Process AC4BFSP -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
  Start-Sleep -Seconds 6
}

[System.IO.File]::WriteAllText($ini, $orig, (New-Object System.Text.UTF8Encoding($false)))
W "=== hook bisect done, ini restored ==="
