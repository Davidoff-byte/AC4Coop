# Builds the co-op launcher package: <Out>\AC4Coop-Launcher-<commit>\ (+ .zip). Run after the Release build.
#   AC4CoopLauncher.exe, README.txt
#   files\AC4Coop.asi  dinput8.dll (Ultimate ASI Loader)  known_builds.txt  AC4Coop.ini (template, used only if none)
#   files\eos\      eos_bridge.exe + EOSSDK-Win64-Shipping.dll + XAudio2_9Redist.dll + x64 VC runtime + eos.ini
#   files\coop_ui\  overlay.rml / overlay.rcss / Lato fonts + licence
# eos.ini holds the EOS client credentials: it comes from the ignored local\eos.ini, goes into the package only, and is
# never printed. The package and zip must never be committed.
# -Overrides <file>: key=value lines applied to the packaged AC4Coop.ini's [Coop] section (replace in place, else add).
# -Guide <file>: copied into the package as GUIDE.md. -Tag <name>: appended to the package name (e.g. test-2026-11-25).
param(
    [string]$Out = (Join-Path $PSScriptRoot "..\..\..\dist"),
    [string]$Loader = (Join-Path $PSScriptRoot "..\..\..\dist\AC4Coop-Launcher-8afd481\files\dinput8.dll"),
    [string]$Overrides = "",
    [string]$Guide = "",
    [string]$Tag = ""
)
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$commit = (git -C $root rev-parse --short HEAD).Trim()
$dirty = (git -C $root status --porcelain --untracked-files=no) -ne $null
$name = "AC4Coop-Launcher-$commit" + $(if ($dirty) { "-dirty" } else { "" }) + $(if ($Tag) { "-$Tag" } else { "" })
$pkg = Join-Path $Out $name
$need = @{
    "bin\AC4CoopLauncher.exe" = "AC4CoopLauncher.exe"; "bin\AC4Coop.asi" = "files\AC4Coop.asi";
    "known_builds.txt" = "files\known_builds.txt"; "AC4Coop.ini" = "files\AC4Coop.ini";
    "local\eos.ini" = "files\eos\eos.ini"
}
foreach ($f in "eos_bridge.exe", "EOSSDK-Win64-Shipping.dll", "XAudio2_9Redist.dll", "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll") { $need["local\eos-bridge\$f"] = "files\eos\$f" }
foreach ($f in Get-ChildItem (Join-Path $root "coop_ui") -File) { $need["coop_ui\$($f.Name)"] = "files\coop_ui\$($f.Name)" }
foreach ($k in $need.Keys) { if (-not (Test-Path (Join-Path $root $k))) { throw "missing $k" } }
if (-not (Test-Path $Loader)) { throw "missing loader $Loader (Ultimate ASI Loader, Win32)" }
$prod = (Get-Item -LiteralPath $Loader).VersionInfo.ProductName
if ($prod -notmatch "ASI.Loader") { throw "$Loader is not Ultimate ASI Loader (ProductName '$prod')" }

if (Test-Path $pkg) { Remove-Item -Recurse -Force $pkg }
New-Item -ItemType Directory -Force (Join-Path $pkg "files\eos"), (Join-Path $pkg "files\coop_ui") | Out-Null
foreach ($k in $need.Keys) { Copy-Item -LiteralPath (Join-Path $root $k) -Destination (Join-Path $pkg $need[$k]) }
Copy-Item -LiteralPath $Loader -Destination (Join-Path $pkg "files\dinput8.dll")
if ($Overrides) {
    $iniPath = Join-Path $pkg "files\AC4Coop.ini"
    $ini = [IO.File]::ReadAllText($iniPath)
    foreach ($line in Get-Content $Overrides) {
        if ($line -match '^\s*#' -or $line -notmatch '^\s*(\w+)\s*=\s*(.*)$') { continue }
        $k = $Matches[1]; $v = $Matches[2].Trim()
        $coop = [regex]::Match($ini, '(?ms)^\[Coop\][^\r\n]*\r?\n.*?(?=^\[|\z)')
        if (-not $coop.Success) { throw "no [Coop] section in the ini" }
        $body = $coop.Value
        if ($body -match "(?m)^$k\s*=") { $body = [regex]::Replace($body, "(?m)^$k\s*=.*$", "$k=$v") }
        else { $body = [regex]::Replace($body, '^(\[Coop\][^\r\n]*\r?\n)', "`$1$k=$v`r`n") }
        $ini = $ini.Substring(0, $coop.Index) + $body + $ini.Substring($coop.Index + $coop.Length)
        "override $k=$v"
    }
    [IO.File]::WriteAllText($iniPath, $ini)
}
if ($Guide) { Copy-Item -LiteralPath $Guide -Destination (Join-Path $pkg "GUIDE.md") }
@"
AC4 Co-op ($name)

1. Unzip this folder anywhere (keep the files\ folder next to AC4CoopLauncher.exe).
2. Start Steam and Ubisoft Connect.
3. Run AC4CoopLauncher.exe, check it found your game, press PLAY CO-OP.
4. In the game press F10: one player clicks Host and sends the 6-letter code, the other types it and clicks Join.

Starting the game from Steam or Ubisoft Connect runs the normal game without co-op.
Logs: overlay_log.txt / net_log.txt in the game folder (or %LOCALAPPDATA%\AC4Coop\logs), eos\bridge.log.
"@ | Set-Content -Encoding ascii (Join-Path $pkg "README.txt")
$zip = "$pkg.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $pkg "*") -DestinationPath $zip
$asi = (Get-FileHash (Join-Path $pkg "files\AC4Coop.asi") -Algorithm SHA256).Hash.ToLower()
"package $pkg"
"zip     $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
"asi     sha256 $asi"
"loader  $prod $((Get-Item -LiteralPath $Loader).VersionInfo.FileVersion)"
