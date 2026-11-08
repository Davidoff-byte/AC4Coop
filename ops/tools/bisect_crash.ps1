# Bisect which mod feature crashes AC4 at launch - fully automated, no menu input needed.
# Each variant sets ini flags, launches via Steam, waits, and records whether the process survived.
$ErrorActionPreference = 'Continue'
$g   = 'D:\SteamLibrary\steamapps\common\Assassin''s Creed IV Black Flag'
$ini = Join-Path $g 'AC4Coop.ini'
$log = 'D:\AC4-RE\bisect.log'
New-Item -ItemType Directory -Force -Path 'D:\AC4-RE' | Out-Null
function W($m) { $l = "$(Get-Date -Format 'HH:mm:ss') $m"; $l | Tee-Object -FilePath $log -Append }

# key=value overrides per variant; empty = leave the ini untouched
$variants = @(
  @{ name='V0 baseline (as shipped)';      set=@{} },
  @{ name='V1 deref guards off';           set=@{ GuardNullTarget=0; GuardStaleTarget=0 } },
  @{ name='V2 action guard off';           set=@{ GuardNullActionTarget=0 } },
  @{ name='V3 pad hook off';               set=@{ LegacyPadIdHook=0 } },
  @{ name='V4 windowed hook off';          set=@{ ForceWindowedMs=0 } },
  @{ name='V5 net off';                    set=@{ Enabled=0 } },
  @{ name='V6 everything off';             set=@{ GuardNullTarget=0; GuardStaleTarget=0; GuardNullActionTarget=0; LegacyPadIdHook=0; ForceWindowedMs=0; Enabled=0 } }
)

$orig = Get-Content $ini -Raw
function Set-Ini([hashtable]$kv) {
  $t = $orig
  foreach ($k in $kv.Keys) {
    $v = $kv[$k]
    if ($t -match "(?m)^$k=") { $t = $t -replace "(?m)^$k=.*$", "$k=$v" }
    else { $t = $t -replace '(?m)^\[Coop\]', "[Coop]`r`n$k=$v" }
  }
  [System.IO.File]::WriteAllText($ini, $t, (New-Object System.Text.UTF8Encoding($false)))
}

Get-Process AC4BFSP -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
Start-Sleep -Seconds 3
W "=== bisect start ==="

foreach ($v in $variants) {
  Set-Ini $v.set
  W "--- $($v.name)"
  Start-Process 'steam://rungameid/242050' | Out-Null
  $alive = $false
  for ($i = 0; $i -lt 24; $i++) {
    Start-Sleep -Seconds 5
    $p = Get-Process AC4BFSP -EA SilentlyContinue
    if ($p) { $alive = $true } elseif ($alive) { break }   # was up, now gone = crashed
  }
  $p = Get-Process AC4BFSP -EA SilentlyContinue
  if ($p) {
    W "    RESULT survived  (pid $($p.Id))"
    Start-Sleep -Seconds 10
    $p2 = Get-Process AC4BFSP -EA SilentlyContinue
    if ($p2) { W "    still alive after +10s" } else { W "    died in the extra 10s" }
  } else {
    W "    RESULT crashed or never started"
  }
  Get-Process AC4BFSP -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
  Start-Sleep -Seconds 5
}

[System.IO.File]::WriteAllText($ini, $orig, (New-Object System.Text.UTF8Encoding($false)))
W "=== bisect done, ini restored ==="
Get-Content $log
