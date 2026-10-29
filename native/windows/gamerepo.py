"""Stage a local Cryo 3.0 GameRepo manifest into a Winedows prefix.

Only copies declared archive layers and imports declared registry data. Does not
modify the repository, execute manifest commands, or apply compatibility fixes.
Requires PyYAML (`python -m pip install PyYAML`).
"""
from __future__ import annotations

import argparse
import json
import pathlib
import shutil
import subprocess
import zipfile

import yaml


def within(root: pathlib.Path, path: pathlib.Path) -> pathlib.Path:
    result = path.resolve()
    if not result.is_relative_to(root.resolve()):
        raise ValueError(f'Path escapes its permitted directory: {path}')
    return result


def extract(archive: pathlib.Path, target: pathlib.Path) -> None:
    target.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as z:
        for entry in z.infolist():
            parts = pathlib.PurePosixPath(entry.filename.replace('\\', '/'))
            if parts.is_absolute() or '..' in parts.parts or any(':' in p for p in parts.parts):
                raise ValueError(f'Unsafe archive path: {entry.filename}')
            if (entry.external_attr >> 16) & 0o170000 == 0o120000:
                raise ValueError(f'Symlink archive entries are unsupported: {entry.filename}')
            destination = within(target, target.joinpath(*parts.parts))
            if entry.is_dir():
                destination.mkdir(parents=True, exist_ok=True)
            else:
                destination.parent.mkdir(parents=True, exist_ok=True)
                with z.open(entry) as src, destination.open('wb') as dst:
                    shutil.copyfileobj(src, dst)


def registry_view(manifest: dict) -> str:
    """Select registry data placement; executable PE headers select runtime bitness."""
    architecture = manifest.get('environment', {}).get('architecture')
    if architecture not in (None, 'win32', 'win64'):
        raise ValueError(f'Unsupported registry architecture: {architecture}')
    return '32' if architecture == 'win32' else '64'


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=pathlib.Path, default=pathlib.Path(r'D:\GameRepo\main'))
    parser.add_argument('--game', required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--prefix', type=pathlib.Path, required=True)
    parser.add_argument('--wine', type=pathlib.Path, default=pathlib.Path(__file__).parent/'bin'/'wine64.exe')
    args = parser.parse_args()
    archive = within(args.repo/'archive', args.repo/'archive'/args.game/args.version)
    manifest = yaml.safe_load((archive/'manifest.yaml').read_text(encoding='utf-8-sig'))
    if manifest.get('format') != 'cryo/3.0':
        raise ValueError('Only cryo/3.0 manifests are supported')
    prefix = args.prefix.resolve()
    if prefix.exists() and any(prefix.iterdir()):
        raise ValueError('Choose a new or empty prefix; existing game state will not be overwritten')
    wine = args.wine.resolve()
    def invoke(*arguments: str) -> None:
        subprocess.run([str(wine), '--prefix', str(prefix), *arguments], check=True)
    # Validate the source/destination list before writing anything.
    layers = []
    letters = set()
    for layer in manifest.get('filesystem', []):
        if layer.get('type') != 'mount':
            raise ValueError('Only archive mount layers are supported')
        source = within(archive, archive/pathlib.Path(layer['source'].replace('\\', '/')))
        dest = pathlib.PureWindowsPath(layer['dest'])
        if len(dest.drive) != 2 or dest.drive[1] != ':' or not dest.is_absolute():
            raise ValueError(f'Expected an absolute drive destination: {dest}')
        letter = dest.drive[0].lower()
        if not 'a' <= letter <= 'z':
            raise ValueError('Invalid drive letter')
        root = prefix/('drive_c' if letter == 'c' else 'drives/'+letter)
        target = within(root, root.joinpath(*dest.parts[1:]))
        if not source.is_file() or source.suffix.lower() != '.zip':
            raise ValueError(f'Only existing ZIP payloads are supported: {source}')
        layers.append((source, target)); letters.add(letter)
    registry = [within(archive, archive/pathlib.Path(p.replace('\\', '/'))) for p in manifest.get('registry', [])]
    if not all(p.is_file() for p in registry):
        raise ValueError('Missing registry input')
    view = registry_view(manifest) if registry else None
    invoke('init')
    for source, destination in layers:
        print(f'Extracting {source.name} -> {destination}', flush=True)
        extract(source, destination)
    configured = {str(d['path'])[0].lower(): d for d in manifest.get('environment', {}).get('drives', [])}
    for letter in sorted(letters-{'c'}):
        drive = configured.get(letter, {})
        kind = {'hd': 'fixed', 'cdrom': 'cdrom', 'floppy': 'removable', 'removable': 'removable'}.get(drive.get('type', 'hd'))
        if not kind:
            raise ValueError('Unsupported drive type')
        invoke('drive', 'map', letter, str(prefix/'drives'/letter), kind, str(drive.get('label', letter.upper())))
    for file in registry:
        invoke('reg', 'import', str(file), f'/reg:{view}')
    entries = manifest.get('entrypoints', [])
    report = {'source': str(archive), 'entrypoints': entries, 'registry_view': view,
              'dependencies_not_installed': manifest.get('dependencies', []),
              'requested_environment': manifest.get('environment', {})}
    (prefix/'.winedows'/'gamerepo.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('Staged. Declared dependencies and compatibility profiles were not automatically installed.')
    for entry in entries:
        executable = entry.get('exec', entry.get('Exec'))
        print(subprocess.list2cmdline([str(wine), '--prefix', str(prefix), 'run', executable]), entry.get('args', ''))


if __name__ == '__main__':
    main()
