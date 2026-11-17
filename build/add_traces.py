#!/usr/bin/env python3
"""Add newly lifted functions to a pack.

    python build/add_traces.py PACK_DIR RESULT.json [RESULT.json ...]

Each RESULT.json comes from recompiler/tools/lift_new_traces.py. Its objects
are appended to the pack's link list and its new heads are registered in
generated/game_traces.h (kTraces stays sorted by address).
"""
import hashlib, json, re, shutil, sys
from pathlib import Path


def windows_path(p):
    m = re.match(r'/mnt/([a-z])/(.*)', p)
    return f'{m.group(1).upper()}:/{m.group(2)}' if m else p


def main():
    pack = Path(sys.argv[1])
    manifest = json.loads((pack / 'pack.json').read_text())
    heads, objects = set(), []
    for result in sys.argv[2:]:
        v = json.loads(Path(result).read_text())
        heads |= {int(x, 16) for x in v['new_heads']}
        objects += [Path(windows_path(o)) for o in v['objects']]
    if not heads:
        print('no new heads')
        return
    header = pack / 'generated/game_traces.h'
    text = header.read_text()
    start = text.index('static const recomp::Trace kTraces[] = {')
    end = text.index('};', start)
    table = {int(m.group(1), 16): m.group(0) for m in re.finditer(r'\{(0x[0-9a-f]+),\s*sub_[0-9a-f]+\}', text[start:end])}
    assert len(table) == text.count('{0x', start, end), 'unrecognized kTraces entries'
    assert not heads & set(table), 'new heads already registered'
    for h in heads:
        table[h] = f'{{{h:#x},sub_{h:x}}}'
    decls = ''.join(f'extern "C" Memory *sub_{h:x}(State &,addr_t,Memory *);\n' for h in sorted(heads))
    body = 'static const recomp::Trace kTraces[] = {\n' + ''.join(f'  {table[h]},\n' for h in sorted(table))
    header.write_text(text[:start] + decls + body + text[end:])
    for source in objects:
        name = f'{len(manifest["objects"]):05d}-{source.parent.name}-{source.name}'
        shutil.copy2(source, pack / 'objects' / name)
        manifest['objects'].append(name)
    digest = hashlib.sha256()
    for name in manifest['objects']:
        digest.update(name.encode() + b'\0' + hashlib.sha256((pack / 'objects' / name).read_bytes()).digest())
    manifest['objects_sha256'] = digest.hexdigest()
    (pack / 'pack.json').write_text(json.dumps(manifest, indent=1) + '\n')
    print(f'registered {len(heads)} new heads from {len(objects)} objects')


if __name__ == '__main__':
    main()
