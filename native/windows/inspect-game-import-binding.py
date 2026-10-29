"""Read-only PE IAT ownership for one exact staged executable and ancestor PID.

Reads only the matching main image's PE header and selected import-table slots
(maximum 64 KiB total). No code invocation, code disassembly, injection, UI
APIs, input, capture, stacks, environment, or unrelated memory are accessed.
Module metadata is used only to identify the owners of the import pointers.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import struct
import pefile

K = C.WinDLL("kernel32", use_last_error=True)
INVALID_HANDLE = C.c_void_p(-1).value


class ProcessEntry(C.Structure):
    _fields_ = [("dwSize", W.DWORD), ("cntUsage", W.DWORD),
                ("th32ProcessID", W.DWORD), ("th32DefaultHeapID", C.c_size_t),
                ("th32ModuleID", W.DWORD), ("cntThreads", W.DWORD),
                ("th32ParentProcessID", W.DWORD), ("pcPriClassBase", W.LONG),
                ("dwFlags", W.DWORD), ("szExeFile", W.WCHAR * 260)]


class ModuleEntry(C.Structure):
    _fields_ = [("dwSize", W.DWORD), ("th32ModuleID", W.DWORD),
                ("th32ProcessID", W.DWORD), ("GlblcntUsage", W.DWORD),
                ("ProccntUsage", W.DWORD), ("modBaseAddr", C.c_void_p),
                ("modBaseSize", W.DWORD), ("hModule", W.HMODULE),
                ("szModule", W.WCHAR * 256), ("szExePath", W.WCHAR * 260)]


def api(name, result, arguments):
    function = getattr(K, name)
    function.restype, function.argtypes = result, arguments
    return function


api("CreateToolhelp32Snapshot", W.HANDLE, [W.DWORD, W.DWORD])
api("Process32FirstW", W.BOOL, [W.HANDLE, C.POINTER(ProcessEntry)])
api("Process32NextW", W.BOOL, [W.HANDLE, C.POINTER(ProcessEntry)])
api("Module32FirstW", W.BOOL, [W.HANDLE, C.POINTER(ModuleEntry)])
api("Module32NextW", W.BOOL, [W.HANDLE, C.POINTER(ModuleEntry)])
api("OpenProcess", W.HANDLE, [W.DWORD, W.BOOL, W.DWORD])
api("CloseHandle", W.BOOL, [W.HANDLE])
api("QueryFullProcessImageNameW", W.BOOL, [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)])
api("ReadProcessMemory", W.BOOL, [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)])


def canonical(path):
    return os.path.normcase(os.path.abspath(path))


def entries(flags, pid, kind, first, following):
    snapshot = K.CreateToolhelp32Snapshot(flags, pid)
    if snapshot == INVALID_HANDLE:
        raise C.WinError(C.get_last_error())
    try:
        value = kind()
        value.dwSize = C.sizeof(value)
        if not first(snapshot, C.byref(value)):
            raise C.WinError(C.get_last_error())
        while True:
            yield kind.from_buffer_copy(value)
            if not following(snapshot, C.byref(value)):
                if C.get_last_error() != 18:  # ERROR_NO_MORE_FILES
                    raise C.WinError(C.get_last_error())
                break
    finally:
        K.CloseHandle(snapshot)


def process_path(handle):
    value = C.create_unicode_buffer(32768)
    count = W.DWORD(len(value))
    if not K.QueryFullProcessImageNameW(handle, 0, value, C.byref(count)):
        raise C.WinError(C.get_last_error())
    return value.value


def descendant(processes, pid, ancestor):
    chain, visited = [], set()
    while pid in processes and pid not in visited and len(chain) < 32:
        visited.add(pid)
        chain.append(pid)
        if pid == ancestor:
            return chain
        pid = processes[pid].th32ParentProcessID
    return None


def inspect(args):
    executable = args.executable.resolve(strict=True)
    expected_wine = canonical(args.wine_module.resolve(strict=True))
    image = executable.read_bytes()
    pe = pefile.PE(data=image, fast_load=True)
    pe.parse_data_directories([pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
    pointer_size = 4 if pe.OPTIONAL_HEADER.Magic == 0x10b else 8
    descriptors = [entry for entry in pe.DIRECTORY_ENTRY_IMPORT
                   if entry.dll.decode().casefold() == args.dll.casefold()]
    if len(descriptors) != 1 or not descriptors[0].imports:
        raise RuntimeError("Expected exactly one nonempty selected import descriptor")
    imports = descriptors[0].imports
    if len(imports) > 4096:
        raise RuntimeError("Import count exceeds the diagnostic bound")
    processes = {entry.th32ProcessID: entry for entry in entries(
        2, 0, ProcessEntry, K.Process32FirstW, K.Process32NextW)}
    matches = []
    for pid, entry in processes.items():
        if entry.szExeFile.casefold() != executable.name.casefold():
            continue
        chain = descendant(processes, pid, args.launcher_pid)
        if not chain:
            continue
        handle = K.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED_INFORMATION
        if not handle:
            continue
        try:
            if canonical(process_path(handle)) == canonical(executable):
                matches.append((pid, chain))
        finally:
            K.CloseHandle(handle)
    if len(matches) != 1:
        raise RuntimeError(f"Expected one exact staged descendant; found {len(matches)}")
    pid, chain = matches[0]
    handle = K.OpenProcess(0x1000 | 0x10, False, pid)  # QUERY_LIMITED_INFORMATION | VM_READ
    if not handle:
        raise C.WinError(C.get_last_error())
    read_count = 0
    try:
        if canonical(process_path(handle)) != canonical(executable):
            raise RuntimeError("Process identity changed before inspection")
        modules = [dict(path=entry.szExePath, base=entry.modBaseAddr,
                        size=entry.modBaseSize) for entry in entries(
                            0x8 | 0x10, pid, ModuleEntry, K.Module32FirstW, K.Module32NextW)]
        main = next((m for m in modules if canonical(m["path"]) == canonical(executable)), None)
        if not main or main["size"] != pe.OPTIONAL_HEADER.SizeOfImage:
            raise RuntimeError("Loaded main image does not match the staged PE layout")

        def read(rva, size):
            nonlocal read_count
            if rva < 0 or size < 1 or rva + size > main["size"] or size > 16384 or read_count + size > 65536:
                raise RuntimeError("Remote read exceeds the main-image IAT diagnostic bound")
            value = C.create_string_buffer(size)
            actual = C.c_size_t()
            if not K.ReadProcessMemory(handle, main["base"] + rva, value, size, C.byref(actual)) or actual.value != size:
                raise C.WinError(C.get_last_error())
            read_count += size
            return value.raw

        if read(0, 64) != image[:64]:
            raise RuntimeError("Loaded DOS header differs from staged image")
        nt_offset = pe.DOS_HEADER.e_lfanew
        if nt_offset > 16384 or read(nt_offset, 24) != image[nt_offset:nt_offset + 24]:
            raise RuntimeError("Loaded PE identity differs from staged image")
        first = min(entry.address - pe.OPTIONAL_HEADER.ImageBase for entry in imports)
        length = max(entry.address - pe.OPTIONAL_HEADER.ImageBase for entry in imports) - first + pointer_size
        values = read(first, length)
        if read(first, length) != values:
            raise RuntimeError("Selected IAT changed during inspection; retry when stable")
        bindings, counts = [], {}
        for entry in imports:
            rva = entry.address - pe.OPTIONAL_HEADER.ImageBase
            address = struct.unpack_from("<I" if pointer_size == 4 else "<Q", values, rva - first)[0]
            owner = next((m for m in modules if m["base"] <= address < m["base"] + m["size"]), None)
            owner_path = owner["path"] if owner else None
            classification = "expected-wine-builtin" if owner and canonical(owner_path) == expected_wine else "other-module" if owner else "unresolved-owner"
            counts[owner_path or "<unresolved>"] = counts.get(owner_path or "<unresolved>", 0) + 1
            bindings.append(dict(symbol=entry.name.decode() if entry.name else None,
                                 ordinal=entry.ordinal, iatRva=hex(rva),
                                 targetAddress=hex(address), ownerPath=owner_path,
                                 ownerRva=hex(address - owner["base"]) if owner else None,
                                 classification=classification))
        return dict(completed=True, utc=datetime.now(timezone.utc).isoformat(),
                    pid=pid, launcherPid=args.launcher_pid, ancestorPids=chain,
                    executable=str(executable), executableSha256=hashlib.sha256(image).hexdigest(),
                    machine=hex(pe.FILE_HEADER.Machine), imageBase=hex(main["base"]),
                    dll=args.dll, expectedWineModule=str(args.wine_module.resolve()),
                    expectedWineModuleSha256=hashlib.sha256(args.wine_module.read_bytes()).hexdigest(),
                    importCount=len(bindings), bytesRead=read_count,
                    selectedIatStable=True, ownerCounts=counts,
                    allImportsOwnedByExpectedWine=all(b["classification"] == "expected-wine-builtin" for b in bindings),
                    bindings=bindings, readOnly=True, uiApisUsed=False,
                    limitations=["Direct IAT target ownership only; detour instructions are not followed",
                                 "Only the named main-image import descriptor was read",
                                 "Binding evidence does not establish complete rendering or game compatibility"])
    finally:
        K.CloseHandle(handle)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--launcher-pid", type=int, required=True)
    parser.add_argument("--dll", required=True)
    parser.add_argument("--wine-module", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    result = inspect(arguments)
    arguments.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in ("pid", "dll", "importCount", "bytesRead", "ownerCounts", "allImportsOwnedByExpectedWine")}, indent=2))
