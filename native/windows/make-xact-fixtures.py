#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Repack Wine's small XACT test bank for real sector-aligned streaming.

The original stays read-only. PCM is repeated five times so streaming must
refill beyond FACT's one-second buffer; ADPCM/xWMA and their seek data are kept.
"""
import pathlib
import struct
import sys

source = pathlib.Path(sys.argv[1]).read_bytes()
target = pathlib.Path(sys.argv[2])
header = list(struct.unpack_from('<13I', source))
assert header[:3] == [0x444e4257, 46, 44]
bank_offset, entry_offset, wave_offset = header[3], header[5], header[11]
assert struct.unpack_from('<I', source, bank_offset + 4)[0] == 3
data = bytearray(source[:wave_offset])
struct.pack_into('<I', data, bank_offset,
                 struct.unpack_from('<I', data, bank_offset)[0] | 1)
struct.pack_into('<I', data, bank_offset + 80, 2048)
data.extend(bytes((-len(data)) % 2048))
new_wave_offset = len(data)
for index in range(3):
    entry = list(struct.unpack_from('<6I', source, entry_offset + index * 24))
    payload = source[wave_offset + entry[2]:wave_offset + entry[2] + entry[3]]
    if index == 0:
        assert entry[1] & 3 == 0
        payload *= 5
        entry[0] = ((entry[0] >> 4) * 5 << 4) | (entry[0] & 15)
    data.extend(bytes((-len(data)) % 2048))
    entry[2], entry[3] = len(data) - new_wave_offset, len(payload)
    data.extend(payload)
    struct.pack_into('<6I', data, entry_offset + index * 24, *entry)
data.extend(bytes((-len(data)) % 2048))
header[11], header[12] = new_wave_offset, len(data) - new_wave_offset
struct.pack_into('<13I', data, 0, *header)
target.write_bytes(data)
for name, bank in (('silent-play.xwb', bytearray(source)), ('silent-streaming.xwb', bytearray(data))):
    words = struct.unpack_from('<13I', bank)
    entry = struct.unpack_from('<6I', bank, words[5])
    offset = words[11] + entry[2]
    bank[offset:offset + entry[3]] = bytes([128]) * entry[3]
    target.with_name(name).write_bytes(bank)
