"""Add verified Unity NVN control aliases to an existing offline shader set."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

ALGORITHM = 'unity-nvn-control-code-v1'


def identity(control, code):
    if len(control) < 0x880 or struct.unpack_from('<I', control, 0x704)[0] != len(code):
        raise ValueError('Invalid Unity shader control/code size')
    stable = bytearray(control[:0x880])
    # Unity rewrites this identifier when instantiating an NVN program. Include
    # the actual machine code so this exception cannot alias different code.
    stable[0x7d0:0x7d8] = bytes(8)
    payload = bytes(stable) + code
    value = 14695981039346656037
    for byte in payload:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f'{value:016x}', hashlib.sha256(payload).hexdigest()


def add_aliases(raw, cache):
    manifest_path = cache / 'manifest.json'
    manifest = json.loads(manifest_path.read_text())
    supplied = manifest.pop('id')
    encode = lambda value: json.dumps(value, sort_keys=True).encode()
    if hashlib.sha256(encode(manifest)).hexdigest() != supplied:
        raise ValueError('Invalid source shader manifest')
    aliases, signatures = {}, {}
    for path in sorted(raw.glob('*.control')):
        key = path.stem
        if key not in manifest['shaders']:
            raise ValueError(f'Untranslated Unity shader: {key}')
        alias, digest = identity(path.read_bytes(), path.with_suffix('.code').read_bytes())
        reflection = json.loads((cache / f'{key}.res.json').read_text())
        if reflection.pop('key') != key or reflection.get('notes'):
            raise ValueError(f'Invalid reflection: {key}')
        spv_digest = hashlib.sha256((cache / f'{key}.spv').read_bytes()).hexdigest()
        expected = manifest['shaders'][key]
        if spv_digest != expected['spv'] or hashlib.sha256((cache / f'{key}.res.json').read_bytes()).hexdigest() != expected['reflection']:
            raise ValueError(f'Unverified shader artifacts: {key}')
        signature = (digest, spv_digest, encode(reflection))
        if alias in signatures and signatures[alias] != signature:
            raise ValueError(f'Ambiguous Unity shader alias: {alias}')
        signatures[alias] = signature
        aliases.setdefault(alias, key)
    if not aliases:
        raise ValueError('No Unity shader aliases')
    manifest['control_aliases'] = dict(algorithm=ALGORITHM, keys=aliases)
    manifest['id'] = hashlib.sha256(encode(manifest)).hexdigest()
    temporary = manifest_path.with_suffix('.json.tmp')
    temporary.write_text(json.dumps(manifest, indent=2))
    temporary.replace(manifest_path)
    return len(aliases), manifest['id']


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('raw', type=Path)
    parser.add_argument('cache', type=Path)
    args = parser.parse_args()
    count, cache_id = add_aliases(args.raw, args.cache)
    print(f'Verified {count} Unity shader aliases; cache {cache_id}')
