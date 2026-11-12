#!/usr/bin/env python3
"""Build REMA on the owner's Windows PC: build-dxr (Visual Studio generator), RelWithDebInfo.

Run by hand:            python tools\\windows_build\\build.py
Run through the bridge: a request file whose lines are
    python
    E:\\DOOMWork\\UZDXREMA\\tools\\windows_build\\build.py
Target zdoom -> build-dxr\\RelWithDebInfo\\doomxr.exe (+ the pk3s beside it, which the boot test loads).
The full log goes to build-dxr\\lastbuild.log; stdout is the summary and the log's tail only,
so a bridge response stays readable. Exit code 0 only if doomxr.exe is newer than the start.
"""
import os, subprocess, sys, time, glob

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.join(REPO, 'build-dxr')
CONFIG = 'RelWithDebInfo'
EXE = os.path.join(BUILD, CONFIG, 'doomxr.exe')
LOG = os.path.join(BUILD, 'lastbuild.log')


def find_cmake():
    for line in open(os.path.join(BUILD, 'CMakeCache.txt'), errors='replace'):
        if line.startswith('CMAKE_COMMAND:INTERNAL='):
            p = line.split('=', 1)[1].strip()
            if os.path.exists(p):
                return p
    for p in glob.glob(r'C:\Program Files\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'):
        return p
    return 'cmake'


def busy():
    """Another build of THIS tree is running: a cmake/link driver alive and build-dxr written
    to in the last 3 minutes (the guard from tools/claude_bridge/run_build.ps1; two builds on
    one tree race on the objects and have deleted the exe before)."""
    try:
        out = subprocess.run(['tasklist', '/FO', 'CSV', '/NH'], capture_output=True, text=True).stdout.lower()
    except Exception:
        return False
    if '"cmake.exe"' not in out and '"link.exe"' not in out:
        return False
    cutoff = time.time() - 180
    for root, _, files in os.walk(BUILD):
        for f in files:
            try:
                if os.path.getmtime(os.path.join(root, f)) > cutoff:
                    return True
            except OSError:
                pass
    return False


def main():
    if not os.path.exists(os.path.join(BUILD, 'CMakeCache.txt')):
        print('no configured build at', BUILD); return 2
    if busy():
        print('refused: a build of this tree is already running'); return 3
    os.environ['MSBUILDDISABLENODEREUSE'] = '1'   # a leftover worker node deadlocks the next build
    cmake = find_cmake()
    start = time.time()
    head = subprocess.run(['git', '-C', REPO, 'log', '--oneline', '-1'], capture_output=True, text=True).stdout.strip()
    print('building', head, 'in', BUILD, 'with', cmake, flush=True)
    cmd = [cmake, '--build', BUILD, '--config', CONFIG, '--target', 'zdoom', '--', '/m', '/v:m', '/nologo']
    with open(LOG, 'w', errors='replace') as log:
        log.write('=== ' + head + '\n=== ' + ' '.join(cmd) + '\n')
        log.flush()
        rc = subprocess.call(cmd, stdout=log, stderr=subprocess.STDOUT, cwd=BUILD)
    # LNK1103 "debugging information corrupt" is the incremental LTCG cache gone stale against a
    # recompiled object (run_build.ps1 guard 5: six times on one day). The cure is mechanical and
    # always the same: delete the cache, KEEP the objects, link again. Once only.
    if rc != 0 and 'LNK1103' in open(LOG, errors='replace').read():
        ltcg = os.path.join(BUILD, 'src', 'zdoom.dir', CONFIG)
        for f in (os.path.join(ltcg, 'doomxr.iobj'), os.path.join(ltcg, 'doomxr.ipdb'), os.path.join(BUILD, CONFIG, 'doomxr.pdb')):
            if os.path.exists(f):
                os.remove(f)
        with open(LOG, 'a', errors='replace') as log:
            log.write('=== LNK1103: cleared the LTCG cache (iobj/ipdb/pdb), kept the objects, relinking\n')
            log.flush()
            rc = subprocess.call(cmd, stdout=log, stderr=subprocess.STDOUT, cwd=BUILD)
    lines = open(LOG, errors='replace').read().splitlines()
    errors = [l for l in lines if ' error ' in l or 'error C' in l or 'error LNK' in l or 'fatal error' in l]
    for l in errors[:30]:
        print(l)
    print('--- log tail')
    for l in lines[-25:]:
        print(l)
    fresh = os.path.exists(EXE) and os.path.getmtime(EXE) >= start - 1
    print('--- cmake exit %d, %d error lines, %.0f s, doomxr.exe %s' % (rc, len(errors), time.time() - start, 'rebuilt' if fresh else 'NOT rebuilt'))
    print('BUILT: ' + EXE if (rc == 0 and fresh) else 'BUILD FAILED, see ' + LOG)
    return 0 if (rc == 0 and fresh) else 1


if __name__ == '__main__':
    sys.exit(main())
