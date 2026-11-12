#!/usr/bin/env python3
"""End the flat JOINER started by nettest.py -Live (the doomxr.exe whose command line has -join),
leaving the VR host running. Owner's bridge, 2026-10-31: with two copies on one PC the OpenXR
runtime gave controller input to the joiner, so the headset copy had no buttons."""
import subprocess, sys

out = subprocess.run(['wmic', 'process', 'where', "name='doomxr.exe'", 'get', 'ProcessId,CommandLine', '/format:list'],
                     capture_output=True, text=True, errors='replace').stdout
pid, cmdline, killed = None, None, 0
for line in out.splitlines():
    line = line.strip()
    if line.startswith('CommandLine='): cmdline = line[12:]
    elif line.startswith('ProcessId='):
        pid = line[10:].strip()
        if cmdline and '-join' in cmdline and pid:
            subprocess.run(['taskkill', '/PID', pid, '/F'], capture_output=True)
            print('ended joiner pid %s' % pid); killed += 1
        cmdline = None
print('%d joiner(s) ended; the VR host keeps running' % killed)
sys.exit(0)
