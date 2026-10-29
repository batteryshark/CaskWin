#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Prepared CDDA fixture: synthetic data track, 3s silence, 2s known PCM."""
from pathlib import Path
import hashlib
import json
import struct
import sys

MANIFEST = '''[disc]
version=1
tracks=3
data_source=synthetic
[track01]
type=data
sectors=150
[track02]
type=audio
file=track02.pcm
sectors=225
[track03]
type=audio
file=track03.pcm
sectors=150
'''


def main():
    root = Path(sys.argv[1])
    root.mkdir(parents=True, exist_ok=True)
    silence = bytes(225 * 2352)
    known = b''.join(struct.pack('<hh', (i % 1000) - 500, 500 - (i % 1000)) for i in range(150 * 588))
    valid = root / 'valid'
    valid.mkdir(exist_ok=True)
    (valid / 'disc.ini').write_text(MANIFEST, encoding='ascii')
    (valid / 'track02.pcm').write_bytes(silence)
    (valid / 'track03.pcm').write_bytes(known)
    two_tracks = MANIFEST.split('[track03]')[0].replace('tracks=3', 'tracks=2')
    alternate = root / 'alternate'
    alternate.mkdir(exist_ok=True)
    (alternate / 'disc.ini').write_text(two_tracks.replace('sectors=225', 'sectors=150'), encoding='ascii')
    (alternate / 'track02.pcm').write_bytes(known)
    boundary = root / 'boundary-valid'
    boundary.mkdir(exist_ok=True)
    boundary_manifest = two_tracks.replace('sectors=150', 'sectors=1151848').replace('sectors=225', 'sectors=1')
    (boundary / 'disc.ini').write_text(boundary_manifest, encoding='ascii')
    (boundary / 'track02.pcm').write_bytes(bytes(2352))
    malformed = {
        'traversal': MANIFEST.replace('file=track03.pcm', 'file=..\\escape.pcm'),
        'size': MANIFEST.replace('sectors=225', 'sectors=226'),
        'integer': MANIFEST.replace('sectors=225', 'sectors=-1'),
        'overflow': MANIFEST.replace('sectors=225', 'sectors=4294967295'),
        'missing': MANIFEST.replace('tracks=3', 'tracks=4'),
        'data-origin': MANIFEST.replace('data_source=synthetic', 'data_source=none'),
        'false-iso': MANIFEST.replace('data_source=synthetic', 'data_source=iso'),
        'version': MANIFEST.replace('version=1', 'version=2'),
        'wrong-file': MANIFEST.replace('file=track03.pcm', 'file=track02.pcm'),
        'msf-limit': boundary_manifest.replace('sectors=1151848', 'sectors=1151849'),
    }
    for name, manifest in malformed.items():
        folder = root / ('bad-' + name)
        folder.mkdir(exist_ok=True)
        (folder / 'disc.ini').write_text(manifest, encoding='ascii')
        (folder / 'track02.pcm').write_bytes(bytes(2352) if name == 'msf-limit' else silence)
        (folder / 'track03.pcm').write_bytes(known)
    (root / 'fixture.json').write_text(json.dumps({
        'valid': str((valid / 'disc.ini').resolve()), 'malformed': len(malformed),
        'track02_sha256': hashlib.sha256(silence).hexdigest(),
        'track03_sha256': hashlib.sha256(known).hexdigest(),
        'track_starts_lba': [0, 150, 375], 'leadout_lba': 525,
        'silent_playback_track': 2,
    }, indent=2), encoding='utf-8')
    print(f'CDDA fixtures ready: {root}')


if __name__ == '__main__':
    main()
