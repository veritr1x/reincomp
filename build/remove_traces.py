#!/usr/bin/env python3
"""Undo build/add_traces.py for one or more results.

    python build/remove_traces.py PACK_DIR RESULT.json [RESULT.json ...]

Removes the result's objects from the pack and its heads from
generated/game_traces.h.
"""
import hashlib, json, re, sys
from pathlib import Path


def main():
    pack = Path(sys.argv[1])
    manifest = json.loads((pack / 'pack.json').read_text())
    heads, folders = set(), set()
    for result in sys.argv[2:]:
        v = json.loads(Path(result).read_text())
        heads |= {int(x, 16) for x in v['new_heads']}
        folders |= {Path(o).parent.name for o in v['objects']}
    drop = [n for n in manifest['objects'] if any(f'-{folder}-' in n for folder in folders)]
    for name in drop:
        (pack / 'objects' / name).unlink()
    manifest['objects'] = [n for n in manifest['objects'] if n not in set(drop)]
    header = pack / 'generated/game_traces.h'
    names = {f'{h:x}' for h in heads}
    kept = []
    for line in header.read_text().splitlines(keepends=True):
        m = re.match(r'extern "C" Memory \*sub_([0-9a-f]+)\(', line) or re.match(r'\s*\{0x([0-9a-f]+),\s*sub_', line)
        if m and m.group(1) in names:
            continue
        kept.append(line)
    header.write_text(''.join(kept))
    digest = hashlib.sha256()
    for name in manifest['objects']:
        digest.update(name.encode() + b'\0' + hashlib.sha256((pack / 'objects' / name).read_bytes()).digest())
    manifest['objects_sha256'] = digest.hexdigest()
    (pack / 'pack.json').write_text(json.dumps(manifest, indent=1) + '\n')
    print(f'removed {len(drop)} objects and {len(heads)} heads')


if __name__ == '__main__':
    main()
