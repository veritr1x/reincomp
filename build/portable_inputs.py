"""Fetch and verify public build dependencies for the Global Windows release."""
import hashlib
import json
from pathlib import Path
import shutil
import urllib.request
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
BUNKER = json.loads((ROOT/'bunker.lock.json').read_bytes())['bunker']
RELEASE = f'{BUNKER["repository"]}/releases/download/{BUNKER["release"]}/'
KIT = (BUNKER['browserKit']['file'], BUNKER['browserKit']['sha256'])
NOTICES = ('bunker-notices-306a4695f28b.zip', '306a4695f28bf30b0dba2bdd3516d23051f18474ddfd52bd994d328730ef278d')
PORTABLE = json.loads((ROOT/'release.lock.json').read_bytes())['portable']


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def download(folder, item, base_url=RELEASE):
    name, expected = item
    path = folder/name
    if path.is_file() and digest(path) == expected:
        return path
    pending = path.with_suffix('.partial')
    req = urllib.request.Request(base_url+name, headers={'User-Agent':'reincomp-source-build'})
    print(f'Fetching {name}', flush=True)
    with urllib.request.urlopen(req, timeout=180) as src, pending.open('wb') as out:
        shutil.copyfileobj(src, out)
    if digest(pending) != expected:
        raise ValueError(f'Checksum mismatch for {name}')
    pending.replace(path)
    return path


def prepare(work):
    work.mkdir(parents=True, exist_ok=True)
    site = work/'site'
    target = site/'windows'
    target.mkdir(parents=True, exist_ok=True)
    with ZipFile(download(work, KIT)) as z:
        info = json.loads(z.read('kit.json'))
        if set(z.namelist()) != {'kit.json', *info['files']}:
            raise ValueError('Unexpected SDK archive members')
        for name in z.namelist():
            path = (target/name).resolve()
            if not path.is_relative_to(target.resolve()) or name.startswith('runtime/') or '\\' in name:
                raise ValueError('Unexpected SDK member '+name)
            if name.startswith(('link/host-','link/embedded-bootstrap.','link/elfload.','link/runtime-host.')):
                # These are compiled from the checkout. The link manifest
                # supplies reference digests for the independent rebuild.
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            with z.open(name) as src, path.open('wb') as f:
                shutil.copyfileobj(src, f)
    bundles = site/'bundles'
    bundles.mkdir(exist_ok=True)
    notices = download(work, NOTICES)
    # The one-command build serves files on loopback; no Pages file-size limit.
    shutil.copyfile(notices,bundles/'notices.zip')
    (bundles/'notices.json').write_text(json.dumps({'parts':['notices.zip'], 'size':notices.stat().st_size}))
    return site
