#!/usr/bin/env python3
"""Make a pack from lifted objects.

    python build/make_pack.py --out PACK_DIR LIFT_DIR [LIFT_DIR ...]

Each LIFT_DIR is an output folder of recompiler/lift_all.py (one per library).
The pack gets every object and pack.json, which lists them in link order.
Heads that lift_all added on its own are appended to game/heads-*.txt, so the
build knows them. A pack is made from the game's own code: keep it out of this
repository.
"""
import argparse, json, os, shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def place(source, target):
    try:
        os.link(source, target)  # same volume: no copy
    except OSError:
        shutil.copy2(source, target)


def heads_file(address):
    return ROOT / ('game/heads-unity.txt' if address < 0x80000000 else 'game/heads-il2cpp.txt')


def add_heads(addresses):
    """Merge addresses into the committed head lists. Returns how many were new."""
    new = 0
    for path in {heads_file(x) for x in addresses}:
        have = {int(line, 16) for line in path.read_text().split()}
        mine = {x for x in addresses if heads_file(x) == path}
        new += len(mine - have)
        path.write_text(''.join(f'{x:#x}\n' for x in sorted(have | mine)), newline='\n')
    return new


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('lift', type=Path, nargs='+')
    a = ap.parse_args()
    (a.out / 'objects').mkdir(parents=True)
    objects, added = [], []
    for folder in a.lift:
        result = json.loads((folder / 'result.json').read_text())
        added += [int(x, 16) for x in result.get('added', [])]
        for name in result['objects']:
            target = f'{folder.name}-{name}'
            place(folder / name, a.out / 'objects' / target)
            objects.append(target)
    (a.out / 'pack.json').write_text(json.dumps({'objects': objects}, indent=1) + '\n')
    print(f'{len(objects)} objects -> {a.out}; {add_heads(added)} heads added to game/')


if __name__ == '__main__':
    main()
