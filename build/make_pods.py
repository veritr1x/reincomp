#!/usr/bin/env python3
"""Assemble the folder that serves the Bunker's pages (Pod Programs) on Windows.

    python build/make_pods.py --python EMBED_ZIP --bunker CHECKOUT --loader DLL --out DIR

EMBED_ZIP is python.org's "Windows embeddable package (64-bit)" for Python
3.12. CHECKOUT is a Bunker checkout at the release pinned in bunker.lock.json,
after its scripts/prepare.py: the pages are Bunker's, and they talk to the
server of the same release. DLL is WebView2Loader.dll (x64) from Microsoft's
Microsoft.Web.WebView2 package. The Python packages are downloaded from PyPI
with pip, as Windows wheels for Python 3.12.

DIR then holds python/ (the runtime), packages/, app/ (the pages),
WebView2Loader.dll and licenses/. Nothing in it comes from the game.
"""
import argparse, json, shutil, subprocess, sys, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# What Bunker's Android build installs (android/app/build.gradle), for Windows.
PACKAGES = ['fastapi==0.115.14', 'pydantic==1.10.22', 'uvicorn==0.34.3', 'jinja2==3.1.6', 'python-multipart==0.0.20',
            'pycryptodome==3.21.0', 'msgpack==1.1.1', 'lz4==4.3.3']
SKIP = shutil.ignore_patterns('__pycache__', '*.pyc')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--python', type=Path, required=True)
    ap.add_argument('--bunker', type=Path, required=True)
    ap.add_argument('--loader', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    source = a.bunker / 'android/app/src/main/python'
    if not (source / 'desktop_runtime.py').is_file() or not (source / 'web').is_dir():
        sys.exit(f'{source} is not prepared: run scripts/prepare.py in the Bunker checkout')
    release = json.loads((ROOT / 'bunker.lock.json').read_text())['bunker']['release']
    version = (a.bunker / 'VERSION').read_text().strip()
    if 'v' + version != release:
        sys.exit(f'The Bunker checkout is {version}; bunker.lock.json pins {release}')
    out = a.out
    out.mkdir(parents=True)
    with zipfile.ZipFile(a.python) as z:
        z.extractall(out / 'python')
    pth = next((out / 'python').glob('python3*._pth'))
    # The runtime looks only here: its own library, the packages and the pages.
    pth.write_text(pth.stem + '.zip\n.\n..\\packages\n..\\app\n')
    subprocess.run([sys.executable, '-m', 'pip', 'install', '--quiet', '--no-compile', '--target', str(out / 'packages'),
                    '--only-binary=:all:', '--platform', 'win_amd64', '--python-version', '3.12',
                    '--implementation', 'cp', *PACKAGES], check=True)
    shutil.rmtree(out / 'packages/bin', ignore_errors=True)
    shutil.copytree(source, out / 'app', ignore=SKIP)
    shutil.copy2(a.loader, out / 'WebView2Loader.dll')

    licenses = out / 'licenses'
    licenses.mkdir()
    shutil.copy2(out / 'python/LICENSE.txt', licenses / 'python-LICENSE.txt')
    shutil.copy2(ROOT / 'third_party/webview2/LICENSE.txt', licenses / 'webview2-LICENSE.txt')
    shutil.copy2(a.bunker / 'LICENSE', licenses / 'bunker-LICENSE.txt')
    for name in ('THIRD-PARTY-NOTICES.md',):
        if (a.bunker / name).is_file():
            shutil.copy2(a.bunker / name, licenses / ('bunker-' + name))
    missing = []
    for info in sorted((out / 'packages').glob('*.dist-info')):
        found = [p for p in info.rglob('*') if p.is_file() and p.name.upper().startswith(('LICENSE', 'LICENCE', 'COPYING', 'NOTICE'))]
        for p in found:
            shutil.copy2(p, licenses / f'{info.name.removesuffix(".dist-info")}-{p.name}')
        if not found:
            missing.append(info.name.removesuffix('.dist-info'))
    (out / 'VERSION').write_text(f'Bunker {version}\n' + ''.join(
        f'{info.name.removesuffix(".dist-info")}\n' for info in sorted((out / 'packages').glob('*.dist-info'))))
    print(out)
    if missing:
        print('No license file inside the wheel of: ' + ', '.join(missing))


if __name__ == '__main__':
    main()
