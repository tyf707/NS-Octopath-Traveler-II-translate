"""Command-line entry point; all user content stays outside the source checkout."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from . import bundle, packages, shaders
from tools.game_profile import make_profile

REPO = Path(__file__).resolve().parents[1]


def outside(path):
    path = path.expanduser().resolve()
    if path == REPO or REPO in path.parents:
        raise ValueError('ROMs, keys, work and generated APKs must be outside the source repository')
    return path


def java_major(home):
    try:
        release = (Path(home) / 'release').read_text()
        version = re.search(r'JAVA_VERSION="([^"\n]+)', release)[1]
        return int(version.split('.')[1] if version.startswith('1.') else version.split('.')[0].split('-')[0])
    except (OSError, TypeError, ValueError):
        return 0


def sdk_environment():
    env = dict(os.environ)
    if java_major(env.get('JAVA_HOME', '/missing')) < 21:
        candidates = [Path('/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home'),
                      *Path('/usr/lib/jvm').glob('*21*')]
        for candidate in candidates:
            if java_major(candidate) >= 21:
                env['JAVA_HOME'] = str(candidate); break
    sdk = env.get('ANDROID_HOME') or env.get('ANDROID_SDK_ROOT')
    if not sdk:
        for candidate in [Path.home() / 'Library/Android/sdk', Path.home() / 'Android/Sdk', Path('/opt/homebrew/share/android-commandlinetools')]:
            if candidate.is_dir():
                sdk = str(candidate); break
    if sdk:
        env['ANDROID_HOME'] = sdk
    return env


def require_android(env):
    if java_major(env.get('JAVA_HOME', '/missing')) < 21:
        raise ValueError('JDK 21+ required. Set JAVA_HOME before building.')
    sdk = Path(env.get('ANDROID_HOME', '/missing'))
    for part in ['platforms/android-36/android.jar', 'ndk/28.2.13676358', 'cmake/3.22.1']:
        if not (sdk / part).exists():
            raise ValueError('Missing Android SDK component: ' + part + '; set ANDROID_HOME and run doctor')


def profile_for(title, version):
    path = REPO / 'profiles' / (title + '.json')
    if not path.is_file():
        raise ValueError('No Android runtime profile for this title. inspect works, but packing does not imply compatibility.')
    profile = json.loads(path.read_text())
    if version not in profile['versions']:
        raise ValueError(f'Unsupported update {version}; supported versions: {profile["versions"]}. Use --version explicitly.')
    return profile


def doctor(args):
    env = sdk_environment(); missing = []
    for command in ['git', 'make', 'cmake', 'dotnet', 'spirv-val', 'cargo']:
        found = shutil.which(command)
        print(f'{command}: {found or "MISSING"}')
        if not found: missing.append(command)
    try:
        import lz4.block
        print('Python lz4: OK')
    except ImportError:
        print('Python lz4: MISSING'); missing.append('Python lz4')
    sdk = Path(env.get('ANDROID_HOME', '/missing'))
    for part in ['platforms/android-36/android.jar', 'ndk/28.2.13676358', 'cmake/3.22.1']:
        ok = (sdk / part).exists(); print(f'Android {part}: {"OK" if ok else "MISSING"}')
        if not ok: missing.append(part)
    java = Path(env.get('JAVA_HOME', '/missing')) / 'bin/java'
    if not java.exists() and not shutil.which('java'): missing.append('JDK 21')
    major = java_major(env.get('JAVA_HOME', '/missing'))
    print(f'JDK: {major} ({java})')
    if major < 21: missing.append('JDK 21')
    return 1 if missing else 0


def build(args):
    source, keys, work, out = map(outside, [args.source, args.keys, args.work, args.output])
    require_android(sdk_environment())
    if not source.is_dir() or not keys.is_file():
        raise ValueError('Input directory or keys file does not exist')
    work.mkdir(parents=True, exist_ok=True); out.mkdir(parents=True, exist_ok=True)
    # Extraction and generated Gradle assets are a single transaction per work directory.
    with (work / '.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        hactool = args.hactool or work / 'tools/hactool/hactool'
        ht = packages.Hactool(hactool, keys)
        with tempfile.TemporaryDirectory(dir=work, prefix='metadata-') as scratch:
            records = packages.inventory(source, ht, Path(scratch))
        selection = packages.select(records, args.title_id, args.version, not args.no_dlc)
        base, selected, dlcs = selection
        title = base['title_id']; profile = profile_for(title, selected['version'])
        print(f'{profile["name"]}: update {selected["version"]}, DLC {list(dlcs)}', flush=True)
        game = work / 'games' / packages.selection_id(selection)
        # Do not trust partial extraction directories; only our committed manifest is reusable.
        if game.exists():
            metadata = json.loads((game / 'manifest.json').read_text())
            if metadata['id'] != packages.selection_id(selection):
                raise ValueError('Cached extraction identity mismatch; remove that work/games directory')
        else:
            print('Decrypting and verifying base + update + DLC', flush=True)
            metadata = packages.extract(selection, ht, game)
        game_profile = make_profile(game, title, metadata['version'], metadata['display_version'])
        main = next(m for m in game_profile['modules'] if m['name'] == 'main')
        if main['build_id'] not in profile['main_build_ids']:
            raise ValueError('Unknown main build ID; refusing game-specific assumptions')
        game_profile.update(name=metadata['name'], dlc=metadata['dlc'])
        # 按标题的补丁由 profile 声明，运行时不再按标题猜。
        if 'patches' in profile:
            game_profile['patches'] = profile['patches']
        (game / 'game-build.json').write_text(json.dumps(game_profile, indent=2) + '\n')
        if args.shader_cache:
            shader_root = outside(args.shader_cache)
            bundle.shader_files(shader_root)
        else:
            shader_root = shaders.prepare(REPO, game, work, args.repak or work / 'tools/repak/target/release/repak', args.workers)
        app_id = 'org.switch2android.g' + title
        product = out / f'{title}-v{metadata["version"]}'
        if product.exists():
            raise FileExistsError(f'Output exists: {product}; select another output directory')
        with tempfile.TemporaryDirectory(prefix='.build-', dir=out) as staging:
            stage = Path(staging)
            print('Building and hashing local content bundle', flush=True)
            content = stage / 'game.s2a'
            launch = bundle.build(game, shader_root, content, game_profile)
            assets = work / 'generated-assets'; shutil.rmtree(assets, ignore_errors=True); assets.mkdir()
            launch['embedded'] = args.embed == 'always' or (args.embed == 'auto' and content.stat().st_size <= 1024**3)
            if launch['embedded'] and content.stat().st_size > 1024**3:
                raise ValueError('Embedded payload exceeds the conservative 1 GiB APK limit; use --embed never')
            if launch['embedded']:
                shutil.copyfile(content, assets / 'game.s2a')
            (assets / 'launch.json').write_text(json.dumps(launch))
            env = sdk_environment()
            print('Building non-debuggable Release APK', flush=True)
            shaders.run([REPO / 'gradlew', '--project-cache-dir', work / 'gradle-cache', ':app:assembleRelease', ':app:lintRelease',
                         '-Ps2aAssets=' + str(assets), '-Ps2aBuildDir=' + str(work / 'android-build'),
                         '-Ps2aAppId=' + app_id, '-Ps2aName=' + profile['name'], '-Ps2aVersionCode=' + str(metadata['version'] + 1)], cwd=REPO, env=env)
            apk = work / 'android-build/app/outputs/apk/release/app-release.apk'
            shutil.copyfile(apk, stage / 'game.apk')
            if launch['embedded']:
                content.unlink()
            launch.update(application_id=app_id, version=metadata['version'], apk_sha256=bundle.sha256(stage / 'game.apk'))
            (stage / 'package.json').write_text(json.dumps(launch, indent=2) + '\n')
            (stage / 'INSTALL.txt').write_text('Private local output: contains your game data. Do not redistribute.\n\nInstall game.apk, open it, then select game.s2a when asked (external mode).\nOr use: python3 switch2android.py install THIS_DIRECTORY --serial ADB_SERIAL\nKeep the same Android signing key for future updates. Saves remain in application-private storage.\n')
            stage.rename(product)
        print(f'Built: {product}', flush=True)


def inspect(args):
    source, keys = outside(args.source), outside(args.keys)
    with tempfile.TemporaryDirectory() as temp:
        ht = packages.Hactool(args.hactool, keys)
        records = packages.inventory(source, ht, Path(temp))
        print(json.dumps([packages.public_record(r) for r in records], indent=2))


def install(args):
    product = outside(args.package); meta = json.loads((product / 'package.json').read_text())
    app_id = meta['application_id']
    if not re.fullmatch(r'org\.switch2android\.g[0-9a-f]{16}', app_id):
        raise ValueError('Invalid application ID')
    if bundle.sha256(product / 'game.apk') != meta['apk_sha256']:
        raise ValueError('APK checksum mismatch')
    adb = [args.adb, '-s', args.serial]
    identity = meta['bundle_id']
    if not re.fullmatch('[0-9a-f]{64}', identity):
        raise ValueError('Invalid resource bundle identity')
    remote = f'/sdcard/Android/data/{app_id}/files'
    already_imported = subprocess.run([*adb, 'shell', 'head', '-c', '65',
        remote + '/content/' + identity + '/.complete'], capture_output=True, text=True)
    reuse = already_imported.returncode == 0 and already_imported.stdout == identity
    if not meta['embedded'] and not reuse:
        print('Verifying resource bundle before installation', flush=True)
        bundle.verify(product / 'game.s2a', identity)
    shaders.run([*adb, 'install', '-r', product / 'game.apk'])
    if reuse:
        print('Reusing already imported content; no resource transfer needed', flush=True)
    elif not meta['embedded']:
        shaders.run([*adb, 'shell', 'mkdir', '-p', remote])
        shaders.run([*adb, 'push', product / 'game.s2a', remote + '/incoming.s2a'])
    shaders.run([*adb, 'shell', 'am', 'start', '-n', app_id + '/dev.ot2.port.LauncherActivity'])


def main():
    parser = argparse.ArgumentParser(description='Build local Android game packages from your own NSPs and keys.')
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('doctor', help='check local build prerequisites')
    for name in ('build', 'inspect'):
        p = commands.add_parser(name)
        p.add_argument('source', type=Path); p.add_argument('-k', '--keys', required=True, type=Path)
        p.add_argument('--hactool', type=Path, required=name == 'inspect')
        if name == 'inspect': continue
        p.add_argument('--work', required=True, type=Path); p.add_argument('-o', '--output', required=True, type=Path)
        p.add_argument('--title-id', type=lambda s: s.lower()); p.add_argument('--version', type=int)
        p.add_argument('--no-dlc', action='store_true'); p.add_argument('--repak', type=Path)
        p.add_argument('--shader-cache', type=Path, help='advanced: use an already validated offline shader set')
        p.add_argument('--workers', type=int, default=4)
        p.add_argument('--embed', choices=['auto', 'always', 'never'], default='auto')
    p = commands.add_parser('install'); p.add_argument('package', type=Path)
    p.add_argument('--serial', required=True); p.add_argument('--adb', default='adb')
    p = commands.add_parser('verify'); p.add_argument('bundle', type=Path)
    args = parser.parse_args()
    try:
        if args.command == 'verify': print(bundle.verify(args.bundle)); return 0
        result = {'build': build, 'inspect': inspect, 'install': install, 'doctor': doctor}[args.command](args)
        return result or 0
    except (ValueError, OSError, RuntimeError, KeyError, subprocess.CalledProcessError) as error:
        print(f'Error: {error}', file=sys.stderr); return 1


if __name__ == '__main__':
    sys.exit(main())
