#!/usr/bin/env python3
"""Create a platform module manifest. Does not unpack engine resources or modify saves."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def make_profile(root, title_id, version=0, display_version=''):
    if not re.fullmatch('[0-9a-fA-F]{16}', title_id) or int(title_id, 16) == 0:
        raise ValueError('Invalid title ID')
    directory = (root / 'ExeFS').resolve()
    modules = []
    for path in sorted(directory.iterdir()):
        if not path.is_file():
            continue
        with path.open('rb') as source:
            header = source.read(256)
            if header[:4] != b'NSO0':
                continue
            if path.resolve().parent != directory or not re.fullmatch(r'rtld|main|sdk|subsdk[0-9]', path.name):
                raise ValueError('Unsupported or escaping NSO module')
            if len(header) != 256:
                raise ValueError('Truncated NSO header')
            digest = hashlib.sha256(header)
            while block := source.read(1024 * 1024):
                digest.update(block)
        modules.append(dict(name=path.name, build_id=header[64:96].hex(), sha256=digest.hexdigest()))
    if not any(m['name'] == 'main' for m in modules):
        raise ValueError('Missing main NSO')
    # These hints select optional offline optimizers. Runtime filesystem access
    # serves the original bytes; it has no Unreal or Halley extraction dependency.
    romfs = root / 'RomFS'
    engine = 'generic'
    # Cheap Unity markers first: their content directories identify the engine
    # without walking the whole tree looking for Unreal paks.
    unity_markers = ('Data/Managed', 'Data/globalgamemanagers', 'Data/StreamingAssets')
    if any((romfs / marker).exists() for marker in unity_markers):
        engine = 'unity'
    elif any(romfs.rglob('*.pak')):
        engine = 'unreal'
    else:
        for path in romfs.rglob('shaders.dat'):
            with path.open('rb') as source:
                if source.read(8) == b'HALLEYPK':
                    engine = 'halley'
                    break
    return dict(schema=1, title_id=title_id.lower(), version=version,
                display_version=display_version, modules=modules,
                resource_adapter=engine)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path, required=True, help='directory containing ExeFS and optional RomFS')
    parser.add_argument('--title-id')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    metadata_path = args.game / 'manifest.json'
    metadata = json.loads(metadata_path.read_text()) if metadata_path.is_file() else {}
    title_id = args.title_id or metadata.get('title_id', '')
    if args.title_id and metadata.get('title_id', args.title_id).lower() != args.title_id.lower():
        parser.error('Title ID differs from extracted package metadata')
    try:
        result = make_profile(args.game, title_id, metadata.get('version', 0), metadata.get('display_version', ''))
        output = args.output or args.game / 'game-build.json'
        with output.open('x') as target:
            target.write(json.dumps(result, indent=2) + '\n')
        print(f"Profile {result['title_id']}: {len(result['modules'])} modules, adapter={result['resource_adapter']}")
    except (ValueError, OSError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
