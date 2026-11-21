"""Export the native build's trace lists and patch edits for the web compiler."""
import argparse
from hashlib import sha256
import json
from pathlib import Path
import shutil
import sys
import tempfile
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'build'))
import from_apk
import native_release
sys.path.insert(0,str(ROOT/'build/native_player'))
from generate_abi import ArmImage
from spring_methods import spring_entries


def edits(source, patched):
    if len(source) != len(patched):
        raise ValueError('A fixed-size Global patch unexpectedly changed the file length')
    result = []
    for base in range(0, len(source), 65536):
        before, after = source[base:base + 65536], patched[base:base + 65536]
        if before == after:
            continue
        i = 0
        while i < len(before):
            if before[i] == after[i]:
                i += 1
                continue
            first = i
            while i < len(before) and before[i] != after[i]:
                i += 1
            result.append({'offset': base + first, 'bytes': after[first:i].hex()})
    return {'sourceSHA256': sha256(source).hexdigest(), 'patchedSHA256': sha256(patched).hexdigest(), 'edits': result}


def prepare(apk, bunker, cxx, out):
    if native_release.digest(apk) != from_apk.APK:
        raise ValueError('Expected the original Global 3.7.1 ARM64 APK')
    release = json.loads((ROOT / 'bunker.lock.json').read_text())['bunker']['release']
    if 'v' + (bunker / 'VERSION').read_text().strip() != release:
        raise ValueError(f'Expected Bunker {release}')
    patch = native_release.module('bunker_game_patch', bunker / 'android/tools/game_patch.py')
    out.mkdir(parents=True, exist_ok=True)
    with ZipFile(apk) as archive, tempfile.TemporaryDirectory() as work:
        metadata_path = 'assets/bin/Data/Managed/Metadata/global-metadata.dat'
        metadata = archive.read(metadata_path)
        il2cpp = archive.read('lib/arm64-v8a/libil2cpp.so')
        (out/'spring_group.entries').write_bytes(spring_entries(ArmImage(il2cpp),metadata,ROOT/'game/heads-il2cpp.txt'))
        profile = patch.identify(il2cpp, metadata, 'global')
        info = {'format': 1, 'apkSHA256': from_apk.APK, 'libraries': [],
                'metadata': {'path': metadata_path, **edits(metadata, patch.patch_metadata(metadata, profile, 30000))}}
        for key, name, bias, heads in from_apk.LIBRARIES:
            path = 'lib/arm64-v8a/' + name
            source = il2cpp if key == 'il2cpp' else archive.read(path)
            patched = native_release.patch_runtime_library(
                patch.patch_library(source, profile, 30000), bunker, Path(work), cxx) if key == 'il2cpp' else source
            shutil.copyfile(ROOT / heads, out / Path(heads).name)
            info['libraries'].append({'name': key, 'path': path, 'bias': int(bias, 0),
                                      'heads': Path(heads).name, **edits(source, patched)})
    info['patches'] = json.loads((ROOT / 'game/patches.json').read_text())
    (out / 'game-plan.json').write_text(json.dumps(info, separators=(',', ':')), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ('apk', 'bunker', 'cxx', 'out'):
        parser.add_argument('--' + flag, type=Path, required=True)
    args = parser.parse_args()
    prepare(args.apk, args.bunker.resolve(), args.cxx.resolve(), args.out)
