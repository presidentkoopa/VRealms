<#
.SYNOPSIS
  Capture frames from the ORIGINAL (ROTH.C), headless, in any map.

.DESCRIPTION
  NO MOVIES, NO CD PROMPT, NO MENUS. The intro movies are not a cosmetic
  annoyance: they decode and stream audio for seconds before the title appears,
  which pushed capture attempts into the pose watchdog and made a working rig
  look broken. They are skipped by oraclelog refusing to play the cutscene, NOT
  by rothc's --skip-gdv -- that flag hides the .GDV files, the open then fails,
  and a failed open raises the CD-swap retry prompt that nothing headless can
  answer. Never add it back.

  NO QUICKSAVE NEEDED. -Map drives the game's own warp path from the plugin
  (see VA_WARP_DEST in rothdiff_plugin.c), so any of the 44 maps can be captured
  without a human making a save first. The single quicksave slot used to mean
  that saving in a new map destroyed the previous map's capturability.

  STARTS FROM THE QUICKSAVE by default. The plugin arms the game's own savegame
  request globals (key_quickload is nothing but three writes, input.c:548), so
  there is no key to press, no menu to navigate and nothing to time. Pass
  -NoQuickload for a fresh game instead.

.EXAMPLE
  .\run_oracle.ps1 -Map STUDY2 -PoseFile poses_study2.csv
#>
[CmdletBinding()]
param(
    [string]$Map = '',
    [string]$PoseFile = 'poses_frame.csv',
    [string]$OutDir = '',
    [int]$Settle = 200,
    [int]$Hold = 20,
    [int]$TimeoutSec = 150,
    [switch]$Quickload,
    [switch]$KeepOpen,
    [string]$GameDir = 'E:\DOOMWork\_oracle',
    [string]$CRoot = 'E:\DOOMWork\_croot'
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not [System.IO.Path]::IsPathRooted($PoseFile)) { $PoseFile = Join-Path $here $PoseFile }
if ($OutDir -eq '') { $OutDir = Join-Path $here 'captures' }
if (-not (Test-Path $PoseFile)) { throw "no pose file at $PoseFile" }
if (-not (Test-Path (Join-Path $GameDir 'rothc.exe'))) { throw "no rothc.exe in $GameDir" }

# Both mods have to be installed or the run never reaches a map: oraclelog
# presses Play, rothdiff pins the pose and writes the frame.
foreach ($m in @('oraclelog', 'rothdiff')) {
    $dll = Join-Path $GameDir "mods\$m\plugin.dll"
    if (-not (Test-Path $dll)) { throw "missing mod: $dll" }
    if ((Get-Item $dll).Length -lt 1000) { throw "$dll is only $((Get-Item $dll).Length) bytes -- rebuild it (build_plugin.cmd)" }
}

$env:ROTHDIFF_POSEFILE = $PoseFile
$env:ROTHDIFF_OUTDIR   = $OutDir
$env:ROTHDIFF_SETTLE   = "$Settle"
$env:ROTHDIFF_HOLD     = "$Hold"
$env:ROTHDIFF_TIMEOUT  = "$TimeoutSec"
if ($Map -ne '') { $env:ROTHDIFF_MAP = $Map } else { Remove-Item Env:ROTHDIFF_MAP -ErrorAction SilentlyContinue }
# QUICKSAVE START IS OPT-IN, AND CURRENTLY HANGS. Arming the savegame request
# leaves the game rendering but not advancing 0x90bcc, and on_frame_game is
# deduped on that tick, so the rig stops being called and nothing is ever
# captured -- it looks like a frozen game. The warp below reaches any map
# without a save at all, which is what the quickload was wanted for.
if ($Quickload) { $env:ROTHDIFF_QUICKLOAD = '1' } else { Remove-Item Env:ROTHDIFF_QUICKLOAD -ErrorAction SilentlyContinue }
# Without this the game stays up after the last capture and has to be closed.
if ($KeepOpen)   { Remove-Item Env:ROTHDIFF_QUIT -ErrorAction SilentlyContinue } else { $env:ROTHDIFF_QUIT = '1' }

$stamp = Get-Date -Format 'HHmmss'
$log = Join-Path $OutDir "oracle_$stamp.log"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Write-Host "  map      $(if ($Map -ne '') { $Map } else { '(whatever the game starts in)' })"
Write-Host "  poses    $PoseFile"
Write-Host "  log      $log"

# REDIRECT THROUGH CMD, NOT POWERSHELL. In 5.1, piping a native exe's stderr
# through `2>&1` wraps every line in an ErrorRecord; with $ErrorActionPreference
# set to Stop that turns the game's first ordinary boot message into a
# terminating error and the run dies at the title with an empty log. The game
# writes everything useful to stderr, so this is not avoidable by ignoring it.
$exe = Join-Path $GameDir 'rothc.exe'
# NO --skip-gdv. It hides the .GDV files, the open then fails, and a failed open
# raises the CD-swap retry prompt (file_config.c:913) which headless nobody can
# answer -- the run hangs asking for the CD. oraclelog refuses to PLAY the
# cutscene instead, so nothing is hidden and no prompt is ever raised.
$cmd = '"{0}" --headless --game-dir "{1}" --c-root "{2}" > "{3}" 2>&1' -f $exe, $GameDir, $CRoot, $log
Push-Location $GameDir
try {
    & cmd.exe /c $cmd | Out-Null
} finally {
    Pop-Location
}

Select-String -Path $log -Pattern '\[rothdiff\]|pressed Play' |
    ForEach-Object { $_.Line }
