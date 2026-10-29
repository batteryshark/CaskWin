#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Three original, intentionally distinguishable discs for live replacement."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys


def module(name, file):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(file))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


iso = module('swap_iso', 'make-media-fixtures.py')
cue = module('swap_cue', 'make-cue-fixtures.py')


def make_iso(label, content):
    result = bytearray(iso.image(False))
    assert len(content) == 22
    result[16 * 2048 + 40:16 * 2048 + 72] = label.encode('ascii').ljust(32, b' ')
    result[28 * 2048:28 * 2048 + 22] = content
    return result


def raw(data, data_sectors, total_sectors, negative=False):
    output = bytearray()
    for sector in range(total_sectors):
        if sector < data_sectors:
            frame = sector + 150
            header = b'\0' + b'\xff' * 10 + b'\0' + bytes((cue.bcd(frame // 4500), cue.bcd(frame // 75 % 60), cue.bcd(frame % 75), 1))
            payload = data[sector * 2048:(sector + 1) * 2048] if sector < 48 else bytes((sector ^ (0x5a if negative else 0xa5),)) * 2048
            output += header + payload + bytes(288)
        else:
            for n in range(588):
                sample = -(sector * 80 + n) if negative else sector * 64 + n
                output += struct.pack('<hh', sample, -sample)
    return output


def main():
    root = Path(sys.argv[1]);root.mkdir(parents=True, exist_ok=True)
    a = make_iso('SWAP_DISC_A', b'Hello from Winedows!\r\n')
    b = make_iso('SWAP_DISC_B', b'Different disc B data\n')
    c = make_iso('SWAP_DISC_C', b'Third disc C contents\n')
    (root / 'disc-a.bin').write_bytes(raw(a, 64, 99))
    (root / 'disc-a.cue').write_text(cue.CUE.replace('disc.bin', 'disc-a.bin'), encoding='utf-8')
    (root / 'disc-b.bin').write_bytes(raw(b, 72, 118, True))
    (root / 'disc-b.cue').write_text('''FILE "disc-b.bin" BINARY
  TRACK 01 MODE1/2352
    INDEX 01 00:00:00
  TRACK 02 AUDIO
    INDEX 00 00:00:72
    INDEX 01 00:00:74
  TRACK 03 AUDIO
    PREGAP 00:00:05
    INDEX 00 00:01:14
    INDEX 01 00:01:17
''', encoding='utf-8')
    (root / 'disc-c.iso').write_bytes(c)
    (root / 'invalid.iso').write_bytes(b'not an ISO')
    hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in root.iterdir() if p.is_file()}
    (root / 'fixture.json').write_text(json.dumps({'hashes': hashes, 'a': {'toc': [150,219,238,256]}, 'b': {'toc': [150,224,247,273]}}, indent=2), encoding='utf-8')
    print(f'Live swap fixtures ready: {root}')


if __name__ == '__main__':
    main()
