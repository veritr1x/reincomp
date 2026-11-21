#!/usr/bin/env python3
"""Build the native Nier Reincarnation Windows player from your three game files.

    python build/from_apk.py --apk GAME.apk --master 20240404193219.bin.e --assets resource_dump_android.7z --out BUILD

The same command accepts a ZIP resource dump. It builds the launcher and native
player, applies Bunker's game and master patches, and imports the assets before
publishing BUILD/reincomp. Install the pinned Unity Windows 64-bit IL2CPP player,
LLVM 18, MinGW-w64 and Go. Override their locations with the options below.
"""
import argparse, hashlib, json, os, shutil, subprocess, sys, time, urllib.request, zipfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import native_release

ROOT = Path(__file__).resolve().parent.parent
APK = '9ded0797ee607b6892b531ba3a02f4c4510774f5fe098ccada78ccff881c9209'
LIBRARIES = (('unity', 'libunity.so', '0x70000000', 'game/heads-unity.txt'),
             ('il2cpp', 'libil2cpp.so', '0x80000000', 'game/heads-il2cpp.txt'))


def step(title, command):
    print(f'== {title}', flush=True)
    started = time.time()
    if subprocess.run([str(x) for x in command]).returncode:
        sys.exit(f'{title}: failed')
    print(f'   {time.time() - started:.0f}s', flush=True)


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def pins():
    """What can be downloaded: name -> (url, sha256)."""
    lock = json.loads((ROOT / 'release.lock.json').read_text())
    found = {name: (entry['url'], entry['sha256']) for name, entry in lock.items() if 'url' in entry}
    bunker = json.loads((ROOT / 'bunker.lock.json').read_text())['bunker']
    server = bunker['server']['windows-x86_64']
    found['server'] = (f'{bunker["repository"]}/releases/download/{bunker["release"]}/{server["file"]}', server['sha256'])
    return found


def download(folder, name):
    """The pinned file for NAME, in FOLDER; a zip is unpacked into a folder of that name."""
    url, pin = pins()[name]
    target = folder / url.rsplit('/', 1)[1]
    if not target.exists() or sha256(target) != pin:
        print(f'== Downloading {target.name}', flush=True)
        folder.mkdir(parents=True, exist_ok=True)
        part = target.with_name(target.name + '.part')
        with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'reincomp-build'})) as response, part.open('wb') as f:
            shutil.copyfileobj(response, f)
        if sha256(part) != pin:
            part.unlink()
            sys.exit(f'{target.name}: the download is not the file pinned for {name}')
        part.replace(target)
    if target.suffix != '.zip':
        return target
    unpacked = folder / name
    if not (unpacked / '.unpacked').exists() or (unpacked / '.unpacked').read_text() != pin:
        shutil.rmtree(unpacked, ignore_errors=True)
        with zipfile.ZipFile(target) as z:
            z.extractall(unpacked)
        (unpacked / '.unpacked').write_text(pin)
    return unpacked


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--apk', type=Path, required=True)
    ap.add_argument('--master', type=Path, required=True)
    ap.add_argument('--assets', type=Path, required=True, help='original resource dump (.7z or .zip)')
    ap.add_argument('--unity-player', type=Path, help='Unity 2019.4.29f1 win64_nondevelopment_il2cpp folder')
    ap.add_argument('--unity-symbols', type=Path, help='matching UnityPlayer Windows IL2CPP PDB, if installed separately')
    ap.add_argument('--go', help='Go executable')
    ap.add_argument('--bunker', type=Path, help='prepared Bunker source at the pinned release')
    ap.add_argument('--notices', type=Path, help='use a prepared Bunker notice pack')
    ap.add_argument('--fsr-runtime', type=Path, help='local copy of the pinned AMD FSR 3.1 Vulkan DLL')
    ap.add_argument('--clang', default=shutil.which('clang'), help='LLVM 18 clang executable')
    ap.add_argument('--cxx', help='MinGW-w64 g++ (default: the one on PATH)')
    ap.add_argument('--toolchain', type=Path, help='your own folder with remill-lift, aarch64.bc and poll-instrument')
    ap.add_argument('--deps', type=Path, help='your own folder of files that ship beside the game (see make_release.py)')
    ap.add_argument('--launcher', type=Path, help='your own reincomp.exe')
    ap.add_argument('--server', type=Path, help='your own bunker-server.exe')
    ap.add_argument('--pods', type=Path, help='your own folder from build/make_pods.py, which serves the Bunker shown with Ctrl+B')
    ap.add_argument('--unpinned-server', action='store_true', help='accept a server build other than the pinned release')
    ap.add_argument('--no-download', action='store_true', help='download nothing; use only what is given')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--jobs', type=int, default=8)
    a = ap.parse_args()
    work = a.out.resolve() / 'work'
    work.mkdir(parents=True, exist_ok=True)

    print('== Checking the APK', flush=True)
    if sha256(a.apk) != APK:
        sys.exit('This is not the APK this port is made from: the English 3.7.1 (152) arm64-v8a build.')
    for option in ('apk','master','assets'):
        setattr(a,option,getattr(a,option).resolve(strict=True))
    if native_release.digest(a.master) != native_release.MASTER_HASH:
        ap.error('The master data must be the original Global 20240404193219.bin.e')
    if not a.clang:
        ap.error('Install LLVM 18 or pass --clang')
    clang = Path(shutil.which(a.clang) or a.clang).resolve(strict=True)
    if a.unity_player is None:
        editor = Path(os.environ.get('ProgramFiles','C:/Program Files'))/'Unity/Hub/Editor/2019.4.29f1'
        a.unity_player = editor/'Editor/Data/PlaybackEngines/WindowsStandaloneSupport/Variations/win64_nondevelopment_il2cpp'
    sys.path.insert(0, str(native_release.NATIVE))
    from generate_abi import generate
    abi = work/'generated-abi'
    generate(a.apk, a.unity_player.resolve(), clang.parent, abi, a.unity_symbols)
    cxx = Path(a.cxx or shutil.which('g++') or 'C:/mingw64/bin/g++.exe').resolve(strict=True)
    def part(name):
        explicit = getattr(a, name, None)
        if explicit:
            return Path(explicit).resolve(strict=True)
        if a.no_download:
            ap.error('--no-download requires --'+name)
        return download(work/'downloads',name)
    deps, pods, server = (part(k) for k in ('deps','pods','server'))
    fsr_name = 'amd_fidelityfx_vk.dll'
    cached_fsr = work/'downloads'/fsr_name
    if not a.fsr_runtime and a.no_download:
        a.fsr_runtime = next((path for path in (deps/fsr_name, cached_fsr) if path.is_file()), None)
        if not a.fsr_runtime:
            ap.error('--no-download requires the pinned --fsr-runtime or a cached copy in --deps')
    a.fsr_runtime = part('fsr_runtime')
    if sha256(a.fsr_runtime) != pins()['fsr_runtime'][1]:
        ap.error('The FidelityFX runtime differs from the revision in release.lock.json')
    if a.toolchain or a.no_download:
        toolchain = part('toolchain')
    else:
        # The release 0.1.0 tools predate the desktop player's instruction
        # semantics, direct calls and per-thread polling.
        builder = ROOT/'recompiler/windows/build_toolchain.py'
        revision = hashlib.sha256()
        for source in (clang, builder, ROOT/'recompiler/poll_instrument.cpp',
                       *sorted((ROOT/'recompiler/remill').glob('*.patch'))):
            revision.update(bytes.fromhex(sha256(source)))
        toolchain = work/('toolchain-'+revision.hexdigest()[:20])
        if not (toolchain/'built').is_file():
            step('Building the current recompiler toolchain',
                 [sys.executable, builder, '--llvm', clang.parent.parent,
                  '--work', work/'toolchain-source', '--out', toolchain])
            (toolchain/'built').touch()
    if not a.unpinned_server and sha256(server)!=pins()['server'][1]:
        ap.error('Bunker server hash differs from bunker.lock.json')
    pin = json.loads((ROOT/'bunker.lock.json').read_text())['bunker']
    bunker = a.bunker or work/'bunker'
    if not bunker.is_dir():
        if a.no_download or a.bunker:
            ap.error('Supply the pinned Bunker checkout with --bunker')
        step('Fetching Bunker source', ['git','clone','--branch',pin['release'],'--depth','1',pin['repository'],bunker])
    bunker = bunker.resolve()
    if 'v'+(bunker/'VERSION').read_text().strip()!=pin['release']:
        ap.error('The Bunker checkout must match bunker.lock.json')
    folder = a.out.resolve()/'reincomp'
    if folder.exists():
        ap.error(f'{folder} already exists. Use a new output directory to preserve that installation.')
    stage_dir = work/'release-staging'
    stage_dir.mkdir(exist_ok=True)
    patched = native_release.prepare(a,stage_dir,work,server,deps,pods,bunker,cxx)
    # Qualify the lifted object cache with the input bytes, patches, heads and tools.
    stamp = hashlib.sha256()
    for item in (patched, ROOT/'game/patches.json', *(ROOT/e[3] for e in LIBRARIES), clang,
                 toolchain/'remill-lift.exe',toolchain/'poll-instrument.exe',toolchain/'aarch64.bc',
                 ROOT/'recompiler/lift_all.py'):
        stamp.update(bytes.fromhex(sha256(item)))
    lifted = work/('lift-'+stamp.hexdigest()[:20])
    for key,name,bias,heads in LIBRARIES:
        source = patched if key=='il2cpp' else stage_dir/'data/native'/name
        if not (lifted/key/'result.json').exists():
            step(f'Recompiling {name}', [sys.executable, ROOT/'recompiler/lift_all.py','--lib',source,'--bias',bias,
                 '--heads',ROOT/heads,'--patches',ROOT/'game/patches.json','--toolchain',toolchain,
                 '--poll',toolchain/'poll-instrument','--poll-flags=--speed --inline-poll=teb --direct-calls',
                 '--clang',clang,'--out',lifted/key,'--jobs',a.jobs])
    pack=lifted/'pack'
    if not (pack/'pack.json').exists():
        step('Collecting the pack',[sys.executable,ROOT/'build/make_pack.py','--out',pack,*(lifted/k for k,*_ in LIBRARIES)])
    spring_builder = ROOT/'recompiler/tools/build_spring_group.py'
    spring_version = hashlib.sha256((sha256(spring_builder)+sha256(abi/'spring_group.entries')).encode())
    spring = lifted/('spring-'+spring_version.hexdigest()[:16])
    if not (spring/'spring.rsp').is_file():
        step('Compiling the character spring animation group',
             [sys.executable,spring_builder,'--image',lifted/'il2cpp/image.bin','--heads',ROOT/LIBRARIES[1][3],
              '--lifter',toolchain/'remill-lift.exe','--semantics',toolchain,'--instrumenter',toolchain/'poll-instrument.exe',
              '--clang',clang,'--output',spring,'--entries',abi/'spring_group.entries'])
    step('Compiling the Windows host',[sys.executable,ROOT/'build/build.py','--pack',pack,'--out',work/'build',
                                      '--jobs',a.jobs,'--cxx',cxx,'--spring-group',spring,'--link-plan'])
    sys.path.insert(0,str(native_release.NATIVE))
    from compile_player import compile_player
    compile_player(work/'native-player',stage_dir/'game',cxx,clang,toolchain,stage_dir/'data/native/libil2cpp.so',bunker,abi)
    stage_dir.rename(folder)
    print(f'Built {folder / "reincomp.exe"}',flush=True)


if __name__ == '__main__':
    main()
