# sea_route.ps1 - take one running copy from the Havana boot save to the Jackdaw's helm at sea, unattended.
#   .\tools\sea_route.ps1 <pid> [<pid> ...]
# Map fast travel to the sea point north of Havana (map (235,649)), the route of
# docs/evidence/fast-travel-and-havana-2026-11-22.md, replayed through the Pilot virtual pad ([Pilot] Enabled=1).
# Verified 2026-11-25 on both copies of a session: arrival at about (-4495, 2500), at the helm, ~35 s after confirm.
# The stick timings assume the boot save's map state (cursor on Edward at map (240,607), world zoom after RS).
# A frame is saved per pid to %TEMP%\sea_route_<pid>.png before the confirm so a miss can be read.
param([Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)][int[]]$Pids)
$pilot = Join-Path $PSScriptRoot 'pilot.py'
foreach ($p in $Pids) {
    py -3.14 $pilot --pid $p tap BACK --sec 0.15 | Out-Null; Start-Sleep 3          # map
    py -3.14 $pilot --pid $p tap RS --sec 0.15 | Out-Null; Start-Sleep 2            # world zoom
    py -3.14 $pilot --pid $p stick -0.15 1 --sec 0.27 | Out-Null; Start-Sleep 0.5   # toward the fast-travel icon
    py -3.14 $pilot --pid $p stick -0.05 1 --sec 0.1 | Out-Null; Start-Sleep 1
    py -3.14 $pilot --pid $p frame --out (Join-Path $env:TEMP "sea_route_$p.png") | Out-Null
    py -3.14 $pilot --pid $p tap X --sec 0.15 | Out-Null; Start-Sleep 1.5           # "fast travel"
    py -3.14 $pilot --pid $p tap A --sec 0.15 | Out-Null                            # confirm
}
Start-Sleep 35
foreach ($p in $Pids) {
    $s = py -3.14 $pilot --pid $p status 2>&1 | Select-String 'pos=' | ForEach-Object { $_.Line }
    $atSea = $s -match 'pos=\((-?\d+)' -and [double]$Matches[1] -lt -3000
    "$p $(if ($atSea) { 'AT SEA' } else { 'NOT AT SEA (read %TEMP%\sea_route_' + $p + '.png)' }) $s"
}
