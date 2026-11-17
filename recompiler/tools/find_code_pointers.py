"""Find code addresses a library materializes or stores that are not trace heads.

    python3 find_code_pointers.py --lib libunity.so --bias 0x70000000 \
        --traces-h game_traces.h --range 0x70ac0000 0x70b60000 --out seeds.json

Functions reached only through pointers (callbacks, method tables filled in at
run time) are invisible to call-graph discovery. This lists two kinds of
candidates inside --range: targets of ADR and ADRP+ADD in executable code, and
relative relocations whose value points at executable code. The output is a
JSON list of absolute addresses for lift_new_traces.py --seeds.
"""
import argparse, json, re, struct
from pathlib import Path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--lib', type=Path, required=True)
    ap.add_argument('--bias', type=lambda s: int(s, 0), required=True)
    ap.add_argument('--traces-h', type=Path, required=True)
    ap.add_argument('--range', type=lambda s: int(s, 0), nargs=2, required=True, metavar=('LOW', 'HIGH'))
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--block-starts', action='store_true',
                    help='also list every instruction after an unconditional branch or return: switch cases '
                         'and other blocks only an indirect branch reaches')
    a = ap.parse_args()
    d = a.lib.read_bytes()
    existing = {int(x, 16) for x in re.findall(r'^extern "C" Memory \*sub_([0-9a-f]+)\(', a.traces_h.read_text(), re.M)}
    phoff, = struct.unpack_from('<Q', d, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
    execs, dynamic = [], None
    for i in range(phnum):
        p_type, p_flags, p_offset, p_vaddr, _, p_filesz, _, _ = struct.unpack_from('<IIQQQQQQ', d, phoff + phentsize * i)
        if p_type == 1 and p_flags & 1:
            execs.append((p_vaddr, p_offset, p_filesz))
        if p_type == 2:
            dynamic = (p_offset, p_filesz)
    low, high = a.range[0] - a.bias, a.range[1] - a.bias
    in_exec = lambda x: x % 4 == 0 and any(v <= x < v + n for v, _, n in execs)
    found = set()
    # ADRP xN, page ; ADD xN, xN, #imm
    for vaddr, offset, size in execs:
        words = struct.unpack_from(f'<{size // 4}I', d, offset)
        for i in range(len(words) - 1):
            w = words[i]
            if w & 0x9f000000 == 0x10000000:  # ADR xN, target
                imm = ((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3)
                if imm & (1 << 20):
                    imm -= 1 << 21
                target = vaddr + i * 4 + imm
                if low <= target < high and in_exec(target):
                    found.add(target)
                continue
            if w & 0x9f000000 != 0x90000000:
                continue
            n = words[i + 1]
            if n & 0xffc00000 != 0x91000000 or (n & 31) != (w & 31) or ((n >> 5) & 31) != (w & 31):
                continue
            imm = ((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            target = ((vaddr + i * 4) & ~0xfff) + (imm << 12) + ((n >> 10) & 0xfff)
            if low <= target < high and in_exec(target):
                found.add(target)
    if a.block_starts:
        for vaddr, offset, size in execs:
            words = struct.unpack_from(f'<{size // 4}I', d, offset)
            first, last = max(0, (low - vaddr) // 4), min(len(words) - 1, (high - vaddr) // 4)
            for i in range(first, last):
                w, following = words[i], words[i + 1]
                ends = w & 0xfc000000 == 0x14000000 or w == 0xd65f03c0 or w & 0xfffffc1f == 0xd61f0000
                # Skip padding and literal data after the branch.
                if ends and following not in (0, 0xd503201f) and following >> 24 != 0:
                    found.add(vaddr + (i + 1) * 4)
    # R_AARCH64_RELATIVE addends
    if dynamic:
        tags = {}
        for i in range(dynamic[1] // 16):
            tag, value = struct.unpack_from('<qQ', d, dynamic[0] + i * 16)
            tags[tag] = value
        rela, relasz = tags.get(7), tags.get(8, 0)
        if rela:
            for i in range(relasz // 24):
                _, info, addend = struct.unpack_from('<QQq', d, rela + i * 24)
                if info & 0xffffffff == 1027 and low <= addend < high and in_exec(addend):
                    found.add(addend)
    new = sorted(x + a.bias for x in found if x + a.bias not in existing)
    a.out.write_text(json.dumps(new))
    print(f'{len(found)} code pointers in range, {len(new)} are not trace heads')


if __name__ == '__main__':
    main()
