#!/usr/bin/env python3
"""From your game APK to a folder you can play from: one command.

    python build/from_apk.py --apk GAME.apk --toolchain TOOLCHAIN_DIR --clang CLANG18 \
        --deps DEPS_DIR --launcher reincomp.exe --server bunker-server.exe --out OUT_DIR

It recompiles the game's two libraries on this machine and builds the game
around them. Nothing it makes from the game leaves OUT_DIR.

  1. checks the APK and takes libunity.so and libil2cpp.so out of it
  2. lifts both from the trace heads in game/ (recompiler/lift_all.py): the long step
  3. collects the objects into a pack (build/make_pack.py)
  4. builds the game executable (build/build.py)
  5. assembles the release folder (build/make_release.py)

TOOLCHAIN_DIR holds remill-lift, aarch64.bc and poll-instrument: a release's
toolchain zip, or the output of recompiler/windows/build_toolchain.py.
CLANG18 is clang 18 (clang.exe from LLVM 18.1.8 on Windows). It also needs
MinGW-w64 g++ (--cxx, or on PATH). Each step keeps its work in OUT_DIR/work
and is skipped when already done, so an interrupted run continues.
"""
import argparse, hashlib, json, subprocess, sys, time, zipfile
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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--apk', type=Path, required=True)
    ap.add_argument('--toolchain', type=Path, required=True)
    ap.add_argument('--clang', required=True)
    ap.add_argument('--cxx', help='MinGW-w64 g++ (default: the one on PATH)')
    ap.add_argument('--deps', type=Path, help='the folder of files that ship beside the game (see make_release.py)')
    ap.add_argument('--launcher', type=Path)
    ap.add_argument('--server', type=Path)
    ap.add_argument('--unpinned-server', action='store_true', help='accept a server build other than the pinned release')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--jobs', type=int, default=8)
    a = ap.parse_args()
    work = a.out.resolve() / 'work'
    work.mkdir(parents=True, exist_ok=True)

    print('== Checking the APK', flush=True)
    digest = hashlib.sha256()
    with a.apk.open('rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    if digest.hexdigest() != APK:
        sys.exit('This is not the APK this port is made from: the English 3.7.1 (152) arm64-v8a build.')
    native = work / 'native'
    native.mkdir(exist_ok=True)
    with zipfile.ZipFile(a.apk) as z:
        for _, name, _, _ in LIBRARIES:
            if not (native / name).exists():
                (native / name).write_bytes(z.read('lib/arm64-v8a/' + name))

    for key, name, bias, heads in LIBRARIES:
        if not (work / key / 'result.json').exists():
            step(f'Recompiling {name} (the long step)', [
                sys.executable, ROOT / 'recompiler/lift_all.py', '--lib', native / name, '--bias', bias, '--heads', ROOT / heads,
                '--patches', ROOT / 'game/patches.json', '--toolchain', a.toolchain, '--poll', a.toolchain / 'poll-instrument',
                '--clang', a.clang, '--out', work / key, '--jobs', a.jobs])
    pack = work / 'pack'
    if not (pack / 'pack.json').exists():
        step('Collecting the pack', [sys.executable, ROOT / 'build/make_pack.py', '--out', pack, *[work / key for key, _, _, _ in LIBRARIES]])
    build = [sys.executable, ROOT / 'build/build.py', '--pack', pack, '--out', work / 'build', '--jobs', a.jobs]
    step('Building the game', build + (['--cxx', a.cxx] if a.cxx else []))
    exe = work / 'build/bunker-game-bootstrap.exe'
    if not (a.deps and a.launcher and a.server):
        print(f'The game executable is {exe}\nGive --deps, --launcher and --server to assemble the folder to play from.')
        return
    folder = a.out.resolve() / 'reincomp'
    if folder.exists():
        sys.exit(f'{folder} already exists: move it away first.')
    step('Assembling the folder', [sys.executable, ROOT / 'build/make_release.py', '--exe', exe, '--launcher', a.launcher,
                                   '--deps', a.deps, '--server', a.server, '--out', folder,
                                   *(['--unpinned-server'] if a.unpinned_server else [])])
    print(f'\nDone. Put your game APK, {json.dumps("20240404193219.bin.e")} and the resource dump in\n  {folder}\nand run reincomp.exe.')


if __name__ == '__main__':
    main()
