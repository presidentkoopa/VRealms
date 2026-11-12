# CLAUDE BRIDGE, REMAROTH -- lets a Claude session request a FIXED set of actions on this PC,
# for THIS repo only. Separate from the UZDXREMA bridge on purpose: its own request folder, its
# own build directory (build-dxr inside this repo) and its own engine config, so a lane on the
# other engine and a lane here cannot step on each other.
#
# The owner starts it by double-clicking START_CLAUDE_BRIDGE_REMAROTH.cmd; closing its window
# stops it. It derives the repo root from where this file sits (tools\claude_bridge\), so it
# works wherever the repo is checked out; a copy kept outside the repo reads repopath.txt.
#
# Claude writes  requests\<id>.req   (line 1 = action, following lines = arguments, one per line)
# The bridge writes responses\<id>.out (all output, last line "EXIT=<code>") then deletes the request.
#
# ACTIONS (nothing else runs):
#   build      cmake --build build-dxr, incremental. Returns the errors and the log tail.
#              lines: -Config RelWithDebInfo|Debug|Release   (default RelWithDebInfo)
#              Needs build-dxr to be configured already: run auto-setup-windows-vr.cmd once by hand.
#   packiwad   python tools\rothdiff\packiwad.py  (rebuilds vrealms.pk3 beside the exe)
#   selftest   src\roth\build_selftest.cmd "<retail folder>"   (engine-free reader checks)
#   dbtest     tools\dbase100_test\build.cmd "<retail folder>" (DBASE100 + inventory tests, dump)
#   run        doomxr.exe -iwad vrealms.pk3 -rothpath "<retail folder>" -config <bridge ini> +map <map>
#              lines: -Map <name> (default STUDY1) | -Exec <cfg under this repo> | -TimeoutSec N (default 60,
#                     max 900) | -Config RelWithDebInfo|Debug|Release | -Foreground
#              The game is killed at the timeout if it has not quit by itself. Returns the engine log
#              tail. Starts minimised unless -Foreground: a minimised window keeps ticking but does not
#              present, so screenshots need -Foreground (HANDOFF_SESSION_2026-10-09.md section 9).
#   git        git in this repo.
#              verbs: status diff log show branch switch checkout add commit rev-parse ls-files fetch pull stash
#              pull is allowed only as "pull --ff-only". refused: push, reset, clean, rm, rebase, merge;
#              and the exact arguments --force -f -D --hard --delete -d --orphan --amend
#   readfile   prints a text file under this repo (not .git) or the DoomXR logs folder
#              lines: <path> | -Tail N
#
# THE RETAIL FOLDER is read from rothpath.txt beside this script (one line, no quotes). It must be
# the folder holding ROTH.RES and DBASE100.DAT -- for Steam, "...\Realms of the Haunting\DATA".
#
# THE ENGINE CONFIG is never the shared doomxr.ini. REMAROTH and UZDXREMA both call themselves
# "DoomXR" and would share one ini, so an archived cvar set here would follow the owner into the
# hands build. Runs use tools\rothdiff\captures\capture.ini when it exists, else bridge.ini beside
# this script.

param([string]$Once = '')

$ErrorActionPreference = 'Continue'
$env:GIT_TERMINAL_PROMPT = '0'   # a git that wants a password fails instead of hanging the bridge
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = [IO.Path]::GetFullPath((Join-Path $root '..\..'))
# A copy kept OUTSIDE the repo (so starting it does not depend on which branch is checked out)
# names the repo in repopath.txt beside it.
$repoFile = Join-Path $root 'repopath.txt'
if (Test-Path -LiteralPath $repoFile) {
	$rp0 = (Get-Content -LiteralPath $repoFile -TotalCount 1).Trim().Trim('"')
	if ($rp0 -and (Test-Path -LiteralPath (Join-Path $rp0 '.git'))) { $repo = [IO.Path]::GetFullPath($rp0) }
}
$req  = Join-Path $root 'requests'
$res  = Join-Path $root 'responses'
$logs = Join-Path $root 'logs'
New-Item -ItemType Directory -Force $req, $res, $logs | Out-Null

$build    = Join-Path $repo 'build-dxr'
$configs  = @('RelWithDebInfo', 'Debug', 'Release')
$gitVerbs = @('status','diff','log','show','branch','switch','checkout','add','commit','rev-parse','ls-files','fetch','pull','stash')
$badArgs  = @('--force','-f','-D','--hard','--delete','-d','--orphan','--amend')

function Under-Repo($p) {
	try { $full = [IO.Path]::GetFullPath($p) } catch { return $false }
	return ($full -like "$repo\*") -and -not ($full -like "$repo\.git\*")
}

function Get-RothPath {
	$f = Join-Path $root 'rothpath.txt'
	if (-not (Test-Path -LiteralPath $f)) { return $null }
	$p = (Get-Content -LiteralPath $f -TotalCount 1).Trim().Trim('"')
	if ($p -and (Test-Path -LiteralPath (Join-Path $p 'ROTH.RES'))) { return $p }
	return $null
}

function Find-CMake {
	$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
	if (Test-Path $vswhere) {
		$vs = & $vswhere -latest -products * -property installationPath
		if ($vs) {
			$c = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
			if (Test-Path $c) { return $c }
		}
	}
	$cmd = Get-Command cmake.exe -ErrorAction SilentlyContinue
	if ($cmd) { return $cmd.Source }
	return $null
}

function Get-EngineIni {
	$capture = Join-Path $repo 'tools\rothdiff\captures\capture.ini'
	if (Test-Path -LiteralPath $capture) { return $capture }
	return (Join-Path $root 'bridge.ini')
}

function Run-Action($lines) {
	$action = $lines[0].Trim()
	$args_  = @($lines | Select-Object -Skip 1 | ForEach-Object { $_.TrimEnd() } | Where-Object { $_ -ne '' })
	switch -regex ($action) {
		'^build$' {
			$cfg = 'RelWithDebInfo'
			foreach ($a in $args_) {
				if ($a -match '^-Config\s+(\S+)$' -and $configs -contains $Matches[1]) { $cfg = $Matches[1] }
				else { "refused build argument: $a"; return 3 }
			}
			if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
				"build-dxr is not configured. Run auto-setup-windows-vr.cmd once by hand, then ask again."; return 4
			}
			$cmake = Find-CMake
			if (-not $cmake) { "cmake not found (looked in Visual Studio and on PATH)"; return 4 }
			$log = Join-Path $logs 'build.log'
			$start = Get-Date
			& $cmake --build $build --config $cfg --parallel 2>&1 | ForEach-Object { "$_" } | Out-File -LiteralPath $log -Encoding UTF8
			$code = $LASTEXITCODE
			"build ${cfg}: exit=$code, " + [int]((Get-Date) - $start).TotalSeconds + "s, log $log"
			$errs = @(Select-String -LiteralPath $log -Pattern '(: (fatal )?error |LINK : |error LNK|error MSB)' | Select-Object -First 60 | ForEach-Object { $_.Line })
			if ($errs.Count -gt 0) { "--- errors (first 60) ---"; $errs -join "`r`n" }
			"--- log tail ---"
			Get-Content -LiteralPath $log -Tail 40 | Out-String
			return $code
		}
		'^packiwad$' {
			$script = Join-Path $repo 'tools\rothdiff\packiwad.py'
			if (-not (Test-Path -LiteralPath $script)) { "missing: $script"; return 3 }
			Push-Location $repo
			& python $script 2>&1 | ForEach-Object { "$_" } | Out-String | Write-Output
			$code = $LASTEXITCODE
			Pop-Location
			return $code
		}
		'^(selftest|dbtest)$' {
			$roth = Get-RothPath
			if (-not $roth) { "rothpath.txt is missing, or the folder it names has no ROTH.RES"; return 4 }
			if ($action -eq 'selftest') { $cmdFile = Join-Path $repo 'src\roth\build_selftest.cmd' }
			else { $cmdFile = Join-Path $repo 'tools\dbase100_test\build.cmd' }
			if (-not (Test-Path -LiteralPath $cmdFile)) { "missing: $cmdFile"; return 3 }
			Push-Location $repo
			& cmd /c "`"$cmdFile`" `"$roth`"" 2>&1 | ForEach-Object { "$_" } | Out-String | Write-Output
			$code = $LASTEXITCODE
			Pop-Location
			return $code
		}
		'^run$' {
			$cfg = 'RelWithDebInfo'; $map = 'STUDY1'; $exec = $null; $timeout = 60; $fg = $false
			foreach ($a in $args_) {
				if ($a -match '^-Config\s+(\S+)$' -and $configs -contains $Matches[1]) { $cfg = $Matches[1] }
				elseif ($a -match '^-Map\s+([A-Za-z0-9_]+)$') { $map = $Matches[1] }
				elseif ($a -match '^-TimeoutSec\s+(\d+)$') { $timeout = [Math]::Min([int]$Matches[1], 900) }
				elseif ($a -match '^-Foreground$') { $fg = $true }
				elseif ($a -match '^-Exec\s+(.+)$') {
					$e = $Matches[1]
					if (-not [IO.Path]::IsPathRooted($e)) { $e = Join-Path $repo $e }
					if (-not (Under-Repo $e) -or -not (Test-Path -LiteralPath $e)) { "refused or missing cfg: $($Matches[1])"; return 3 }
					$exec = [IO.Path]::GetFullPath($e)
				}
				else { "refused run argument: $a"; return 3 }
			}
			$roth = Get-RothPath
			if (-not $roth) { "rothpath.txt is missing, or the folder it names has no ROTH.RES"; return 4 }
			$exeDir = Join-Path $build $cfg
			$exe = Join-Path $exeDir 'doomxr.exe'
			if (-not (Test-Path -LiteralPath $exe)) { "not built: $exe"; return 4 }
			if (-not (Test-Path -LiteralPath (Join-Path $exeDir 'vrealms.pk3'))) { "vrealms.pk3 is not beside the exe: ask for packiwad first"; return 4 }
			$ini = Get-EngineIni
			$log = Join-Path $logs 'run.log'
			Remove-Item -LiteralPath $log -Force -ErrorAction SilentlyContinue
			$al = "-iwad vrealms.pk3 -rothpath `"$roth`" -config `"$ini`" +logfile `"$log`" +map $map"
			if ($exec) { $al += " +exec `"" + ($exec -replace '\\', '/') + "`"" }
			"doomxr.exe $al"
			$style = 'Minimized'; if ($fg) { $style = 'Normal' }
			$p = Start-Process -FilePath $exe -ArgumentList $al -WorkingDirectory $exeDir -WindowStyle $style -PassThru
			$exited = $p.WaitForExit($timeout * 1000)
			if ($exited) { "exited by itself, code " + $p.ExitCode; $code = $p.ExitCode }
			else { try { $p.Kill() } catch {}; "still running at ${timeout}s: killed"; $code = 0 }
			if (Test-Path -LiteralPath $log) {
				"--- engine log tail ($log) ---"
				Get-Content -LiteralPath $log -Tail 200 | Out-String
			} else { "no engine log was written: the engine stopped before the console was up" ; if ($code -eq 0) { $code = 5 } }
			$reports = @(Get-ChildItem -LiteralPath $exeDir -Filter '*.txt' -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt (Get-Date).AddSeconds(-($timeout + 30)) })
			if ($reports.Count -gt 0) { "--- files written beside the exe this run ---"; ($reports | ForEach-Object { $_.FullName }) -join "`r`n" }
			return $code
		}
		'^git$' {
			if ($args_.Count -lt 1 -or $gitVerbs -notcontains $args_[0]) { "refused git verb: $($args_[0])"; return 3 }
			foreach ($a in $args_) { if ($badArgs -ccontains $a) { "refused git argument: $a"; return 3 } }
			if ($args_[0] -eq 'pull' -and ($args_ -notcontains '--ff-only')) { "pull is allowed only with --ff-only"; return 3 }
			Push-Location $repo
			& git @args_ 2>&1 | ForEach-Object { "$_" } | Out-String | Write-Output
			$code = $LASTEXITCODE
			Pop-Location
			return $code
		}
		'^readfile$' {
			if ($args_.Count -lt 1) { "readfile needs a path"; return 3 }
			$path = $args_[0]; $tail = 0
			foreach ($a in @($args_ | Select-Object -Skip 1)) {
				if ($a -match '^-Tail\s+(\d+)$') { $tail = [int]$Matches[1] } else { "refused readfile argument: $a"; return 3 }
			}
			if (-not [IO.Path]::IsPathRooted($path)) { $path = Join-Path $repo $path }
			$ok = (Under-Repo $path) -or $path -like "$env:USERPROFILE\Documents\My Games\DoomXR\*"
			if (-not $ok -or -not (Test-Path -LiteralPath $path)) { "refused or missing: $path"; return 3 }
			if ($tail -gt 0) { Get-Content -LiteralPath $path -Tail $tail | Out-String } else { Get-Content -LiteralPath $path -Raw }
			return 0
		}
		default { "refused action: $action"; return 3 }
	}
}

function Handle-Request($file) {
	$id = [IO.Path]::GetFileNameWithoutExtension($file)
	$lines = @(Get-Content -LiteralPath $file)
	Remove-Item -LiteralPath $file -Force
	if ($lines.Count -lt 1) { $lines = @('') }
	Write-Host ("[{0}] {1}: {2}" -f (Get-Date -Format 'HH:mm:ss'), $id, $lines[0])
	$out = Join-Path $res "$id.out"
	$tmp = "$out.partial"
	try {
		$result = @(Run-Action $lines)
		$code = $result[-1]
		$text = ($result | Select-Object -SkipLast 1) -join "`r`n"
	} catch { $text = $_ | Out-String; $code = 99 }
	Set-Content -LiteralPath $tmp -Value ($text + "`r`nEXIT=$code") -Encoding UTF8
	Move-Item -LiteralPath $tmp -Destination $out -Force
	Write-Host ("[{0}] {1}: EXIT={2}" -f (Get-Date -Format 'HH:mm:ss'), $id, $code)
}

# -Once <request file>: handle that one request and exit. The watching window below starts a
# fresh copy of THIS FILE for every request, so a fix to an action takes effect on the next
# request without restarting the window. The list of actions is still only what this file says.
if ($Once -ne '') {
	if (Test-Path -LiteralPath $Once) { Handle-Request $Once }
	exit 0
}

Write-Host "Claude bridge (REMAROTH) running."
Write-Host "  repo      $repo"
Write-Host "  requests  $req"
$rp = Get-RothPath
if ($rp) { Write-Host "  retail    $rp" } else { Write-Host "  retail    NOT SET -- put the folder holding ROTH.RES in rothpath.txt beside this script" -ForegroundColor Yellow }
Write-Host "Close this window to stop it."
while ($true) {
	Get-ChildItem $req -Filter '*.req' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | ForEach-Object {
		& powershell -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -Once $_.FullName
	}
	Start-Sleep -Seconds 2
}
