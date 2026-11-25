# sea_route.ps1 - take running copies from the boot save to the Jackdaw's helm at sea, unattended.
#   .\tools\sea_route.ps1 <pid> [<pid> ...]
# Map fast travel to the sea point north of Havana (map (235,649)), the route of
# docs/evidence/fast-travel-and-havana-2026-11-22.md, replayed through the Pilot virtual pad ([Pilot] Enabled=1).
# Two boot places seen (2026-11-25): Havana city (x near 0) and Dry Tortuga (about (-4035, 4087)); the stick moves differ.
# Known miss (2026-11-25, 3/3): the Dry Tortuga moves end at the Havana docks (~(107,-77)); run the script again from there.
# Arrival: about (-4400..-4500, 2440..2560) at the helm, ~35 s after the confirm. Open-loop timings: if a copy reports
# NOT AT SEA, read %TEMP%\sea_route_<pid>.png (the map right before X) and nudge by hand.
param([Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)][int[]]$Pids)
$pilot = Join-Path $PSScriptRoot 'pilot.py'
function Get-Pos([int]$p) {
    $s = py -3.14 $pilot --pid $p status 2>&1 | Select-String 'pos=' | ForEach-Object { $_.Line }
    if ($s -match 'pos=\((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\)') { return @([double]$Matches[1], [double]$Matches[2], [double]$Matches[3]) }
    return $null
}
foreach ($p in $Pids) {
    $pos = Get-Pos $p
    $dryTortuga = $pos -and [math]::Abs($pos[0] + 4035) -lt 300 -and [math]::Abs($pos[1] - 4087) -lt 300
    py -3.14 $pilot --pid $p tap BACK --sec 0.15 | Out-Null; Start-Sleep 3          # map
    py -3.14 $pilot --pid $p tap RS --sec 0.15 | Out-Null; Start-Sleep 2            # world zoom
    if ($dryTortuga) {                                                              # cursor starts at map ~(249,789)
        py -3.14 $pilot --pid $p stick -0.1 -1 --sec 1.0 | Out-Null; Start-Sleep 0.5
        py -3.14 $pilot --pid $p stick 0.6 0.9 --sec 0.2 | Out-Null; Start-Sleep 0.5
        py -3.14 $pilot --pid $p stick 0.5 0.9 --sec 0.08 | Out-Null; Start-Sleep 0.5
        py -3.14 $pilot --pid $p stick 0.3 0.8 --sec 0.06 | Out-Null; Start-Sleep 1
    } else {                                                                        # Havana: cursor at map ~(240,607)
        py -3.14 $pilot --pid $p stick -0.15 1 --sec 0.27 | Out-Null; Start-Sleep 0.5
        py -3.14 $pilot --pid $p stick -0.05 1 --sec 0.1 | Out-Null; Start-Sleep 1
        $havanaEdge = $pos -and [math]::Abs($pos[0] - 104) -lt 30 -and [math]::Abs($pos[1] + 74) -lt 30
        if ($havanaEdge) {                                                          # (104..108,-74..-78): cursor lands at ~(238,640)
            py -3.14 $pilot --pid $p stick -0.4 0.9 --sec 0.12 | Out-Null; Start-Sleep 1
            py -3.14 $pilot --pid $p stick -0.3 0.9 --sec 0.08 | Out-Null; Start-Sleep 1   # 2026-11-25: needed to reach (235,649)
        }
    }
    py -3.14 $pilot --pid $p frame --out (Join-Path $env:TEMP "sea_route_$p.png") | Out-Null
    py -3.14 $pilot --pid $p tap X --sec 0.15 | Out-Null; Start-Sleep 1.5           # "fast travel"
    py -3.14 $pilot --pid $p tap A --sec 0.15 | Out-Null                            # confirm
}
Start-Sleep 35
foreach ($p in $Pids) {
    $pos = Get-Pos $p
    $atSea = $pos -and $pos[0] -lt -4300 -and $pos[0] -gt -4700 -and $pos[1] -gt 2300 -and $pos[1] -lt 2700
    "$p $(if ($atSea) { 'AT SEA' } else { 'NOT AT SEA (read %TEMP%\sea_route_' + $p + '.png)' }) pos=$($pos -join ',')"
}
