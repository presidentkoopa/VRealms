# CLAUDE BRIDGE v2 -- lets a Claude session request a FIXED set of actions on this PC.
# The owner starts it by double-clicking START_CLAUDE_BRIDGE.cmd; closing its window stops it.
#
# Claude writes  requests\<id>.req   (line 1 = action, following lines = arguments, one per line)
# The bridge writes responses\<id>.out (all output, last line "EXIT=<code>") then deletes the request.
#
# ACTIONS (nothing else runs):
#   build      E:\DOOMWork\_buildtemp\run_build.ps1, waits for it, returns build.done + the log tail
#   boottest   E:\DOOMWork\tools\main_boottest\main_boottest.ps1 (always windowed, muted, hidden desktop)
#              argument lines: -Tics N | -TimeoutSec N | -Map <map> | -ExtraArgs <text> | -Exec <path>
#                              -Exe <path under E:\DOOMWork> | -ExtraFiles <path under E:\DOOMWork> (repeatable)
#                              -Zdl <path under E:\DOOMWork>
#   git <repo> git in E:\DOOMWork\<repo>; repo is UZDXREMA, RS_Avatar or RS_ModelSwapper
#              verbs: status diff log show branch switch checkout add commit rev-parse ls-files
#              refused: push, reset, clean, rm, rebase, merge; and the exact arguments
#              --force -f -D --hard --delete -d --orphan --amend (case-sensitive: -F is allowed)
#   blender    Blender, headless: blender --background --factory-startup --python <script> -- <args>
#              the script must be in claude_bridge\scripts\ ; arguments follow on the next lines
#   python     python <script> <args>; the script must be under E:\DOOMWork (not _old)
#   readfile   prints a text file under E:\DOOMWork or the DoomXR logs folder

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$req  = Join-Path $root 'requests'
$res  = Join-Path $root 'responses'
$scripts = Join-Path $root 'scripts'
New-Item -ItemType Directory -Force $req, $res, $scripts | Out-Null

$git  = 'git'
$repos = @{ 'UZDXREMA' = 'E:\DOOMWork\UZDXREMA'; 'RS_Avatar' = 'E:\DOOMWork\RS_Avatar'; 'RS_ModelSwapper' = 'E:\DOOMWork\RS_ModelSwapper' }
$gitVerbs = @('status','diff','log','show','branch','switch','checkout','add','commit','rev-parse','ls-files')
$badArgs  = @('--force','-f','-D','--hard','--delete','-d','--orphan','--amend')

function Under-DoomWork($p) {
	try { $full = [IO.Path]::GetFullPath($p) } catch { return $false }
	return ($full -like 'E:\DOOMWork\*') -and -not ($full -like 'E:\DOOMWork\_old\*')
}

function Find-Blender {
	if ($env:BLENDER -and (Test-Path $env:BLENDER)) { return $env:BLENDER }
	$cands = @()
	foreach ($base in @('C:\Program Files\Blender Foundation', 'D:\Program Files\Blender Foundation', 'C:\Program Files (x86)\Steam\steamapps\common\Blender', 'D:\SteamLibrary\steamapps\common\Blender')) {
		if (Test-Path $base) { $cands += Get-ChildItem $base -Recurse -Filter blender.exe -ErrorAction SilentlyContinue }
	}
	$cmd = Get-Command blender.exe -ErrorAction SilentlyContinue
	if ($cmd) { return $cmd.Source }
	if ($cands.Count -gt 0) { return ($cands | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName }
	return $null
}

function Run-Action($lines) {
	$action = $lines[0].Trim()
	$args_  = @($lines | Select-Object -Skip 1 | ForEach-Object { $_.TrimEnd() } | Where-Object { $_ -ne '' })
	switch -regex ($action) {
		'^build$' {
			$start = Get-Date
			& powershell -NoProfile -ExecutionPolicy Bypass -File 'E:\DOOMWork\_buildtemp\run_build.ps1' 2>&1 | Out-String | Write-Output
			$done = 'E:\DOOMWork\_buildtemp\build.done'
			if ((Test-Path $done) -and ((Get-Item $done).LastWriteTime -gt $start)) {
				"build.done: " + (Get-Content $done -Raw)
				Get-Content 'E:\DOOMWork\_buildtemp\build.log' -Tail 60 | Out-String
				if ((Get-Content $done -Raw) -match 'exit=0') { return 0 } else { return 1 }
			}
			"build.done was not written"; return 2
		}
		'^boottest$' {
			$p = @{}; $extra = @()
			foreach ($a in $args_) {
				if ($a -match '^-(Tics|TimeoutSec)\s+(\d+)$') { $p[$Matches[1]] = [int]$Matches[2] }
				elseif ($a -match '^-(ExtraArgs|Exec|Map)\s+(.+)$') { $p[$Matches[1]] = $Matches[2] }
				elseif ($a -match '^-(Exe|Zdl)\s+(.+)$') {
					if (-not (Under-DoomWork $Matches[2])) { "refused path: $($Matches[2])"; return 3 }
					$p[$Matches[1]] = $Matches[2]
				}
				elseif ($a -match '^-ExtraFiles\s+(.+)$') {
					if (-not (Under-DoomWork $Matches[1])) { "refused path: $($Matches[1])"; return 3 }
					$extra += $Matches[1]
				}
				else { "refused boottest argument: $a"; return 3 }
			}
			if ($extra.Count -gt 0) { $p['ExtraFiles'] = [string[]]$extra }
			& 'E:\DOOMWork\tools\main_boottest\main_boottest.ps1' @p 2>&1 | Out-String | Write-Output
			return $LASTEXITCODE
		}
		'^git\s+(\S+)$' {
			$repo = $Matches[1]
			if (-not $repos.ContainsKey($repo)) { "refused repo: $repo"; return 3 }
			if ($args_.Count -lt 1 -or $gitVerbs -notcontains $args_[0]) { "refused git verb: $($args_[0])"; return 3 }
			foreach ($a in $args_) { if ($badArgs -ccontains $a) { "refused git argument: $a"; return 3 } }
			Push-Location $repos[$repo]
			& $git @args_ 2>&1 | Out-String | Write-Output
			$code = $LASTEXITCODE
			Pop-Location
			return $code
		}
		'^blender$' {
			if ($args_.Count -lt 1) { "blender needs a script name"; return 3 }
			$script = Join-Path $scripts $args_[0]
			if (-not (Test-Path -LiteralPath $script) -or ([IO.Path]::GetFullPath($script) -notlike "$scripts\*")) { "refused or missing script: $($args_[0])"; return 3 }
			$exe = Find-Blender
			if (-not $exe) { "Blender not found. Set the BLENDER environment variable to blender.exe and restart the bridge."; return 4 }
			"blender: $exe"
			$rest = @($args_ | Select-Object -Skip 1)
			& $exe --background --factory-startup --python $script -- @rest 2>&1 | Out-String | Write-Output
			return $LASTEXITCODE
		}
		'^python$' {
			if ($args_.Count -lt 1) { "python needs a script path"; return 3 }
			if (-not (Under-DoomWork $args_[0]) -or -not (Test-Path -LiteralPath $args_[0])) { "refused or missing script: $($args_[0])"; return 3 }
			$rest = @($args_ | Select-Object -Skip 1)
			& python $args_[0] @rest 2>&1 | Out-String | Write-Output
			return $LASTEXITCODE
		}
		'^readfile$' {
			$path = $args_[0]
			$ok = (Under-DoomWork $path) -or $path -like "$env:USERPROFILE\Documents\My Games\DoomXR\*"
			if (-not $ok -or -not (Test-Path -LiteralPath $path)) { "refused or missing: $path"; return 3 }
			Get-Content -LiteralPath $path -Raw; return 0
		}
		default { "refused action: $action"; return 3 }
	}
}

Write-Host "Claude bridge v2 running. Watching $req"
Write-Host "Close this window to stop it."
while ($true) {
	Get-ChildItem $req -Filter '*.req' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | ForEach-Object {
		$id = $_.BaseName
		$lines = @(Get-Content -LiteralPath $_.FullName)
		Remove-Item -LiteralPath $_.FullName -Force
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
	Start-Sleep -Seconds 2
}
