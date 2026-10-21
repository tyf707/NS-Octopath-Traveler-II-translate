#!/usr/bin/env python3
"""Recover NVN shaders from locally unpacked Unity bundles.

Unity stores each platform's compiled programs in a compressed sub-program blob.
On Switch those programs are NVN shader binaries: a control block preceded by its
own size and the code size, followed by the Maxwell code. This mirrors
extract_shaders.py's Unreal framing, only the sizes precede the control block.
"""
import argparse
import hashlib
import json
import lz4.block
import struct
import pathlib
import sys

MAGIC = bytes.fromhex('34127698010000001000000020010000')
MAX_CODE = 16 * 1024 * 1024
CONTROL_MIN = 0x880
CONTROL_MAX = 0x10000


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def unity_source(header, file_size):
    if header[:7] in (b'UnityFS', b'UnityWe', b'UnityRa'):
        return True
    if len(header) < 20:
        return False
    metadata, declared, version, offset = struct.unpack_from('>IIII', header)
    if version >= 22:
        if len(header) < 48:
            return False
        metadata, declared, offset = struct.unpack_from('>IQQ', header, 20)
    return 7 <= version <= 30 and declared == file_size and 0 < metadata < declared and 20 <= offset <= declared


def carve(data):
    """Yield (control, code, assembly) for every well-formed NVN binary."""
    cursor = 0
    while (at := data.find(MAGIC, cursor)) >= 0:
        cursor = at + len(MAGIC)
        if at < 8:
            continue
        control_size, code_size = u32(data, at - 8), u32(data, at - 4)
        if not (CONTROL_MIN <= control_size <= CONTROL_MAX and 0 < code_size <= MAX_CODE):
            continue
        end = at + control_size + code_size
        if end > len(data):
            continue
        control = data[at:at + control_size]
        c1, size, entry = (u32(control, x) for x in (0x700, 0x704, 0x708))
        stage = u32(control, 0x714)
        if size != code_size or not (0 <= entry < c1 <= size) or stage > 5:
            continue
        code = data[at + control_size:end]
        assembly = control[CONTROL_MIN:].split(b'\0', 1)[0]
        yield control, code, assembly


def compute_programs(shader):
    # Unity's ComputeShader stores NVN binaries in kernel code arrays rather
    # than Shader.compressedBlob. Support both flat and per-variant layouts.
    groups = [shader, *(getattr(shader, 'variants', None) or [])]
    for group in groups:
        for kernel in getattr(group, 'kernels', None) or []:
            yield from carve(bytes(kernel.code or []))


def shaders_of(path):
    import UnityPy
    env = UnityPy.load(str(path))
    for obj in env.objects:
        if obj.type.name == 'ComputeShader':
            shader = obj.read()
            for control, code, assembly in compute_programs(shader):
                yield shader.m_Name, control, code, assembly
            continue
        if obj.type.name != 'Shader':
            continue
        shader = obj.read()
        blob = bytes(shader.compressedBlob or b'')
        if not blob:
            continue
        compressed = [x for sub in (shader.compressedLengths or []) for x in sub]
        expanded = [x for sub in (shader.decompressedLengths or []) for x in sub]
        offset = 0
        parts = []
        for c, u in zip(compressed, expanded):
            try:
                parts.append(lz4.block.decompress(blob[offset:offset + c], uncompressed_size=u))
            except lz4.block.LZ4BlockError:
                break
            offset += c
        else:
            name = shader.m_ParsedForm.m_Name if shader.m_ParsedForm else '?'
            for control, code, assembly in carve(b''.join(parts)):
                yield name, control, code, assembly


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('romfs', type=pathlib.Path)
    parser.add_argument('output', type=pathlib.Path)
    parser.add_argument('--glob', default='*')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    manifest, files = {}, 0
    for path in sorted(p for p in args.romfs.rglob(args.glob) if p.is_file()):
        try:
            if path.stat().st_size > 128 * 1024 * 1024:
                continue
            with path.open('rb') as f:
                head = f.read(48)
            # Only Unity bundles and serialized files can hold Shader objects.
            # Built-in resources are serialized files without an extension.
            if not unity_source(head, path.stat().st_size):
                continue
            found = list(shaders_of(path))
        except Exception:
            continue
        if not found:
            continue
        files += 1
        for name, control, code, assembly in found:
            key = 14695981039346656037
            for byte in control:
                key = ((key ^ byte) * 1099511628211) & ((1 << 64) - 1)
            key = f'{key:016x}'
            digest = hashlib.sha256(control + code).hexdigest()
            if key in manifest:
                if manifest[key]['sha256'] != digest:
                    raise SystemExit(f'Shader control key collision: {key}')
                manifest[key]['occurrences'] += 1
                continue
            manifest[key] = dict(source=str(path), name=name, sha256=digest,
                                 code_size=len(code), assembly=len(assembly), occurrences=1)
            for suffix, content in (('control', control), ('code', code), ('asm', assembly)):
                (args.output / f'{key}.{suffix}').write_bytes(content)
        if files % 50 == 0:
            print(f'{files} bundles, {len(manifest)} shaders', flush=True)
    report = dict(bundles=files, unique_shaders=len(manifest), shaders=manifest)
    (args.output / 'manifest.json').write_text(json.dumps(report, indent=2))
    with_asm = sum(1 for v in manifest.values() if v['assembly'])
    print(json.dumps(dict(bundles=files, unique_shaders=len(manifest), with_assembly=with_asm), indent=2))


if __name__ == '__main__':
    main()
