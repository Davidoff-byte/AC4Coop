# Generates src\build_info.h -- the commit/tree stamp compiled into AC4Coop.asi (task 0.2).
#
# Runs as a PreBuildEvent for both Debug and Release, so every ASI knows which tree it came from and
# phase1_log_<pid>.txt can name it. This script must NEVER fail a build: no git, a detached worktree,
# a shallow copy or a read-only checkout just yields "unknown" and the build continues.
#
# Output is UTF-8 without BOM with LF endings: the rest of src\ is LF, and a stray BOM would show up as
# a diff on a file nobody edits by hand.

param([string]$Out = 'src\build_info.h', [string[]]$DirtyPaths = @('src', 'AC4Coop.ini', 'decomp', 'tools'))

$ErrorActionPreference = 'Continue'

$root = Split-Path -Parent $PSScriptRoot          # tools\ -> repo root
$out  = Join-Path $root $Out

function Warn([string]$msg) {
    Write-Warning ("make_build_info: " + $msg)
}

$commit = 'unknown'
$tree   = 'unknown'

# --- commit ---------------------------------------------------------------------------------------
try {
    $c = & git -C $root rev-parse --short HEAD 2>$null
    if ($LASTEXITCODE -eq 0 -and $c) {
        $c = ($c | Select-Object -First 1).Trim()
        if ($c -match '^[0-9a-fA-F]{4,40}$') { $commit = $c }
        else { Warn "unexpected commit '$c'" }
    } else {
        Warn "git rev-parse failed (exit $LASTEXITCODE)"
    }
} catch {
    Warn ("git rev-parse threw: " + $_.Exception.Message)
}

# --- tree -----------------------------------------------------------------------------------------
# Paths that decide a run's behaviour, not just the binary: src\ (the ASI is built from it), the shipped
# ini template AC4Coop.ini (a hand-edited flag changes the run as much as a code edit), decomp\ (the
# extracted engine units the gates check src\ against) and tools\ (the gates and this stamp itself).
# A dirty README still does not count. The generated header is excluded so the stamp can never make the
# tree dirty (it is gitignored too; the filter also covers the case of the file being tracked by mistake).
# Untracked files under these paths count as dirty (porcelain '??').
# `powershell -File ... -DirtyPaths v2,decomp,tools` passes ONE string "v2,decomp,tools" (no array parsing under -File),
# which matched no path and stamped tree=clean on a dirty tree (v2 boot logs 2026-11-28). Split on commas.
$dirtyPaths = @($DirtyPaths | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
try {
    $porcelain = & git -C $root status --porcelain -- @dirtyPaths 2>$null
    if ($LASTEXITCODE -eq 0) {
        $dirty = @($porcelain | Where-Object { $_ -and ($_ -notmatch 'build_info\.h$') })
        $tree = if ($dirty.Count -eq 0) { 'clean' } else { 'dirty' }
    } else {
        Warn "git status failed (exit $LASTEXITCODE)"
    }
} catch {
    Warn ("git status threw: " + $_.Exception.Message)
}

# --- header ---------------------------------------------------------------------------------------
$text = "#pragma once`n" +
        "#define BUILD_GIT_COMMIT `"$commit`"`n" +
        "#define BUILD_GIT_TREE   `"$tree`"`n"

try {
    $enc = New-Object System.Text.UTF8Encoding($false)   # $false = no BOM
    [System.IO.File]::WriteAllText($out, $text, $enc)
    Write-Host "make_build_info: commit=$commit tree=$tree -> src\build_info.h"
} catch {
    Warn ("could not write " + $out + ": " + $_.Exception.Message)
}

exit 0
