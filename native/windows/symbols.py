"""Resolve local PDB symbols for a list of module-relative offsets."""
import ctypes as c
from ctypes import wintypes as w
import pathlib
import sys

class Symbol(c.Structure):
    _fields_ = [('SizeOfStruct', w.ULONG), ('TypeIndex', w.ULONG), ('Reserved', c.c_ulonglong*2), ('Size', w.ULONG), ('ModBase', c.c_ulonglong), ('Flags', w.ULONG), ('Value', c.c_ulonglong), ('Address', c.c_ulonglong), ('Register', w.ULONG), ('Scope', w.ULONG), ('Tag', w.ULONG), ('NameLen', w.ULONG), ('MaxNameLen', w.ULONG), ('Name', c.c_char*1024)]

dbg = c.WinDLL('dbghelp', use_last_error=True)
kernel = c.WinDLL('kernel32')
kernel.GetCurrentProcess.restype = w.HANDLE
proc = kernel.GetCurrentProcess()
dbg.SymInitializeW.argtypes = [w.HANDLE, w.LPCWSTR, w.BOOL]
dbg.SymLoadModuleExW.argtypes = [w.HANDLE, w.HANDLE, w.LPCWSTR, w.LPCWSTR, c.c_ulonglong, w.DWORD, w.LPVOID, w.DWORD]
dbg.SymLoadModuleExW.restype = c.c_ulonglong
dbg.SymFromAddr.argtypes = [w.HANDLE, c.c_ulonglong, c.POINTER(c.c_ulonglong), c.POINTER(Symbol)]
path = pathlib.Path(sys.argv[1]).resolve()
assert dbg.SymInitializeW(proc, str(path.parent), False)
base = dbg.SymLoadModuleExW(proc, None, str(path), None, 0, 0, None, 0)
assert base, c.get_last_error()
for offset in sys.argv[2:]:
    sym = Symbol(); sym.SizeOfStruct = 88; sym.MaxNameLen = 1024
    displacement = c.c_ulonglong()
    if dbg.SymFromAddr(proc, base+int(offset,16), c.byref(displacement), c.byref(sym)):
        print(offset, sym.Name.decode(errors='replace'), hex(displacement.value))
    else:
        print(offset, 'unresolved', c.get_last_error())
