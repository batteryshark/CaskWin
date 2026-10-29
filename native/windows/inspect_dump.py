"""Inspect module-relative addresses in a captured x64 Windows minidump (no execution)."""
import pathlib
import struct
import sys

data = pathlib.Path(sys.argv[1]).read_bytes()
u32 = lambda at: struct.unpack_from('<I', data, at)[0]
u64 = lambda at: struct.unpack_from('<Q', data, at)[0]
assert data[:4] == b'MDMP'
streams = {}
for i in range(u32(8)):
    kind, size, rva = struct.unpack_from('<III', data, u32(12) + i * 12)
    streams[kind] = (rva, size)
modules = []
rva, _ = streams[4]
for i in range(u32(rva)):
    p = rva + 4 + 108 * i
    name = u32(p + 20)
    name = data[name + 4:name + 4 + u32(name)].decode('utf-16-le')
    modules.append((u64(p), u32(p + 8), name))

def describe(address):
    for base, size, name in modules:
        if base <= address < base + size:
            return f'{pathlib.PureWindowsPath(name).name}+0x{address-base:x}'
    return None

exception, _ = streams[6]
tid = u32(exception)
print('Exception', hex(u32(exception + 8)), 'at', describe(u64(exception + 24)), 'thread', tid)
threads, _ = streams[3]
for i in range(u32(threads)):
    p = threads + 4 + 48 * i
    if u32(p) != tid:
        continue
    start, size, offset = u64(p + 24), u32(p + 32), u32(p + 36)
    context = u32(exception + 164)
    rsp = u64(context + 152)
    print('RSP', hex(rsp))
    for at in range(max(0, rsp-start), min(size, rsp-start+0x1000), 8):
        address = u64(offset + at)
        text = describe(address)
        if text:
            print(f'  {start+at-rsp:04x}: {text}')
for base, size, name in modules:
    print(hex(base), hex(size), name)
