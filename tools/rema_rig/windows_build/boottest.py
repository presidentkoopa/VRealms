#!/usr/bin/env python3
"""Run the owner's boot test (E:\\DOOMWork\\tools\\main_boottest\\main_boottest.ps1) with ANY of its
parameters, through the bridge's `python` action (whose own `boottest` action passes only a few).

    python
    E:\\DOOMWork\\UZDXREMA\\tools\\windows_build\\boottest.py
    -Tics 700
    -ScriptErrorOk No sprites specified for voxel
Each request line after the script path is one parameter: `-Name value...` (the value is the
rest of the line, so spaces are fine). A parameter without a value is passed as a switch.
"""
import subprocess, sys

PS1 = r'E:\DOOMWork\tools\main_boottest\main_boottest.ps1'


def main(argv):
    args = []
    for a in argv:
        a = a.strip()
        if not a:
            continue
        if a.startswith('-') and ' ' in a:
            name, value = a.split(' ', 1)
            args += [name, value]
        else:
            args.append(a)
    cmd = ['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', PS1] + args
    print('boottest:', ' '.join(args), flush=True)
    return subprocess.call(cmd)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
