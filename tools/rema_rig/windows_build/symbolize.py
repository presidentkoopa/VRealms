#!/usr/bin/env python3
"""Name the function and line at a crash address, from the build's PDB, through dbghelp.

    python tools\\windows_build\\symbolize.py <report.txt> [more report.txt...]
    python tools\\windows_build\\symbolize.py --rva 0x68551c          (offset from the exe's base)
Reads "Address:" and the "doomxr.exe" module line from a crash report (CrashReport\\report.txt), then
asks dbghelp for the symbol and source line of that address and of every address in the "Stack
Contents" dump that falls inside doomxr.exe (a crude stack walk, but it names the callers).
"""
import ctypes, ctypes.wintypes as wt, os, re, sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = os.path.join(REPO, 'build-dxr', 'RelWithDebInfo', 'doomxr.exe')


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_ulonglong * 2),
                ('Index', wt.ULONG), ('Size', wt.ULONG), ('ModBase', ctypes.c_ulonglong), ('Flags', wt.ULONG),
                ('Value', ctypes.c_ulonglong), ('Address', ctypes.c_ulonglong), ('Register', wt.ULONG),
                ('Scope', wt.ULONG), ('Tag', wt.ULONG), ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG),
                ('Name', ctypes.c_char * 1024)]


class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.DWORD), ('Key', ctypes.c_void_p), ('LineNumber', wt.DWORD),
                ('FileName', ctypes.c_char_p), ('Address', ctypes.c_ulonglong)]


def main(argv):
    dbg = ctypes.windll.dbghelp
    dbg.SymInitialize.argtypes = [wt.HANDLE, ctypes.c_char_p, wt.BOOL]
    dbg.SymLoadModuleEx.restype = ctypes.c_ulonglong
    dbg.SymLoadModuleEx.argtypes = [wt.HANDLE, wt.HANDLE, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_ulonglong, wt.DWORD, ctypes.c_void_p, wt.DWORD]
    dbg.SymFromAddr.argtypes = [wt.HANDLE, ctypes.c_ulonglong, ctypes.POINTER(ctypes.c_ulonglong), ctypes.POINTER(SYMBOL_INFO)]
    dbg.SymGetLineFromAddr64.argtypes = [wt.HANDLE, ctypes.c_ulonglong, ctypes.POINTER(wt.DWORD), ctypes.POINTER(IMAGEHLP_LINE64)]
    dbg.SymSetOptions(0x10 | 0x2)   # SYMOPT_LOAD_LINES | SYMOPT_UNDNAME (no deferred loads: load now, fail loudly)
    proc = wt.HANDLE(4242)
    if not dbg.SymInitialize(proc, os.path.dirname(EXE).encode(), False):
        print('SymInitialize failed', ctypes.GetLastError()); return 2
    base = 0x10000000
    mod = dbg.SymLoadModuleEx(proc, None, EXE.encode(), None, base, 0x3600000, None, 0)
    if not mod:
        print('SymLoadModuleEx failed', ctypes.GetLastError()); return 2
    print('symbols:', EXE, 'pdb beside it:', os.path.exists(EXE[:-4] + '.pdb'))

    def name(rva):
        sym = SYMBOL_INFO(); sym.SizeOfStruct = 88; sym.MaxNameLen = 1024   # 88 = sizeof(SYMBOL_INFO) with Name[1], padded
        disp = ctypes.c_ulonglong(0)
        s = '?'
        if dbg.SymFromAddr(proc, base + rva, ctypes.byref(disp), ctypes.byref(sym)):
            s = '%s+0x%x' % (sym.Name.decode(errors='replace'), disp.value)
        else:
            s = '? (SymFromAddr error %d)' % ctypes.GetLastError()
        line = IMAGEHLP_LINE64(); line.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINE64)
        ld = wt.DWORD(0)
        if dbg.SymGetLineFromAddr64(proc, base + rva, ctypes.byref(ld), ctypes.byref(line)):
            s += '  %s:%d' % (os.path.basename(line.FileName.decode(errors='replace')), line.LineNumber)
        return s

    if argv and argv[0] == '--rva':
        print('0x%x: %s' % (int(argv[1], 16), name(int(argv[1], 16)))); return 0
    for rp in argv:
        txt = open(rp, errors='replace').read()
        m = re.search(r'Address:\s*([0-9a-fA-F]+)', txt)
        mb = re.search(r'([0-9a-fA-F]{16}) - [0-9a-fA-F]{16} \*doomxr\.exe', txt)
        if not m or not mb:
            print(rp, ': no address / module base found'); continue
        addr, mbase = int(m.group(1), 16), int(mb.group(1), 16)
        size = 0x3600000
        print('== %s\ncrash: rva 0x%x  %s' % (rp, addr - mbase, name(addr - mbase)))
        st = txt.find('Stack Contents:')
        seen = 0
        if st >= 0:
            for word in re.findall(r'\b([0-9a-fA-F]{16})\b', txt[st:]):
                v = int(word, 16)
                if mbase <= v < mbase + size:
                    print('  stack: rva 0x%x  %s' % (v - mbase, name(v - mbase)))
                    seen += 1
                    if seen >= 25: break
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
