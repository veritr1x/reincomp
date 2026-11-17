#!/usr/bin/env python3
"""Collect the game-derived inputs of a linked build into one portable folder.

    python build/make_pack.py LINK_RESPONSE BUILD_DIR PACK_DIR

LINK_RESPONSE is an existing build's game-bootstrap-link.rsp; BUILD_DIR holds
its generated headers (game_traces.h and so on). The pack gets every guest
object in link order, those headers, the third-party include folders and
libz, and pack.json, which build.py reads. A pack is made from the game's own
code: keep it out of this repository.
"""
import hashlib, json, os, re, shutil, sys
from pathlib import Path

HOST = re.compile(r'(host-\d+|game-bootstrap|elfload)\.o$')


def place(source, target):
    try:
        os.link(source, target)  # same volume: no copy
    except OSError:
        shutil.copy2(source, target)


def main():
    response, build, pack = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    args = [a.strip().strip('"') for a in response.read_text().split() if a.strip()]
    (pack / 'objects').mkdir(parents=True)
    (pack / 'generated').mkdir()
    objects, wraps, libraries, includes, archive = [], [], [], [], None
    for a in args:
        if a.endswith('.o'):
            if HOST.search(a):
                continue
            name = f'{len(objects):05d}-{Path(a).name}'
            place(a, pack / 'objects' / name)
            objects.append(name)
        elif a.endswith('.a'):
            archive = a
        elif a.startswith('-Wl,--wrap='):
            wraps.append(a)
        elif a.startswith('-l'):
            libraries.append(a)
        elif a.startswith('-I'):
            includes.append(Path(a[2:]))
    for header in sorted(build.glob('*.h')):
        shutil.copy2(header, pack / 'generated' / header.name)
    for folder in includes:
        if folder.name.endswith('-include') and not (pack / folder.name).exists():
            shutil.copytree(folder, pack / folder.name)
    shutil.copy2(archive, pack / Path(archive).name)
    digest = hashlib.sha256()
    for name in objects:
        digest.update(name.encode() + b'\0' + hashlib.sha256((pack / 'objects' / name).read_bytes()).digest())
    manifest = {'objects': objects, 'objects_sha256': digest.hexdigest(), 'archive': Path(archive).name,
                'wraps': wraps, 'libraries': libraries,
                'generated': sorted(p.name for p in (pack / 'generated').iterdir()),
                'includes': sorted(p.name for p in pack.iterdir() if p.name.endswith('-include'))}
    (pack / 'pack.json').write_text(json.dumps(manifest, indent=1) + '\n')
    print(f'{len(objects)} guest objects, {len(wraps)} wraps -> {pack}')


if __name__ == '__main__':
    main()
