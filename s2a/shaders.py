"""Unreal's offline adapter. Other engines must supply their own adapter."""
import hashlib
import json
import subprocess
import sys
from pathlib import Path
from .bundle import shader_files


def run(command, **kwargs):
    subprocess.run([str(x) for x in command], check=True, **kwargs)


def prepare(repo, game, work, repak, workers):
    # Identity covers source content, extraction/translation logic and vendored compiler.
    digest = hashlib.sha256((game / 'manifest.json').read_bytes())
    for folder in ('tools', 'scripts'):
        for p in sorted((repo / folder).rglob('*')):
            if p.is_file() and not set(p.parts) & {'bin', 'obj', '__pycache__'} and p.suffix in ('.cs', '.csproj', '.py', '.props'):
                digest.update(p.relative_to(repo).as_posix().encode()); digest.update(p.read_bytes())
    digest.update(Path(repak).read_bytes())
    target = work / 'shaders' / digest.hexdigest()
    output = target / 'spirv'
    if (output / 'manifest.json').exists():
        shader_files(output)
        print('Reusing verified offline shaders', flush=True)
        return output
    target.mkdir(parents=True, exist_ok=True)
    assets = target / 'assets'; assets.mkdir(exist_ok=True)
    paks = sorted((game / 'RomFS').rglob('*.pak'))
    if not paks:
        return prepare_unity(repo, game, target, output, workers, digest)
    # Preserve subdirectory names. repak fails on encrypted/unsupported archives.
    for pak in paks:
        run([repak, 'unpack', '-q', '-f', '-o', assets, '-i', '**/*.uexp', '-i', '**/*.ushaderbytecode', '-i', '**/GlobalShaderCache*.bin', pak])
    globals_ = list(assets.rglob('GlobalShaderCache-SF_GLSL_SWITCH.bin'))
    if len(globals_) != 1:
        raise ValueError('Expected one Switch global shader cache')
    raw, jobs = target / 'raw', target / 'jobs'
    command = [sys.executable, repo / 'tools/extract_shaders.py', assets, globals_[0], raw]
    for archive in sorted(assets.rglob('*.ushaderbytecode')):
        command.extend(['--archive', archive])
    run(command)
    run([sys.executable, repo / 'tools/prepare_shaders.py', raw, jobs])
    translator = target / 'translator'
    run(['dotnet', 'build', repo / 'tools/shader_translate/dmsh_xlate.csproj', '-c', 'Release', '-o', translator])
    run([sys.executable, repo / 'scripts/build-offline-shaders.py', jobs / 'jobs.json', output,
         '--translator', translator / 'dmsh_xlate.dll', '--workers', workers])
    shader_files(output)
    return output


def prepare_unity(repo, game, target, output, workers, digest):
    """Unity keeps each platform's NVN programs in compressed sub-program blobs."""
    raw, jobs = target / 'raw', target / 'jobs'
    run([sys.executable, repo / 'tools/extract_unity_shaders.py', game / 'RomFS', raw])
    if not (raw / 'manifest.json').exists():
        raise ValueError('Unity shader extraction produced no manifest')
    run([sys.executable, repo / 'tools/prepare_shaders.py', raw, jobs])
    translator = target / 'translator'
    if not (translator / 'dmsh_xlate.dll').exists():
        run(['dotnet', 'build', repo / 'tools/shader_translate/dmsh_xlate.csproj', '-c', 'Release', '-o', translator])
    run([sys.executable, repo / 'scripts/build-offline-shaders.py', jobs / 'jobs.json', output,
         '--translator', translator / 'dmsh_xlate.dll', '--workers', workers])
    run([sys.executable, repo / 'tools/unity_shader_identity.py', raw, output])
    shader_files(output)
    return output
