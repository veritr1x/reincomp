#!/usr/bin/env python3
"""Inline trivial runtime helpers at lifted-code call sites, in place.

    python inline_lifted_helpers.py EXE [--nm EXE.nm] [--fpsr-unobserved] [--dry-run]

Lifted functions (symbols `sub_*`) call Remill memory/FP intrinsics out of
line. Each such call is emitted as

    sub $0x20,%rsp ; <argument moves> ; call HELPER ; add $0x20,%rsp

The plain loads are a single `mov (%rdx),...; ret`, so the 5-byte call is
replaced by the equivalent load (rdx = guest address, result in rax/xmm0),
padded with a nop. Only rax/xmm0 are written, a subset of what the call may
clobber, so the surrounding code is unaffected.

With --fpsr-unobserved (valid only for programs whose guest code never reads
or writes FPSR; see __bunker_set_fpsr_unobserved in runtime/runtime.cpp),
`__remill_fpu_exception_clear` becomes a nop and `__remill_fpu_exception_test`
returns 0. Remill only ORs that result into the sticky State.sr bits.

A site is rewritten only when it is an E8 rel32 call whose target is the
helper, immediately followed by `add $0x20,%rsp` (48 83 c4 20), and lies inside
a `sub_*` symbol. Works for PE and ELF via objdump/nm section and symbol data.
"""
import argparse
import bisect
import re
import struct
import subprocess
import sys

NOP2 = b'\x66\x90'
NOP3 = b'\x0f\x1f\x00'
LOADS = {
    '__remill_read_memory_8': b'\x0f\xb6\x02' + NOP2,      # movzbl (%rdx),%eax
    '__remill_read_memory_16': b'\x0f\xb7\x02' + NOP2,     # movzwl (%rdx),%eax
    '__remill_read_memory_32': b'\x8b\x02' + NOP3,         # mov (%rdx),%eax
    '__remill_read_memory_64': b'\x48\x8b\x02' + NOP2,     # mov (%rdx),%rax
    '__remill_read_memory_f32': b'\xf3\x0f\x10\x02\x90',   # movss (%rdx),%xmm0
    '__remill_read_memory_f64': b'\xf2\x0f\x10\x02\x90',   # movsd (%rdx),%xmm0
}
# --inline-stores: plain guest stores become the store itself plus the returned
# Memory* (rcx -> rax by xchg; rcx is call-clobbered, so its new value is dead).
# This drops the helper's exclusive-reservation invalidation; exclusive stores
# still compare-exchange against the reserved value, so only an A->B->A plain
# store sequence during another thread's LDXR/STXR window goes unnoticed.
STORES = {
    '__remill_write_memory_8': b'\x44\x88\x02\x48\x91',  # mov %r8b,(%rdx); xchg %rcx,%rax
    '__remill_write_memory_32': b'\x44\x89\x02\x48\x91',  # mov %r8d,(%rdx); xchg %rcx,%rax
    '__remill_write_memory_64': b'\x4c\x89\x02\x48\x91',  # mov %r8,(%rdx);  xchg %rcx,%rax
}
FPSR = {
    '__remill_fpu_exception_clear': b'\x0f\x1f\x44\x00\x00',  # nop
    '__remill_fpu_exception_test': b'\x31\xc0' + NOP3,        # xor %eax,%eax
}
# Expected bodies; refuse to rewrite if a helper changed shape.
BODIES = {
    '__remill_read_memory_8': b'\x0f\xb6\x02\xc3',
    '__remill_read_memory_16': b'\x0f\xb7\x02\xc3',
    '__remill_read_memory_32': b'\x8b\x02\xc3',
    '__remill_read_memory_64': b'\x48\x8b\x02\xc3',
    '__remill_read_memory_f32': b'\xf3\x0f\x10\x02\xc3',
    '__remill_read_memory_f64': b'\xf2\x0f\x10\x02\xc3',
}


def sections(exe):
    out = subprocess.run(['objdump', '-h', exe], capture_output=True, text=True, check=True).stdout
    lines = out.splitlines()
    result = []
    for i, line in enumerate(lines):
        p = line.split()
        if len(p) >= 6 and p[0].isdigit():
            flags = lines[i + 1] if i + 1 < len(lines) else ''
            result.append((p[1], int(p[3], 16), int(p[2], 16), int(p[5], 16), 'CODE' in flags))
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exe')
    ap.add_argument('--nm')
    ap.add_argument('--fpsr-unobserved', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--all-sites', action='store_true',
                    help='also rewrite call sites not followed by add $0x20,%%rsp (preallocated shadow space)')
    ap.add_argument('--inline-stores', action='store_true', help='inline 8/32/64-bit guest stores (see STORES)')
    a = ap.parse_args()
    nm = open(a.nm).read() if a.nm else subprocess.run(['nm', '-n', '--defined-only', a.exe], capture_output=True, text=True, check=True).stdout
    starts, names, addr = [], [], {}
    for line in nm.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in 'TtWw':
            v = int(p[0], 16)
            starts.append(v); names.append(p[2]); addr.setdefault(p[2], v)
    wanted = dict(LOADS)
    if a.fpsr_unobserved:
        wanted.update(FPSR)
    if a.inline_stores:
        wanted.update(STORES)
    targets = {addr[n]: (n, b) for n, b in wanted.items() if n in addr}
    with open(a.exe, 'r+b' if not a.dry_run else 'rb') as f:
        data = bytearray(f.read())
        for name, vma, size, off, code in sections(a.exe):
            if not code:
                continue
            for n, body in BODIES.items():
                if n in addr and vma <= addr[n] < vma + size:
                    got = bytes(data[off + addr[n] - vma: off + addr[n] - vma + len(body)])
                    if got != body:
                        sys.exit(f'{n} body changed: {got.hex()}')
            counts = {}
            skipped = 0
            pattern = rb'\xe8(....)' if a.all_sites else rb'\xe8(....)\x48\x83\xc4\x20'
            for m in re.finditer(pattern, bytes(data[off:off + size]), re.S):
                site = vma + m.start()
                target = site + 5 + struct.unpack('<i', m.group(1))[0]
                hit = targets.get(target)
                if not hit:
                    continue
                i = bisect.bisect_right(starts, site) - 1
                if i < 0 or not names[i].startswith('sub_'):
                    skipped += 1
                    continue
                data[off + m.start(): off + m.start() + 5] = hit[1]
                counts[hit[0]] = counts.get(hit[0], 0) + 1
            for n in sorted(counts):
                print(f'{name}: {n} {counts[n]}')
            print(f'{name}: non-lifted sites left alone {skipped}')
        if not a.dry_run:
            f.seek(0)
            f.write(data)


if __name__ == '__main__':
    main()
