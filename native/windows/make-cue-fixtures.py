#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Original mixed-mode BIN/CUE fixture with stored and inserted audio pregaps."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys

spec = importlib.util.spec_from_file_location('iso_fixture', Path(__file__).with_name('make-media-fixtures.py'))
iso_fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(iso_fixture)

CUE = '''REM Original generated fixture; no game assets
TITLE "Generated Mixed Mode"
FILE "disc.bin" BINARY
  TRACK 01 MODE1/2352
    INDEX 01 00:00:00
  TRACK 02 AUDIO
    PREGAP 00:00:03
    INDEX 00 00:00:64
    INDEX 01 00:00:66
  TRACK 03 AUDIO
    PREGAP 00:00:04
    INDEX 00 00:01:04
    INDEX 01 00:01:06
'''


def bcd(n):
    return (n // 10) * 16 + n % 10


def make_bin():
    iso = iso_fixture.image(False)
    output = bytearray()
    for lba in range(99):
        if lba < 64:
            frame = lba + 150
            header = b'\0' + b'\xff' * 10 + b'\0' + bytes((bcd(frame // 4500), bcd(frame // 75 % 60), bcd(frame % 75), 1))
            payload = iso[lba * 2048:(lba + 1) * 2048] if lba < 48 else bytes((lba ^ 0xa5,)) * 2048
            # EDC/ECC are not consumed by this payload adapter.
            output += header + payload + bytes(288)
        else:
            output += b''.join(struct.pack('<hh', lba * 64 + n, -(lba * 64 + n)) for n in range(588))
    return output


def main():
    root = Path(sys.argv[1])
    root.mkdir(parents=True, exist_ok=True)
    binary = make_bin()
    (root / 'disc.bin').write_bytes(binary)
    (root / 'disc.cue').write_text(CUE, encoding='utf-8')
    malformed = {
        'traversal': CUE.replace('"disc.bin"', '"../outside.bin"'),
        'absolute-file': CUE.replace('"disc.bin"', '"C:\\outside.bin"'),
        'multiple-files': CUE.replace('  TRACK 03', 'FILE "disc.bin" BINARY\n  TRACK 03'),
        'mode2': CUE.replace('MODE1/2352', 'MODE2/2352'),
        'wrong-file-type': CUE.replace('BINARY', 'WAVE'),
        'track-number': CUE.replace('TRACK 03', 'TRACK 04'),
        'duplicate-index': CUE.replace('INDEX 01 00:00:66', 'INDEX 01 00:00:66\n    INDEX 01 00:00:66'),
        'extra-index': CUE.replace('INDEX 01 00:00:66', 'INDEX 01 00:00:66\n    INDEX 02 00:00:67'),
        'first-index0': CUE.replace('INDEX 01 00:00:00', 'INDEX 00 00:00:00\n    INDEX 01 00:00:00'),
        'first-offset': CUE.replace('INDEX 01 00:00:00', 'INDEX 01 00:00:01'),
        'index-outside': CUE.replace('INDEX 01 00:01:06', 'INDEX 01 00:02:00'),
        'inverted-indexes': CUE.replace('INDEX 00 00:00:64', 'INDEX 00 00:00:67'),
        'missing-index': CUE.replace('    INDEX 01 00:01:06\n', ''),
        'overlap': CUE.replace('INDEX 00 00:01:04', 'INDEX 00 00:00:65'),
        'invalid-msf': CUE.replace('PREGAP 00:00:04', 'PREGAP 00:60:00'),
        'postgap': CUE + '    POSTGAP 00:00:02\n',
        'flags': CUE.replace('  TRACK 02 AUDIO', '  TRACK 02 AUDIO\n    FLAGS PRE'),
    }
    for name, contents in malformed.items():
        (root / ('bad-' + name + '.cue')).write_text(contents, encoding='utf-8')
    for name, contents in (
        ('truncated-bin', binary[:-1]),
        ('bad-sync', bytes((1,)) + binary[1:]),
        ('bad-mode', binary[:15] + bytes((2,)) + binary[16:]),
    ):
        (root / (name + '.bin')).write_bytes(contents)
        (root / ('bad-' + name + '.cue')).write_text(CUE.replace('disc.bin', name + '.bin'), encoding='utf-8')
    metadata = {
        'cue': str((root / 'disc.cue').resolve()), 'bin': str((root / 'disc.bin').resolve()),
        'cue_sha256': hashlib.sha256(CUE.encode('utf-8')).hexdigest(),
        'bin_sha256': hashlib.sha256(binary).hexdigest(),
        'data_sectors': 64, 'filesystem_sectors': 48,
        'extent_starts': [0, 64, 82], 'index01_lba': [0, 69, 88], 'leadout_lba': 106,
        'malformed': len(malformed) + 3,
    }
    # Path.write_text uses platform newline translation; hash the actual file.
    metadata['cue_sha256'] = hashlib.sha256((root / 'disc.cue').read_bytes()).hexdigest()
    (root / 'fixture.json').write_text(json.dumps(metadata, indent=2), encoding='utf-8')
    print(f'Mixed-mode fixtures ready: {root}')


if __name__ == '__main__':
    main()
