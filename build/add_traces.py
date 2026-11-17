#!/usr/bin/env python3
"""Add newly lifted functions to a pack, or take them out again.

    python build/add_traces.py PACK_DIR RESULT.json [RESULT.json ...]
    python build/add_traces.py --remove PACK_DIR RESULT.json [RESULT.json ...]

Each RESULT.json comes from recompiler/tools/lift_new_traces.py. Its objects
join the pack's link list and its new heads join game/heads-*.txt.
"""
import argparse, json, re, shutil, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_pack import heads_file, add_heads  # noqa: E402


def windows_path(p):
    m = re.match(r'/mnt/([a-z])/(.*)', p)
    return f'{m.group(1).upper()}:/{m.group(2)}' if m else p


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--remove', action='store_true')
    ap.add_argument('pack', type=Path)
    ap.add_argument('results', type=Path, nargs='+')
    a = ap.parse_args()
    manifest = json.loads((a.pack / 'pack.json').read_text())
    heads, objects = set(), []
    for result in a.results:
        v = json.loads(result.read_text())
        heads |= {int(x, 16) for x in v['new_heads']}
        objects += [Path(windows_path(o)) for o in v['objects']]
    names = {o: f'{o.parent.name}-{o.name}' for o in objects}
    if a.remove:
        drop = set(names.values())
        for name in drop & set(manifest['objects']):
            (a.pack / 'objects' / name).unlink()
        manifest['objects'] = [n for n in manifest['objects'] if n not in drop]
        for path in {heads_file(x) for x in heads}:
            keep = [x for x in (int(line, 16) for line in path.read_text().split()) if x not in heads]
            path.write_text(''.join(f'{x:#x}\n' for x in keep), newline='\n')
        print(f'removed {len(drop)} objects and {len(heads)} heads')
    else:
        for source, name in names.items():
            if name not in manifest['objects']:
                shutil.copy2(source, a.pack / 'objects' / name)
                manifest['objects'].append(name)
        print(f'added {len(names)} objects; {add_heads(sorted(heads))} new heads in game/')
    (a.pack / 'pack.json').write_text(json.dumps(manifest, indent=1) + '\n')


if __name__ == '__main__':
    main()
