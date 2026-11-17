"""Lift additional guest trace heads into objects that link beside an existing
cohort (run under Linux/WSL; the lifter is a Linux binary).

    python3 lift_new_traces.py --lib libil2cpp.so --bias 0x80000000 \
        --traces-h BUILD/game_traces.h --seeds a.json [--seeds b.json] --out DIR \
        --lifter remill-lift --semantics aarch64.bc --poll poll-instrument

Seeds are JSON lists (or {"missing": [...]}) of absolute guest addresses that
are not yet trace heads, e.g. unresolved switch-case targets. Every existing
head stays an external boundary, so new traces only add code; direct targets
that are neither existing nor new heads are lifted in further rounds, like
recompiler/compile.py. The lifter image holds every PT_LOAD segment so jump
tables in read-only data are visible. Output: DIR/new-*.o plus DIR/result.json
(new heads, objects, traces with unsupported instructions).
"""
import argparse, concurrent.futures, json, re, struct, subprocess
from pathlib import Path


def load_image(path):
    d = Path(path).read_bytes()
    assert d[:4] == b'\x7fELF' and d[4] == 2 and d[5] == 1, 'expected ELF64 little-endian'
    phoff, = struct.unpack_from('<Q', d, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
    loads, execs = [], []
    for i in range(phnum):
        p_type, p_flags, p_offset, p_vaddr, _, p_filesz, _, _ = struct.unpack_from('<IIQQQQQQ', d, phoff + phentsize * i)
        if p_type != 1:
            continue
        loads.append((p_vaddr, p_offset, p_filesz))
        if p_flags & 1:
            execs.append((p_vaddr, p_vaddr + p_filesz))
    start = min(v for v, _, _ in loads)
    end = max(v + n for v, _, n in loads)
    image = bytearray(end - start)
    for v, off, n in loads:
        image[v - start:v - start + n] = d[off:off + n]
    return bytes(image), start, execs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--lib', type=Path, required=True)
    ap.add_argument('--bias', type=lambda s: int(s, 0), required=True)
    ap.add_argument('--traces-h', type=Path, required=True)
    ap.add_argument('--seeds', type=Path, action='append', required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--lifter', type=Path, required=True)
    ap.add_argument('--semantics', type=Path, required=True)
    ap.add_argument('--poll', type=Path, required=True)
    ap.add_argument('--cxx', default='/usr/lib/llvm-18/bin/clang')
    ap.add_argument('--target', default='x86_64-w64-windows-gnu')
    ap.add_argument('--shard-size', type=int, default=64)
    ap.add_argument('--jobs', type=int, default=8)
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)

    existing = {int(x, 16) for x in re.findall(r'^extern "C" Memory \*sub_([0-9a-f]+)\(', a.traces_h.read_text(), re.M)}
    image, start, execs = load_image(a.lib)
    raw = a.out / 'image.bin'
    raw.write_bytes(image)
    ranges = [(a.bias + lo, a.bias + hi) for lo, hi in execs]
    in_exec = lambda x: x % 4 == 0 and any(lo <= x < hi for lo, hi in ranges)

    seeds = set()
    for s in a.seeds:
        v = json.loads(s.read_text())
        seeds.update(v['missing'] if isinstance(v, dict) else v)
    pending = sorted(x for x in seeds if x not in existing and in_exec(x))
    rejected = sorted(x for x in seeds if x not in existing and not in_exec(x))
    lifted, unsupported, outside, objects = set(), set(), set(), []
    index = 0

    def lift(job):
        idx, entries, heads_file = job
        prefix = a.out / f'new-{idx:04d}'
        ef = prefix.with_suffix('.entries')
        ef.write_text(''.join(f'{x:#x}\n' for x in entries))
        ll, pl, obj = prefix.with_suffix('.ll'), prefix.with_suffix('.poll.ll'), prefix.with_suffix('.o')
        with prefix.with_suffix('.log').open('w') as log:
            subprocess.run([str(a.lifter), '--arch', 'aarch64', '--os', 'linux', '--semantics_dir', str(a.semantics.parent),
                            '--bytes_file', str(raw), '--address', str(a.bias + start), '--entry_addresses_file', str(ef),
                            '--trace_heads_file', str(heads_file), '--guest_call_transfer=true', '--ir_out', str(ll)],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
            subprocess.run([str(a.poll), str(ll), str(pl)], stdout=log, stderr=subprocess.STDOUT, check=True)
            subprocess.run([a.cxx, '-target', a.target, '-O2', '-Wno-override-module', '-c', str(pl), '-o', str(obj)],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        text = ll.read_text()
        defs = {int(x, 16) for x in re.findall(r'^define[^\n]*@sub_([0-9a-f]+)\(', text, re.M)}
        decls = {int(x, 16) for x in re.findall(r'^declare[^\n]*@sub_([0-9a-f]+)\(', text, re.M)}
        errors = set()
        for m in re.finditer(r'^define[^\n]*@sub_([0-9a-f]+)\([^\n]*\).*?^}', text, re.M | re.S):
            if re.search(r'call[^\n]*@__remill_(?:error|missing_block)\(', m.group()):
                errors.add(int(m.group(1), 16))
        return idx, defs, decls, errors, str(obj), set(entries)

    rounds = 0
    while pending:
        rounds += 1
        heads = sorted(existing | lifted | set(pending))
        heads_file = a.out / f'heads-round{rounds}.txt'
        heads_file.write_text(''.join(f'{x:#x}\n' for x in heads))
        jobs = []
        for i in range(0, len(pending), a.shard_size):
            jobs.append((index, pending[i:i + a.shard_size], heads_file))
            index += 1
        discovered = set()
        with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
            for idx, defs, decls, errors, obj, entries in pool.map(lift, jobs):
                assert entries <= defs, f'shard {idx}: lifter omitted requested entries'
                dup = defs & (existing | lifted)
                assert not dup, f'shard {idx}: redefines existing heads {sorted(map(hex, dup))[:5]}'
                lifted |= defs
                unsupported |= errors
                discovered |= decls
                objects.append(obj)
                print(f'round {rounds} shard {idx}: {len(defs)} traces, {len(errors)} unsupported', flush=True)
        pending = []
        for x in sorted(discovered - existing - lifted):
            if in_exec(x):
                pending.append(x)
            else:
                outside.add(x)
    result = dict(lib=str(a.lib), bias=hex(a.bias), new_heads=[hex(x) for x in sorted(lifted)],
                  unsupported=[hex(x) for x in sorted(unsupported)], outside_executable=[hex(x) for x in sorted(outside)],
                  rejected_seeds=[hex(x) for x in rejected], objects=objects, rounds=rounds)
    (a.out / 'result.json').write_text(json.dumps(result, indent=1) + '\n')
    print(json.dumps({k: (len(v) if isinstance(v, list) else v) for k, v in result.items()}))


if __name__ == '__main__':
    main()
