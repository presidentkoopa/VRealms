#!/usr/bin/env python3
"""End every doomxr.exe (a stuck copy keeps the build from writing the exe: LNK1104)."""
import subprocess
r = subprocess.run(['taskkill', '/IM', 'doomxr.exe', '/F'], capture_output=True, text=True, errors='replace')
print((r.stdout + r.stderr).strip() or 'no doomxr.exe was running')
