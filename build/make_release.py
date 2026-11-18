#!/usr/bin/env python3
"""Assemble the folder players unzip: launcher, game, server and licenses.

    python build/make_release.py --exe EXE --launcher EXE --deps DIR --server EXE --out DIR [--pods DIR]

DEPS holds what ships beside the game executable: angle/ (libEGL.dll and the
rest), FirebaseCppApp-9_6_0.dll, grpc_csharp_ext.x64.dll and licenses/.
SERVER is Bunker's bunker-server for Windows; its hash must be the one pinned
in bunker.lock.json. PODS is the folder build/make_pods.py makes: with it,
Ctrl+B in the game shows the Bunker's pages.
"""
import argparse, hashlib, json, shutil, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME_FILES = ['FirebaseCppApp-9_6_0.dll', 'grpc_csharp_ext.x64.dll']
ANGLE = ['libEGL.dll', 'libGLESv2.dll']
README = '''re[in]comp

1. Put these three files of yours in this folder:
     your game APK (any name ending in .apk)
     20240404193219.bin.e (the master data)
     resource_dump_android.7z (the resource dump)
2. Run reincomp.exe. The first start prepares and imports them: about 20 GB, several minutes.

reincomp.exe -export-save backup.zip   writes a backup of your save
reincomp.exe -import-save backup.zip   restores one (also one made by Bunker on a phone)
reincomp.exe -nosound                  plays without sound

Ctrl+B in the game opens and closes the Bunker (Pod Programs), when the pods folder is here.
F11 or Alt+Enter switches to full screen.

Saves, logs and the imported files are kept in the data folder here.
'''


def main():
    ap = argparse.ArgumentParser()
    for name in ('exe', 'launcher', 'deps', 'server', 'out'):
        ap.add_argument('--' + name, type=Path, required=True)
    ap.add_argument('--pods', type=Path, help='the folder from make_pods.py (optional)')
    ap.add_argument('--unpinned-server', action='store_true', help='accept a server build other than the pinned release (to test one)')
    a = ap.parse_args()
    pin = json.loads((ROOT / 'bunker.lock.json').read_text())['bunker']['server']['windows-x86_64']['sha256']
    if not a.unpinned_server and hashlib.sha256(a.server.read_bytes()).hexdigest() != pin:
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
    if a.pods:
        shutil.copytree(a.pods, out / 'pods')
    (out / 'README.txt').write_text(README, newline='\r\n')
    print(out)


if __name__ == '__main__':
    main()
