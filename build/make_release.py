#!/usr/bin/env python3
"""Assemble the folder players unzip: launcher, game, server and licenses.

    python build/make_release.py --exe EXE --launcher EXE --deps DIR --server EXE --out DIR

DEPS holds what ships beside the game executable: angle/ (libEGL.dll and the
rest), FirebaseCppApp-9_6_0.dll, grpc_csharp_ext.x64.dll and licenses/.
SERVER is Bunker's bunker-server for Windows; its hash must be the one pinned
in bunker.lock.json.
"""
import argparse, hashlib, json, shutil, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME_FILES = ['FirebaseCppApp-9_6_0.dll', 'grpc_csharp_ext.x64.dll']
ANGLE = ['libEGL.dll', 'libGLESv2.dll']


def main():
    ap = argparse.ArgumentParser()
    for name in ('exe', 'launcher', 'deps', 'server', 'out'):
        ap.add_argument('--' + name, type=Path, required=True)
    a = ap.parse_args()
    pin = json.loads((ROOT / 'bunker.lock.json').read_text())['bunker']['server']['windows-x86_64']['sha256']
    if hashlib.sha256(a.server.read_bytes()).hexdigest() != pin:
        sys.exit('The server file is not the one pinned in bunker.lock.json')
    for name in ANGLE:
        if not (a.deps / 'angle' / name).is_file():
            sys.exit(f'{a.deps}/angle/{name} is missing')
    out = a.out
    (out / 'game').mkdir(parents=True)
    (out / 'server').mkdir()
    shutil.copy2(a.launcher, out / 'reincomp.exe')
    shutil.copy2(a.exe, out / 'game/bunker-game-bootstrap.exe')
    for name in GAME_FILES:
        shutil.copy2(a.deps / name, out / 'game' / name)
    shutil.copytree(a.deps / 'angle', out / 'game/angle')
    shutil.copy2(a.server, out / 'server/bunker-server.exe')
    shutil.copytree(a.deps / 'licenses', out / 'licenses')
    shutil.copy2(ROOT / 'LICENSE', out / 'licenses/LICENSE.reincomp.txt')
    shutil.copy2(ROOT / 'THIRD-PARTY.md', out / 'licenses/THIRD-PARTY.md')
    (out / 'README.txt').write_text(
        're[in]comp\n\n'
        '1. Put your game APK in this folder (any name ending in .apk).\n'
        '2. Run reincomp.exe. The first start prepares the game files; that takes about a minute.\n\n'
        'The server needs the imported game files. If they are not in data\\assets, start it as\n'
        '    reincomp.exe -assets <folder>\n\n'
        'Saves, logs and prepared files are kept in the data folder here.\n', newline='\r\n')
    print(out)


if __name__ == '__main__':
    main()
