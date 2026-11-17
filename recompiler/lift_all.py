"""Lift one library's whole list of trace heads into objects (Linux, WSL or Windows).

    python3 lift_all.py --lib libunity.so --bias 0x70000000 --heads heads.txt \
        --toolchain DIR --poll poll-instrument --out DIR [--jobs N]

heads.txt holds every trace head of the library, one hexadecimal address per
line. The heads are cut into shards in address order; each shard is lifted
with DIR/remill-lift and DIR/aarch64.bc, instrumented, and compiled for
Windows. Every head is a boundary for every shard, so the shards link
together. A direct target that is not in the list is lifted in a further
round and reported. Finished shards are kept: run again to resume.

Output: OUT/shard-NNNNN.o and OUT/result.json (the objects in link order and
any heads added).
"""
import argparse, concurrent.futures, json, re, shlex, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / 'tools'))
from lift_new_traces import load_image  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--lib', type=Path, required=True)
    ap.add_argument('--bias', type=lambda s: int(s, 0), required=True)
    ap.add_argument('--heads', type=Path, required=True)
    ap.add_argument('--toolchain', type=Path, required=True, help='folder with remill-lift and aarch64.bc')
    ap.add_argument('--poll', type=Path, required=True)
    ap.add_argument('--poll-flags', default='--speed --inline-poll=teb')
    ap.add_argument('--clang', default='/usr/lib/llvm-18/bin/clang', help='clang 18; on Windows, the path of clang.exe')
    ap.add_argument('--opt', default='-O1')
    ap.add_argument('--fallback-opt', default='-O2', help='used for a shard the compiler fails on at --opt')
    ap.add_argument('--target', default='x86_64-w64-windows-gnu')
    ap.add_argument('--patches', type=Path, help='game/patches.json: code bytes to change before lifting')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--shard-size', type=int, default=128)
    ap.add_argument('--jobs', type=int, default=8)
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    image, start, execs = load_image(a.lib)
    # Code patches (game/patches.json): the bytes are changed in the image the lifter
    # reads, so the recompiled code carries them. The library on disk is not touched.
    patched = bytearray(image)
    for patch in (json.loads(a.patches.read_text()) if a.patches else []):
        at = int(patch['address'], 16) - a.bias - start
        data = bytes.fromhex(patch['bytes'])
        if 0 <= at <= len(patched) - len(data):
            patched[at:at + len(data)] = data
    image = bytes(patched)
    raw = a.out / 'image.bin'
    if not raw.exists() or raw.read_bytes() != image:
        raw.write_bytes(image)
    in_exec = lambda x: x % 4 == 0 and any(lo <= x - a.bias < hi for lo, hi in execs)
    heads = sorted({int(line, 16) for line in a.heads.read_text().split()})
    bad = [x for x in heads if not in_exec(x)]
    assert not bad, f'{len(bad)} heads are outside executable code, first {bad[0]:#x}'
    flags = shlex.split(a.poll_flags)
    # On Windows the tools carry .exe.
    program = lambda path: str(path) + ('.exe' if not Path(path).exists() and Path(str(path) + '.exe').exists() else '')
    lifter, poll = program(a.toolchain / 'remill-lift'), program(a.poll)

    def lift(job):
        index, entries, heads_file = job
        prefix = a.out / f'shard-{index:05d}'
        obj, done = prefix.with_suffix('.o'), prefix.with_suffix('.done')
        if obj.exists() and done.exists():
            return index, json.loads(done.read_text())
        ef, ll, pl = prefix.with_suffix('.entries'), prefix.with_suffix('.ll'), prefix.with_suffix('.poll.ll')
        ef.write_text(''.join(f'{x:#x}\n' for x in entries))
        fallback = False
        with prefix.with_suffix('.log').open('w') as log:
            run = lambda command: subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
            run([lifter, '--arch', 'aarch64', '--os', 'linux', '--semantics_dir', str(a.toolchain),
                 '--bytes_file', str(raw), '--address', str(a.bias + start), '--entry_addresses_file', str(ef),
                 '--trace_heads_file', str(heads_file), '--guest_call_transfer=true', '--ir_out', str(ll)])
            run([poll, *flags, str(ll), str(pl)])
            compile_ir = lambda level: [a.clang, '-target', a.target, level, '-Wno-override-module', '-c', str(pl), '-o', str(obj)]
            try:
                run(compile_ir(a.opt))
            except subprocess.CalledProcessError:
                # clang 18's register allocator crashes on a few very large functions at
                # -O1 (hoisting spills); the same module compiles at -O2.
                run(compile_ir(a.fallback_opt))
                fallback = True
        text = ll.read_text()
        defs = {int(x, 16) for x in re.findall(r'^define[^\n]*@sub_([0-9a-f]+)\(', text, re.M)}
        decls = {int(x, 16) for x in re.findall(r'^declare[^\n]*@sub_([0-9a-f]+)\(', text, re.M)}
        assert set(entries) <= defs, f'shard {index}: the lifter left out requested heads'
        summary = {'defs': sorted(defs), 'decls': sorted(decls), 'fallback': fallback}
        for f in (ll, pl, ef):
            f.unlink()
        done.write_text(json.dumps(summary))
        return index, summary

    known, lifted, objects, added, index, rounds = set(heads), set(), [], [], 0, 0
    fallbacks = []
    pending = heads
    while pending:
        rounds += 1
        heads_file = a.out / f'heads-round{rounds}.txt'
        heads_file.write_text(''.join(f'{x:#x}\n' for x in sorted(known)))
        jobs = []
        for i in range(0, len(pending), a.shard_size):
            jobs.append((index, pending[i:i + a.shard_size], heads_file))
            index += 1
        wanted = set()
        with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
            for count, (shard, summary) in enumerate(pool.map(lift, jobs), 1):
                defs = set(summary['defs'])
                duplicate = defs & lifted
                assert not duplicate, f'shard {shard} defines heads another shard has: {sorted(map(hex, duplicate))[:4]}'
                lifted |= defs
                if summary.get('fallback'):
                    fallbacks.append(shard)
                wanted |= set(summary['decls'])
                objects.append(f'shard-{shard:05d}.o')
                if count % 100 == 0:
                    print(f'round {rounds}: {count} of {len(jobs)} shards', flush=True)
        # Direct targets nobody defines: lift them too, as heads of their own.
        pending = sorted(x for x in wanted - lifted - known if in_exec(x))
        added += pending
        known |= set(pending)
    result = {'lib': a.lib.name, 'bias': hex(a.bias), 'heads': len(lifted), 'added': [hex(x) for x in added],
              'objects': objects, 'rounds': rounds, 'fallback_shards': fallbacks}
    (a.out / 'result.json').write_text(json.dumps(result))
    print(json.dumps({**result, 'objects': len(objects), 'added': len(added)}))


if __name__ == '__main__':
    main()
