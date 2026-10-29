#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Bounded, read-only XACT follow-up to the saved GameRepo PE inventory."""
import argparse
import json
import pathlib
import re
import struct
import uuid
import zipfile
from wine_source import resolve_wine_source

repo = pathlib.Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=pathlib.Path)
parser.add_argument('--wine-source-directory', type=pathlib.Path)
parser.add_argument('--inventory', type=pathlib.Path,
                    default=repo / 'native/windows/test-output/game-helper-import-inventory-full.json')
args = parser.parse_args()
wine_source = resolve_wine_source(args.wine_source_directory)
inventory = json.loads(args.inventory.read_text(encoding='utf-8-sig'))
patterns = []
for version, match in enumerate(re.finditer(r'DEFINE_GUID\(CLSID_XACTEngine,([^;]+)\);', (wine_source / 'include/xact3.h').read_text())):
    numbers = [int(n, 16) for n in re.findall(r'0x[0-9a-f]+', match[1])]
    binary = struct.pack('<IHH8B', *numbers)
    text = str(uuid.UUID(bytes_le=binary))
    patterns.append((f'3.{version}', binary, text.encode(), text.encode('utf-16-le')))
images = inventory['images']
selected = [x for x in images if x.get('archive') and x['bytes'] <= 20 * 1024 * 1024 and not x['redistributable'] and
            (pathlib.PurePosixPath(x['member'].replace('\\', '/')).name.lower() in ('fna.dll', 'microsoft.xna.framework.dll') or
             (x['member'].lower().endswith('.exe') and any(name in x['game'].lower() for name in ('blossom', 'castle crashers', 'duck game', 'duckgame', 'spelunky', 'cuphead', 'sonic 4'))))]
result = dict(read_only=True, source_inventory=str(args.inventory.resolve()), wine_source=str(wine_source),
              static_import_matches=[], selected_images=[], banks=[], errors=[], uncompressed_pe_bytes_read=0,
              limits='Only listed game executables and bundled FNA/XNA frameworks are byte-scanned; bank scan reads ZIP metadata and first 40 bytes only. Absence is not proof of no runtime use.')
for entry in images:
    for imp in entry['imports'] + entry['delay_imports']:
        if 'xactengine' in imp['dll'].lower():
            result['static_import_matches'].append(dict(game=entry['game'], member=entry['member'], dll=imp['dll']))
for entry in selected:
    try:
        with zipfile.ZipFile(entry['archive']) as archive:
            data = archive.read(entry['member'])
        result['uncompressed_pe_bytes_read'] += len(data)
        folded = data.lower()
        matches = [version for version, binary, ascii_text, wide_text in patterns
                   if binary in data or ascii_text in folded or wide_text in folded]
        direct = [name for name in ('FACTCreateEngine', 'FACTAudioEngine_Initialize', 'FACTAudioEngine_CreateSoundBank', 'FACTAudioEngine_CreateStreamingWaveBank') if name.encode() in data]
        result['selected_images'].append(dict(game=entry['game'], archive=entry['archive'], member=entry['member'], bytes=len(data), xact_class_versions=matches, direct_fact_names=direct))
    except (OSError, KeyError, zipfile.BadZipFile) as error:
        result['errors'].append(dict(archive=entry['archive'], member=entry['member'], error=str(error)))
archives = {entry['archive']: entry['game'] for entry in images if entry.get('archive')}
for path, game in archives.items():
    try:
        with zipfile.ZipFile(path) as archive:
            for entry in archive.infolist():
                if entry.filename.lower().endswith(('.xwb', '.xsb', '.xgs')):
                    with archive.open(entry) as source:
                        header = source.read(40)
                    result['banks'].append(dict(game=game, archive=path, member=entry.filename, bytes=entry.file_size, header40=header.hex()))
    except (OSError, zipfile.BadZipFile) as error:
        result['errors'].append(dict(archive=path, error=str(error)))
args.output.write_text(json.dumps(result, indent=2), encoding='utf-8')
print(json.dumps(dict(selected_images=len(result['selected_images']), bytes_read=result['uncompressed_pe_bytes_read'],
                     class_matches=[x for x in result['selected_images'] if x['xact_class_versions']], banks=len(result['banks']), errors=len(result['errors']))))
