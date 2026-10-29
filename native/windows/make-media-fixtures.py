#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Small original ISO9660/Joliet fixtures. No game files or image tools required."""
import hashlib
import json
from pathlib import Path
import struct
import sys

SECTOR = 2048
BLOCKS = 48
PAYLOAD = bytes((i * 17 + 3) % 256 for i in range(5000))


def both16(n):
    return struct.pack('<H', n) + struct.pack('>H', n)


def both32(n):
    return struct.pack('<I', n) + struct.pack('>I', n)


def record(name, block, size, directory=False):
    n = 33 + len(name) + (len(name) % 2 == 0)
    r = bytearray(n)
    r[0] = n
    r[2:10] = both32(block)
    r[10:18] = both32(size)
    r[18:25] = bytes((126, 10, 29, 12, 0, 0, 0))
    r[25] = 2 if directory else 0
    r[28:32] = both16(1)
    r[32] = len(name)
    r[33:33 + len(name)] = name
    return r


def image(joliet=True, bad=None):
    data = bytearray(BLOCKS * SECTOR)
    locations = {}
    for is_joliet in ([False, True] if joliet else [False]):
        root = 22 if is_joliet else 20
        sub = root + 1
        desc = 17 if is_joliet else 16
        d = bytearray(SECTOR)
        d[0:7] = bytes((2 if is_joliet else 1,)) + b'CD001\x01'
        d[8:40] = b'WINEDOWS'.ljust(32)
        d[40:72] = ('WD_DISC_\u03a9'.ljust(16).encode('utf-16-be') if is_joliet else b'TEST_DISC'.ljust(32))
        d[80:88] = both32(BLOCKS)
        if is_joliet:
            d[88:91] = b'%/E'
        d[120:124] = both16(1)
        d[124:128] = both16(1)
        d[128:132] = both16(SECTOR)
        d[156:190] = record(b'\0', root, SECTOR, True)
        d[881] = 1
        name = lambda s: s.encode('utf-16-be' if is_joliet else 'ascii')
        entries = [record(b'\0', root, SECTOR, True), record(b'\1', root, SECTOR, True)]
        records = [
            ('Readme.txt;1' if is_joliet else 'README.TXT;1', 28, 22, False),
            ('DATA', sub, SECTOR, True),
            ('Payload.bin;1' if is_joliet else 'PAYLOAD.BIN;1', 30, len(PAYLOAD), False),
            ('Empty.;1' if is_joliet else 'EMPTY.;1', 34, 0, False),
        ]
        if is_joliet:
            records.append(('R\u00e9sum\u00e9 \u03a9.txt;1', 33, 13, False))
        if bad == 'traversal':
            records.append(('..\\escape;1', 28, 22, False))
        if bad == 'reserved-name':
            records.append(('CON.TXT;1', 28, 22, False))
        if bad == 'name-collision':
            records.append(('readme.txt;1', 28, 22, False))
        if bad == 'directory-cycle':
            records.append(('LOOP', root, SECTOR, True))
        for identifier, block, size, is_dir in records:
            offset = root * SECTOR + sum(map(len, entries))
            r = record(name(identifier), block, size, is_dir)
            if identifier.lower().startswith('readme') and not identifier.startswith('readme'):
                locations['file'] = offset
            entries.append(r)
        directory = b''.join(entries)
        data[root * SECTOR:root * SECTOR + len(directory)] = directory
        nested = b''.join((record(b'\0', sub, SECTOR, True), record(b'\1', root, SECTOR, True),
                           record(name('Nested.txt;1' if is_joliet else 'NESTED.TXT;1'), 29, 17)))
        data[sub * SECTOR:sub * SECTOR + len(nested)] = nested
        # Both endian path tables make these usable by independent ISO readers.
        lblock, mblock = ((26, 27) if is_joliet else (24, 25))
        tables = []
        for endian in ('<', '>'):
            table = b'\x01\0' + struct.pack(endian + 'IH', root, 1) + b'\0\0'
            encoded = name('DATA')
            table += bytes((len(encoded), 0)) + struct.pack(endian + 'IH', sub, 1) + encoded
            if len(encoded) % 2:
                table += b'\0'
            tables.append(table)
        d[132:140] = both32(len(tables[0]))
        d[140:144] = struct.pack('<I', lblock)
        d[148:152] = struct.pack('>I', mblock)
        for block, table in zip((lblock, mblock), tables):
            data[block * SECTOR:block * SECTOR + len(table)] = table
        data[desc * SECTOR:(desc + 1) * SECTOR] = d
    end = 18 if joliet else 17
    data[end * SECTOR:end * SECTOR + 7] = b'\xffCD001\x01'
    payloads = [(28, b'Hello from Winedows!\r\n'), (29, b'nested ISO data\r\n'), (30, PAYLOAD)]
    if joliet:
        payloads.append((33, b'Joliet names\n'))
    for block, content in payloads:
        data[block * SECTOR:block * SECTOR + len(content)] = content
    # Declared lengths are exact, including the deliberately empty file.
    for root in ([20, 22] if joliet else [20]):
        first = root * SECTOR + 68
        data[first + 10:first + 18] = both32(22)
    f = locations['file']
    if bad == 'out-of-range':
        data[f + 2:f + 10] = both32(BLOCKS)
    elif bad == 'endian-mismatch':
        data[f + 6] ^= 1
    elif bad == 'multi-extent':
        data[f + 25] |= 0x80
    elif bad == 'interleaved':
        data[f + 26] = 1
    elif bad == 'extended-attributes':
        data[f + 1] = 1
    elif bad == 'other-volume':
        data[f + 28:f + 32] = both16(2)
    elif bad == 'bad-signature':
        data[16 * SECTOR + 1] = ord('X')
    elif bad == 'no-terminator':
        data[end * SECTOR] = 0
    elif bad == 'truncated':
        data = data[:-1]
    elif bad == 'bad-block-size':
        data[16 * SECTOR + 128:16 * SECTOR + 132] = both16(512)
    elif bad == 'volume-overflow':
        data[16 * SECTOR + 80:16 * SECTOR + 88] = both32(0xffffffff)
    elif bad == 'label-newline':
        label = (17 if joliet else 16) * SECTOR + 40
        data[label + (1 if joliet else 0)] = 10
    return data


def serial(data, joliet):
    descriptor = data[(17 if joliet else 16) * SECTOR:(18 if joliet else 17) * SECTOR]
    sums = [sum(descriptor[i::4]) % 256 for i in range(4)]
    return int.from_bytes(bytes(sums), 'big')


def excessive_metadata():
    # Five overlapping 16 MiB directories fit in a ~16 MiB image, but a naive
    # recursive parser retains a full buffer for each. Unique child names and
    # extents ensure rejection comes from the aggregate budget, not a cycle.
    root, directory_sectors = 20, 8192
    blocks = root + directory_sectors + 8
    data = bytearray(blocks * SECTOR)
    data[:root * SECTOR] = image(False)[:root * SECTOR]
    data[16 * SECTOR + 80:16 * SECTOR + 88] = both32(blocks)
    data[16 * SECTOR + 156:16 * SECTOR + 190] = record(b'\0', root, directory_sectors * SECTOR, True)
    for n in range(5):
        child = record(f'LEVEL{n}'.encode('ascii'), root + n + 1, directory_sectors * SECTOR, True)
        offset = (root + n) * SECTOR
        data[offset:offset + len(child)] = child
    return data


def main():
    output = Path(sys.argv[1])
    output.mkdir(parents=True, exist_ok=True)
    manifest = {}
    for title, joliet in (('primary', False), ('joliet', True)):
        data = image(joliet)
        path = output / (title + '.iso')
        path.write_bytes(data)
        manifest[title] = dict(path=str(path.resolve()), size=len(data), serial=serial(data, joliet),
                               sha256=hashlib.sha256(data).hexdigest(), label='WD_DISC_\u03a9' if joliet else 'TEST_DISC')
    malformed = ('traversal', 'reserved-name', 'name-collision', 'directory-cycle', 'out-of-range',
                 'endian-mismatch', 'multi-extent', 'interleaved', 'extended-attributes', 'other-volume',
                 'bad-signature', 'no-terminator', 'truncated', 'bad-block-size', 'volume-overflow', 'label-newline')
    for title in malformed:
        (output / ('bad-' + title + '.iso')).write_bytes(image(True, title))
    (output / 'bad-aggregate-metadata.iso').write_bytes(excessive_metadata())
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(f'Created primary + Joliet fixtures and {len(malformed) + 1} malformed images in {output}')


if __name__ == '__main__':
    main()
