# Hidden BOOT TEST of the MAIN engine with the owner's desktop ZDL load order (owner 2026-09-15: "ensure engine boots with load order
# in desktop zdl file"). Starts MAP01 on Vulkan, sets off a barrel so the effect paths draw, and stops after the "BOOT: ok" marker.
# Rules: a scratch COPY of the owner's ini (vr_mode 0, windowed, sound muted); scratch working dir and -savedir; the process runs on
# its own Win32 desktop, which is never shown; killed on the marker, a timeout, a Fatal Error window or fatal text.
# Guards: the owner's ini, the owner's logs, the logs beside the exe and Saved Games\DoomXR must not change.
[CmdletBinding()]
# -Map defaults to e1m1: the owner's IWAD is Ultimate Doom, and a run that asks for map01 never enters
# a level, exits cleanly and reads like a pass. A boot test that silently tests nothing is the worst
# kind (caught by the VR Body lane, 2026-09-18), so the report below also fails a run that never
# reached a level.
param([int] $TimeoutSec = 150, [string] $Zdl = (Join-Path $env:USERPROFILE 'Desktop\test.zdl'), [string] $Map = 'e1m1', [string[]] $ExtraFiles = @(), [string] $Exe = '', [string] $Gun = '', [string] $Pickup = '', [int] $Fire = 0, [int] $Restart = 0, [string] $Exec = '', [string] $NetEvent = '', [int] $Kill = 0, [int] $Tics = 0, [int] $Throw = 0, [string] $ThrowClass = '', [double] $ThrowSpeed = 0, [double] $ThrowPitch = -999, [int] $ThrowHands = 0, [int] $ThrowTrace = 0, [string] $ThrowTarget = '', [double] $ThrowDist = 0, [int] $ThrowDebug = 0, [string] $ExtraArgs = '', [string] $Iwad = '', [string] $ScriptErrorOk = '')
$ErrorActionPreference = 'Stop'

$eng  = 'E:\DOOMWork\UZDXREMA\build-dxr\RelWithDebInfo'
$exe  = Join-Path $eng 'doomxr.exe'
if ($Exe) { $exe = $Exe }   # -Exe: test another build's exe (a backup, or a bisect step); its own pk3s must sit beside it
$iwad = Join-Path $eng 'doom.wad'
# -Iwad: a mod built for a different IWAD. Punch Out Doom is Doom 2 content, and against
# doom.wad it logs hundreds of "Unknown patch" lines for textures that are simply not in
# the IWAD -- noise that buries whatever the run was meant to show. Default unchanged.
if ($Iwad) { $iwad = $Iwad }
$ini  = Join-Path $env:USERPROFILE 'Documents\My Games\DoomXR\doomxr.ini'
$ownerLogDir = Join-Path $env:USERPROFILE 'Documents\My Games\DoomXR'
$ownerSaves = Join-Path $env:USERPROFILE 'Saved Games\DoomXR'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'   # unique per run; names the scratch folder AND the log
# PER-RUN SCRATCH. It used to be one fixed folder with one fixed boottest.pk3, so two lanes
# running at once collided: the second died at "Cannot remove boottest.pk3 -- being used by
# another process", which reads as a broken harness rather than as a concurrent run.
# $stamp is already unique per run, so the whole working set is now private to it.
$scratchRoot = Join-Path $env:TEMP 'doomxr_boottest'
$scratch = Join-Path $scratchRoot "run_$stamp"
$saveDir = Join-Path $scratch 'saves'
New-Item -ItemType Directory -Force $scratch, $saveDir | Out-Null

# --- load order: the ZDL file's file0..fileN, in order
$files = @()
$zdlLines = Get-Content $Zdl
$pairs = foreach ($l in $zdlLines) { if ($l -match '^file(\d+)=(.+)$') { [pscustomobject]@{ I = [int]$Matches[1]; P = $Matches[2].Trim() } } }
$files = @($pairs | Sort-Object I | ForEach-Object { $_.P })
# An EMPTY load order is legal and useful: it is how the build lane isolates the engine from the mods.
if ($files.Count -eq 0) { Write-Output "load order is EMPTY -- engine and the test pk3 only" }
if ($ExtraFiles.Count -gt 0) { $files = @($files) + @($ExtraFiles) }   # loaded after the ZDL's own files, e.g. RS_Modern
# -LiteralPath: the owner's load order has filenames with brackets (mk-champions[20210309].pk3), and
# Test-Path treats those as wildcards, so the file "did not exist" and the test refused to start.
foreach ($f in $files) { if (-not (Test-Path -LiteralPath $f)) { throw "load order file missing: $f" } }
Write-Output ("load order from {0}: {1} file(s)" -f $Zdl, $files.Count)
$files | ForEach-Object { Write-Output "  $_" }

# --- scratch config: copy, vr_mode 0, windowed, muted
$cfg = Join-Path $scratch 'boot.ini'
Copy-Item $ini $cfg -Force
$t = [IO.File]::ReadAllText($cfg)
foreach ($kv in @(@('vr_mode', '0'), @('vid_fullscreen', 'false'), @('i_pauseinbackground', 'false'), @('snd_mastervolume', '0'), @('snd_musicvolume', '0'))) {
    if ($t -match "(?m)^$($kv[0])=") { $t = $t -replace "(?m)^$($kv[0])=.*$", "$($kv[0])=$($kv[1])" }
    else { $t = $t -replace '(?m)^\[GlobalSettings\]\s*$', "[GlobalSettings]`r`n$($kv[0])=$($kv[1])" }
}
[IO.File]::WriteAllText($cfg, $t, (New-Object System.Text.UTF8Encoding($false)))
if (-not ([IO.File]::ReadAllText($cfg) -match '(?m)^vr_mode=0\s*$')) { throw "scratch ini does not have vr_mode=0 -- refusing to start" }

# --- test package
$pk3 = Join-Path $scratch 'boottest.pk3'
if (Test-Path $pk3) { Remove-Item -LiteralPath $pk3 -Force -Confirm:$false }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory((Join-Path $here 'pk3'), $pk3)

# --- guards
function Snap($path) { if (Test-Path $path) { (Get-ChildItem $path -Recurse -Force -File -ErrorAction SilentlyContinue | Measure-Object -Property LastWriteTimeUtc -Maximum).Maximum } }
$guardFiles = @($ini, (Join-Path $ownerLogDir 'log-debug.txt'), (Join-Path $ownerLogDir 'doomxr-log.txt'), (Join-Path $eng 'log-debug.txt'), (Join-Path $eng 'doomxr-log.txt')) | Where-Object { Test-Path $_ }
Write-Output ('guarding: ' + ($guardFiles -join '; '))
$before = @{}; foreach ($g in $guardFiles) { $before[$g] = (Get-Item $g).LastWriteTimeUtc }
$savesBefore = Snap $ownerSaves

# RS_PROFILES IS THE OWNER'S, AND A BOOT TEST WAS EATING IT.
#
# level.JSONProfileSave writes to the owner s Documents rs_profiles folder REGARDLESS of
# -savedir, so every run overwrote his bodydiag.json -- the body report he had just been
# told to switch on, destroyed by the next test anyone ran. Caught 2026-09-20 after it
# clobbered a real VR session's report.
#
# Copied aside before the run and put back after, so a test can write whatever it likes
# and his evidence survives. Restored on every exit path, including a timeout kill.
# Forward slashes on purpose: PowerShell takes them, and a backslash before "rs_profiles"
# is a carriage return to anything that unescapes on the way in. It ate the separator once.
$ownerProfiles = Join-Path $env:USERPROFILE 'Documents/My Games/DoomXR/rs_profiles'
$profileStash  = Join-Path $scratch 'rs_profiles_stash'
if (Test-Path $ownerProfiles) {
    if (Test-Path $profileStash) { Remove-Item -Recurse -Force $profileStash -Confirm:$false }
    Copy-Item -Recurse -Force $ownerProfiles $profileStash
    Write-Output "guarding the owner's rs_profiles (stashed)"
}

# --- arguments
$out = Join-Path $scratch ("boot_{0}.txt" -f $stamp)
$fileArgs = (@($files) + @($pk3) | ForEach-Object { '"' + $_ + '"' }) -join ' '
$argLine = "-iwad `"$iwad`" -config `"$cfg`" -file $fileArgs -savedir `"$saveDir`" -stdout +vid_preferbackend 1 +map $Map"
# -Gun <class>: give and select that weapon at tic 200, so a CARD'S PARTS are exercised and not
# just parsed. A clean compile proves a card is LEGAL; only a gun actually held proves its
# surfaces RESOLVE. Inert when unset -- the handler does nothing and every other run is unchanged.
if ($Gun) { $argLine += " +set bootgun $Gun"; Write-Output "gun: will give and select $Gun at tic 200" }
# -Pickup <class>: spawn that pickup and print what it actually hands over. For mechanisms
# that are INVISIBLE WHEN THEY WORK -- a set bridge swapping one weapon class for another at
# spawn, where a clean parse and a green boot prove nothing.
if ($Pickup) { $argLine += " +set bootpickup $Pickup"; Write-Output "pickup: will spawn $Pickup at tic 250 and report what it gave" }
# -Fire <tics>: hold the trigger from tic 260, after -Gun has put a weapon in the hand. A gun that is
# HELD proves its card resolves; only a gun that FIRES proves the flash, the casing and the round in
# flight resolve, and those are missing in exactly the same way they are fine. Inert unless asked.
if ($Fire -gt 0) { $argLine += " +set bootfire $Fire"; Write-Output "fire: will hold the trigger for $Fire tics from tic 260" }
# -Restart: change level at tic N, to hunt the long-parked crash on level change / new game. The
# handler prints a heartbeat either side, because a hang and a crash look identical from out here --
# the process simply stops. Inert at 0, so every other lane's run is byte-identical.
if ($Restart -gt 0) { $argLine += " +set bootrestart $Restart"; Write-Output "restart: will change level at tic $Restart" }
# -Exec: run a console script. The point of it is the MENU's New Game, which is
# G_DeferedInitNew -- the same call the `map` CCMD makes, and NOT a level change. A cfg of
# "wait 200; map e1m1" reaches it without needing a keypress, which a hidden desktop cannot
# send. Inert when unset.
if ($Exec) { $argLine += " +exec `"$Exec`""; Write-Output "exec: $Exec" }
# -NetEvent "<name> [a0 [a1 [a2]]]": send that network event at tic 230, from inside the level
# -- after -Gun has put a weapon in the hand and before -Fire pulls the trigger. The ONLY way
# to exercise a netevent offline: +netevent and -exec both run before a level exists, so a
# handler that reads the player prints nothing, which looks the same as one that is broken.
#
# THE VALUE IS QUOTED, AND UNTIL 2026-10-10 IT WAS NOT. `+set bootnetevent spo_bout 0 1 4`
# sets the cvar to "spo_bout" and leaves `0 1 4` as three further console arguments, which go
# nowhere -- so a command whose whole point is its numbers fired with none of them, and
# nothing said so. The pk3 side now splits the string and passes up to three ints; this side
# has to deliver it as ONE value for that to be possible. A bare name is unaffected.
if ($NetEvent) { $argLine += " +set bootnetevent `"$NetEvent`""; Write-Output "netevent: will send $NetEvent at tic 230" }
# -Kill: kill the player at tic N and press USE to respawn. Inert at 0.
if ($Kill -gt 0) { $argLine += " +set bootkill $Kill"; Write-Output "kill: will kill the player at tic $Kill" }
# -Tics <n>: how long the run lasts. 0 keeps the historic 420 (12 s), which is right for what
# this test was built for -- start a level, set off a barrel, watch the effect paths draw.
# Anything with a clock of its own needs longer: a Super Punch-Out bout spends 105 tics on the
# intro and then waits for the bell, so 420 landed about 80 tics into round 1, before the
# opponent had thrown a punch. -TimeoutSec is NOT this: that is the wall-clock patience for
# the marker to appear, while this is the tic the marker is printed at.
if ($Tics -gt 0) { $argLine += " +set boottics $Tics"; Write-Output "tics: run ends at tic $Tics ($([int]($Tics / 35)) s of play)" }
# -Throw <tic>: run a scripted throw at that maptime -- spawn an object, take hold of it,
# release it with a velocity built from a speed and an angle, and report the range measured
# against the range the arithmetic expects. Everything after the fingers open is plain
# playsim, so this needs no headset and no controller: the only half it skips is the half
# that could not desync anyway. See RS_WorldHands zscript/hands/rs_throwtest.zs.
#
# +set AT STARTUP, NEVER AN EXEC AFTER THE MAP. Setting a SERVER STRING cvar from the
# console once a level is running takes the engine down between "E1M1 - Hangar" and the
# first tic: no fatal window, no error text, the log simply stops. Caught 2026-09-28 doing
# exactly that with rs_throwtest_class -- the int and float ones survive it, the string
# does not. Every bootX cvar above is set this way, which is why none of them ever hit it.
if ($Throw -gt 0) { $argLine += " +set rs_throwtest $Throw"; Write-Output "throw: scripted throw at tic $Throw" }
if ($ThrowClass)  { $argLine += " +set rs_throwtest_class $ThrowClass"; Write-Output "throw: object $ThrowClass" }
if ($ThrowSpeed -gt 0)    { $argLine += " +set rs_throwtest_speed $ThrowSpeed"; Write-Output "throw: hand at $ThrowSpeed m/s" }
if ($ThrowPitch -ne -999) { $argLine += " +set rs_throwtest_pitch $ThrowPitch"; Write-Output "throw: $ThrowPitch degrees up" }
if ($ThrowHands -gt 0)    { $argLine += " +set rs_throwtest_hands $ThrowHands"; Write-Output "throw: $ThrowHands hand(s)" }
if ($ThrowTrace -gt 0)    { $argLine += " +set rs_throwtest_trace 1"; Write-Output "throw: tracing every world step" }
if ($ThrowTarget)         { $argLine += " +set rs_throwtest_target $ThrowTarget"; Write-Output "throw: at a $ThrowTarget" }
if ($ThrowDist -gt 0)     { $argLine += " +set rs_throwtest_dist $ThrowDist"; Write-Output "throw: target $ThrowDist m away" }
if ($ThrowDebug -gt 0)    { $argLine += " +set rs_throw_debug 1"; Write-Output "throw: printing every impact" }
# -ExtraArgs: raw text appended to the command line, for an A/B that turns one cvar over and
# changes nothing else. Added for the texture-thread A/B (CODER_PLAN step 9), where the whole
# point is that the two arms differ by exactly one token. Empty by default, so every existing
# caller's command line is byte-identical to what it was.
if ($ExtraArgs) { $argLine += " $ExtraArgs"; Write-Output "extra args: $ExtraArgs" }

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class BootDesktop {
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
  struct STARTUPINFO { public int cb; public string lpReserved; public string lpDesktop; public string lpTitle; public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags; public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError; }
  [StructLayout(LayoutKind.Sequential)] struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
  [StructLayout(LayoutKind.Sequential)] struct SECURITY_ATTRIBUTES { public int nLength; public IntPtr lpSecurityDescriptor; public bool bInheritHandle; }
  [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr CreateDesktop(string name, IntPtr dev, IntPtr mode, int flags, uint access, IntPtr sa);
  [DllImport("user32.dll", SetLastError=true)] static extern bool CloseDesktop(IntPtr h);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool CreateProcess(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags, IntPtr env, string cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr CreateFile(string name, uint access, uint share, ref SECURITY_ATTRIBUTES sa, uint disp, uint flags, IntPtr tmpl);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumDesktopWindows(IntPtr desk, EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  public static bool HasFatalWindow() {
    if (Desktop == IntPtr.Zero) return false;
    bool found = false;
    EnumDesktopWindows(Desktop, (h, l) => { var sb = new StringBuilder(512); GetWindowText(h, sb, 512); if (sb.ToString().StartsWith("Fatal Error")) found = true; return !found; }, IntPtr.Zero);
    return found;
  }
  // EVERY titled window on the hidden desktop, for the REPORT only -- never to decide a verdict.
  //
  // CODER_PLAN step 66 asks for "any top-level window" to be treated as a crash dialog. It is
  // deliberately NOT done that way: the engine's OWN main window is a top-level window on this
  // desktop, so "any window is fatal" would fail every healthy run -- and this harness has
  // already cost three lanes their runs with exactly that shape of over-broad match (see the
  // kill-by-name comment further down, which did the same thing to processes).
  //
  // A dialog nobody can see is a DIAGNOSTIC problem, not a detection problem. The run already
  // goes RED on the timeout; what was missing was any way to learn WHY. So the titles are
  // printed when a run fails, and nothing is judged by them.
  public static string WindowTitles() {
    if (Desktop == IntPtr.Zero) return "";
    var all = new StringBuilder();
    EnumDesktopWindows(Desktop, (h, l) => {
      var sb = new StringBuilder(512); GetWindowText(h, sb, 512);
      string t = sb.ToString().Trim();
      if (t.Length > 0) { if (all.Length > 0) all.Append("  |  "); all.Append(t); }
      return true;
    }, IntPtr.Zero);
    return all.ToString();
  }
  public static IntPtr Desktop = IntPtr.Zero;
  public static int Start(string exe, string args, string cwd, string outFile, string desktopName) {
    Desktop = CreateDesktop(desktopName, IntPtr.Zero, IntPtr.Zero, 0, 0x10000000, IntPtr.Zero);
    if (Desktop == IntPtr.Zero) throw new Exception("CreateDesktop failed " + Marshal.GetLastWin32Error());
    var sa = new SECURITY_ATTRIBUTES(); sa.nLength = Marshal.SizeOf(sa); sa.bInheritHandle = true;
    IntPtr hOut = CreateFile(outFile, 0x40000000, 1, ref sa, 2, 0x80, IntPtr.Zero);
    var si = new STARTUPINFO(); si.cb = Marshal.SizeOf(si); si.lpDesktop = desktopName;
    si.dwFlags = 0x100; si.hStdOutput = hOut; si.hStdError = hOut;
    PROCESS_INFORMATION pi;
    bool ok = CreateProcess(exe, new StringBuilder("\"" + exe + "\" " + args), IntPtr.Zero, IntPtr.Zero, true, 0, IntPtr.Zero, cwd, ref si, out pi);
    int err = Marshal.GetLastWin32Error();
    CloseHandle(hOut);
    if (!ok) { CloseDesktop(Desktop); Desktop = IntPtr.Zero; throw new Exception("CreateProcess failed " + err); }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return pi.dwProcessId;
  }
  public static void Close() { if (Desktop != IntPtr.Zero) { CloseDesktop(Desktop); Desktop = IntPtr.Zero; } }
}
'@
# ONE ENGINE AT A TIME ON THIS MACHINE.
#
# The per-run scratch folder above fixed lanes colliding on FILES. It did not fix them colliding
# on DEVICES, and that is the one that keeps costing us. Two doomxr.exe starting within a second
# of each other both reach for the OpenXR runtime, the audio device and the GPU, and the loser
# DIES TWO SECONDS IN -- the log just stops after "I_InitSound" or "I_StartupOpenXR", with no
# fatal error, no window and no exit message. From the report it is indistinguishable from the
# tested mod failing to boot, so the lane that lost the race writes up a RED against its own work.
# That is exactly the shape that produced one confident wrong conclusion about RS_VR_Weapons.
#
# So runs QUEUE instead of racing. The mutex is machine-wide (Global\) because the lanes are
# separate processes, and it is held across the whole launch-and-wait, not just the start: the
# devices stay claimed for as long as the engine is alive.
#
# AN ABANDONED MUTEX IS A SUCCESSFUL ACQUISITION, not an error -- it means the previous holder
# died without releasing, which is precisely when the next run most needs to go.
# Global\ spans sessions and is the right scope; creating one needs a privilege a plain user
# account may not hold, so fall back to this session's own namespace rather than fail the run.
try   { $runLock = New-Object System.Threading.Mutex($false, 'Global\doomxr_boottest_engine') }
catch { $runLock = New-Object System.Threading.Mutex($false, 'Local\doomxr_boottest_engine') }
$haveLock = $false
try { $haveLock = $runLock.WaitOne([TimeSpan]::FromSeconds($TimeoutSec + 120)) }
catch [System.Threading.AbandonedMutexException] { $haveLock = $true; Write-Output 'lock: taken over from a run that died holding it' }
if (-not $haveLock) { throw "another lane's boot test has held the engine lock for over $($TimeoutSec + 120) s -- refusing to race it" }
Write-Output 'lock: held (one engine at a time on this machine)'

Write-Output ("exe {0}  ->  {1}" -f (Get-Item $exe).LastWriteTime.ToString('MM-dd HH:mm'), $out)
$runStarted = Get-Date   # so the report names only crash folders THIS run wrote, not September's
$procId = [BootDesktop]::Start($exe, $argLine, $scratch, $out, "doomxrboot_$stamp")
$p = Get-Process -Id $procId
$sw = [Diagnostics.Stopwatch]::StartNew()
$killed = $false; $reason = ''
while (-not $p.HasExited) {
    if ($sw.Elapsed.TotalSeconds -gt $TimeoutSec) { $reason = "TIMEOUT $TimeoutSec s"; break }
    if ([BootDesktop]::HasFatalWindow()) { Start-Sleep -Milliseconds 800; $reason = 'FATAL ERROR WINDOW'; break }
    if (Test-Path $out) {
        # "Script error" alone is NOT fatal. The owner's other mods print dozens of harmless GLDEFS
        # warnings that begin `Script error, "...:gldefs" line N:` and carry `Warning:` ON THE NEXT
        # LINE, which killed every full-order run a few seconds in. So a Script error counts only when
        # the line after it is not a Warning. The genuinely fatal patterns stand on their own.
        #
        # -ScriptErrorOk <regex> names a SECOND second-line pattern that is also not fatal, for a
        # loaded wad whose own data is older than the engine. Punch Out Doom's MAPINFO uses the
        # ZDoom-era episode key `MustConfirm`, which GZDoom dropped: the parser prints
        # "Unknown property 'MustConfirm' found in episode definition", skips it and carries on --
        # but it is not a `Warning:` line, so it read as fatal and killed the run two seconds in,
        # before ZScript had even compiled. Default '' leaves the test exactly as it was.
        $txt = Get-Content $out -Raw -ErrorAction SilentlyContinue
        $seOk = if ($ScriptErrorOk) { "Warning:|$ScriptErrorOk" } else { 'Warning:' }
        if ($txt -and (($txt -match 'Execution could not continue|VM execution aborted|I_FatalError|Unhandled exception|Very Fatal Error') -or
                       ($txt -match "Script error[^\r\n]*\r?\n(?!\s*(?:$seOk))"))) { Start-Sleep -Milliseconds 800; $reason = 'FATAL TEXT in output'; break }
        if (Select-String -Path $out -Pattern 'BOOT: ok' -Quiet) { Start-Sleep -Milliseconds 1500; $reason = 'boot marker reached'; break }
    }
    Start-Sleep -Milliseconds 500
}
# WHAT WAS ON SCREEN AND WHAT THE PROCESS WAS DOING, captured HERE because both are gone in a
# moment: Close() destroys the desktop and its windows with it, and a killed process stops
# answering for its CPU time. Diagnostics only -- nothing below changes the verdict.
#
# This is the gap CODER_PLAN step 66 is about, and it cost two rounds on 2026-10-02: a pack whose
# MAPINFO named an event handler that did not exist took the engine down the instant the level
# started, and the report said only that the BOOT marker was not reached. No window, no CPU, no
# exit code -- a fatal exit and an infinite loop read identically. They do not any more.
$winTitles = ''
$cpuAtEnd = ''
try { $winTitles = [BootDesktop]::WindowTitles() } catch {}
try { if (-not $p.HasExited) { $cpuAtEnd = '{0:N1} s CPU, {1:N0} MB' -f $p.TotalProcessorTime.TotalSeconds, ($p.WorkingSet64 / 1MB) } } catch {}
if (-not $p.HasExited) { Stop-Process -Id $procId -Force -Confirm:$false -ErrorAction SilentlyContinue; $killed = $true }
$p.WaitForExit(5000) | Out-Null
[BootDesktop]::Close()
$sw.Stop()
# THE DEVICES ARE FREE NOW -- the engine has exited and been waited on. Released here rather than
# at the end of the script so the next lane starts while this one is still writing its report.
if ($haveLock) { $runLock.ReleaseMutex(); $haveLock = $false; Write-Output 'lock: released' }
# A RED RUN USED TO LEAK THE ENGINE PROCESS, and the cost was paid by the NEXT run: a live
# doomxr.exe kept holding its pk3, so the following run died on a locked file and looked like a
# harness fault instead of the previous run's corpse.
#
# THE FIRST FIX FOR THAT WAS WORSE THAN THE BUG. It swept every doomxr.exe whose command line
# mentioned the shared scratch folder -- which matched EVERY LANE'S RUN, not this one's. So a
# lane finishing killed another lane's run mid-level, and the victim reported
# "RED: the level never reached the BOOT marker" as though its own mod were broken. Three lanes
# lost runs to it, and it produced at least one confident wrong conclusion about which package
# was at fault.
#
# SO: KILL ONLY THE PROCESS THIS RUN STARTED. The harness launches it and holds the pid; there
# is no reason to go looking by name. A sweep that can reach another session is a sweep that
# will, and a false RED costs far more than a leaked corpse.
if (-not $p.HasExited) {
	Stop-Process -Id $procId -Force -Confirm:$false -ErrorAction SilentlyContinue
	Start-Sleep -Milliseconds 500
}
# Say so if OUR process outlived the kill -- it holds this run's scratch folder open.
$mine = Get-Process -Id $procId -ErrorAction SilentlyContinue
if ($mine) { Write-Warning ("LEAKED: this run's doomxr.exe (pid {0}) is still alive after the kill." -f $procId) }

# --- report
foreach ($g in $guardFiles) { if ((Get-Item $g).LastWriteTimeUtc -ne $before[$g]) { Write-Warning "GUARD: $g CHANGED -- report this" } }
if ((Snap $ownerSaves) -ne $savesBefore) { Write-Warning "GUARD: $ownerSaves CHANGED -- report this" }
if (Test-Path $profileStash) {
    Copy-Item -Recurse -Force (Join-Path $profileStash '*') $ownerProfiles
    Write-Output "restored the owner's rs_profiles"
}
Write-Output ("{0:N0} s, killed {1}, {2}, exit {3}" -f $sw.Elapsed.TotalSeconds, $killed, $reason, $(if ($killed) { 'n/a' } else { $p.ExitCode }))
$L = @(Get-Content $out)
# A run that never entered a level proves nothing, and without this it reads like a pass.
$noMap = @($L | Where-Object { $_ -match "No map $Map|Can't find map $Map" }).Count -gt 0
$reachedMarker = @($L | Where-Object { $_ -match 'BOOT: ok' }).Count -gt 0
if ($noMap) { Write-Warning "RED: the map '$Map' does not exist in this IWAD -- nothing was tested. The owner's IWAD is Ultimate Doom: use e1m1." }
elseif (-not $reachedMarker) { Write-Warning "RED: the level never reached the BOOT marker -- see the output file below." }
else { Write-Output "GREEN: level ran to the BOOT marker." }
# WHY IT FAILED, printed only when it did (step 66). Three things that were invisible before:
#
#   the windows   a crash or assert dialog on a hidden desktop stalls the engine where nobody
#                 can see it, and the run then reads as a plain hang.
#   the exit      a process that LEFT is not a process that HUNG, and the two were indistinguishable.
#                 Exit code 0 after a fatal is normal for this engine, so the code is reported as
#                 a fact rather than read as a verdict.
#   crash folders the engine writes crash_<stamp>\ beside the exe. Only ones created DURING this
#                 run are named: the folder has old ones going back to September.
if (-not $reachedMarker) {
    Write-Output '--- why (step 66 diagnostics) ---'
    Write-Output ("  windows on the hidden desktop: {0}" -f $(if ($winTitles) { $winTitles } else { '(none titled)' }))
    Write-Output ("  process: {0}{1}" -f $(if ($killed) { 'killed by the harness after the timeout' } else { "exited on its own, code $($p.ExitCode)" }),
                                         $(if ($cpuAtEnd) { " -- $cpuAtEnd when the loop gave up" } else { '' }))
    $fresh = @(Get-ChildItem -Path $eng, $scratch -Directory -Filter 'crash_*' -ErrorAction SilentlyContinue |
               Where-Object { $_.CreationTime -ge $runStarted })
    if ($fresh) { foreach ($c in $fresh) { Write-Warning ("  CRASH FOLDER written by this run: {0}" -f $c.FullName) } }
    else        { Write-Output  '  crash folders: none written by this run' }
}
$L = @(Get-Content $out)
Write-Output '--- key lines ---'
$L | Where-Object { $_ -match 'BOOT:|script parsing took|Vulkan|GPU|OpenXR|vr_mode|MAP01|Entryway' } | Select-Object -First 25 | ForEach-Object { Write-Output "  $_" }
Write-Output '--- errors / warnings after the script compile ---'
$i0 = [Array]::FindIndex($L, [Predicate[string]]{ param($x) $x -match 'script parsing took' })
if ($i0 -ge 0) { $L[$i0..($L.Count - 1)] | Where-Object { $_ -match 'error|Error|fatal|Fatal|abort|Unknown|not found|failed|Failed|exception' -and $_ -notmatch 'sndinfo' } | Select-Object -First 40 | ForEach-Object { Write-Output "  $_" } }
else { Write-Output '  (script parsing line never printed)' }
Write-Output '--- tail 20 ---'
$L | Select-Object -Last 20 | ForEach-Object { Write-Output "  $_" }
Write-Output "full output: $out"
