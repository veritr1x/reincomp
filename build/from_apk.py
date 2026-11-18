#!/usr/bin/env python3
"""From your game APK to a folder you can play from: one command.

    python build/from_apk.py --apk GAME.apk --clang CLANG18 --out OUT_DIR

It recompiles the game's two libraries on this machine and builds the game
around them. Nothing it makes from the game leaves OUT_DIR.

  1. checks the APK and takes libunity.so and libil2cpp.so out of it
  2. downloads what it was not given: the toolchain, the files that ship
     beside the game, the launcher and the server
  3. lifts both libraries from the trace heads in game/ (recompiler/lift_all.py): the long step
  4. collects the objects into a pack (build/make_pack.py)
  5. builds the game executable (build/build.py)
  6. assembles the folder to play from (build/make_release.py)

CLANG18 is clang 18 (clang.exe from LLVM 18.1.8 on Windows). It also needs
MinGW-w64 g++ (--cxx, or on PATH). Each step keeps its work in OUT_DIR/work
and is skipped when already done, so an interrupted run continues.

The downloads are this project's release files (release.lock.json) and
Bunker's server (bunker.lock.json), each checked against its pinned hash.
--toolchain, --deps, --launcher and --server take your own instead, and
--no-download stops at the game executable when one of them is missing.
"""
import argparse, hashlib, json, shutil, subprocess, sys, time, urllib.request, zipfile
from pathlib import Path

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
    found = {name: (entry['url'], entry['sha256']) for name, entry in json.loads((ROOT / 'release.lock.json').read_text()).items()}
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
    ap.add_argument('--clang', required=True)
    ap.add_argument('--cxx', help='MinGW-w64 g++ (default: the one on PATH)')
    ap.add_argument('--toolchain', type=Path, help='your own folder with remill-lift, aarch64.bc and poll-instrument')
    ap.add_argument('--deps', type=Path, help='your own folder of files that ship beside the game (see make_release.py)')
    ap.add_argument('--launcher', type=Path, help='your own reincomp.exe')
    ap.add_argument('--server', type=Path, help='your own bunker-server.exe')
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
    native = work / 'native'
    native.mkdir(exist_ok=True)
    with zipfile.ZipFile(a.apk) as z:
        for _, name, _, _ in LIBRARIES:
            if not (native / name).exists():
                (native / name).write_bytes(z.read('lib/arm64-v8a/' + name))

    def part(name):
        given = getattr(a, name)
        return given if given or a.no_download else download(work / 'downloads', name)

    toolchain = part('toolchain')
    if not toolchain:
        sys.exit('Give --toolchain, or leave --no-download out.')
    for key, name, bias, heads in LIBRARIES:
        if not (work / key / 'result.json').exists():
            step(f'Recompiling {name} (the long step)', [
                sys.executable, ROOT / 'recompiler/lift_all.py', '--lib', native / name, '--bias', bias, '--heads', ROOT / heads,
                '--patches', ROOT / 'game/patches.json', '--toolchain', toolchain, '--poll', toolchain / 'poll-instrument',
                '--clang', a.clang, '--out', work / key, '--jobs', a.jobs])
    pack = work / 'pack'
    if not (pack / 'pack.json').exists():
        step('Collecting the pack', [sys.executable, ROOT / 'build/make_pack.py', '--out', pack, *[work / key for key, _, _, _ in LIBRARIES]])
    build = [sys.executable, ROOT / 'build/build.py', '--pack', pack, '--out', work / 'build', '--jobs', a.jobs]
    step('Building the game', build + (['--cxx', a.cxx] if a.cxx else []))
    exe = work / 'build/bunker-game-bootstrap.exe'
    deps, launcher, server = part('deps'), part('launcher'), part('server')
    if not (deps and launcher and server):
        print(f'The game executable is {exe}\nGive --deps, --launcher and --server to assemble the folder to play from.')
        return
    folder = a.out.resolve() / 'reincomp'
    if folder.exists():
        sys.exit(f'{folder} already exists: move it away first.')
    step('Assembling the folder', [sys.executable, ROOT / 'build/make_release.py', '--exe', exe, '--launcher', launcher,
                                   '--deps', deps, '--server', server, '--out', folder,
                                   *(['--unpinned-server'] if a.unpinned_server else [])])
    print(f'\nDone. Put your game APK, {json.dumps("20240404193219.bin.e")} and the resource dump in\n  {folder}\nand run reincomp.exe.')


if __name__ == '__main__':
    main()
