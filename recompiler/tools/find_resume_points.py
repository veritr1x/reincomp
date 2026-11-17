"""List the guest addresses where execution resumes after an emulated instruction.

    python find_resume_points.py EXE SYMBOLS.nm --traces-h game_traces.h --out PREFIX

Where the lifter cannot translate an instruction, the lifted code calls
__remill_error with the address of the next instruction; the runtime emulates
the instruction and continues there, so that address must be a trace head.
This scans a linked executable for those calls and writes the addresses that
are not heads yet to PREFIX-unity.json (0x7...) and PREFIX-il2cpp.json (0x8...),
as seeds for lift_new_traces.py.
"""
import argparse, json, re, struct
from pathlib import Path

import numpy as np


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('exe', type=Path)
    ap.add_argument('symbols', type=Path)
    ap.add_argument('--traces-h', type=Path, required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    error = None
    for line in a.symbols.open():
        parts = line.split()
        if len(parts) == 3 and parts[2] == '__remill_error':
            error = int(parts[0], 16)
    assert error, '__remill_error is not in the symbol list'
    existing = {int(x, 16) for x in re.findall(r'^extern "C" Memory \*sub_([0-9a-f]+)\(', a.traces_h.read_text(), re.M)}
    data = a.exe.read_bytes()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    sections = struct.unpack_from('<H', data, pe + 6)[0]
    optional = struct.unpack_from('<H', data, pe + 20)[0]
    base = struct.unpack_from('<Q', data, pe + 24 + 24)[0]
    found = set()
    for i in range(sections):
        o = pe + 24 + optional + i * 40
        name = data[o:o + 8].rstrip(b'\0')
        vsize, vaddr, rsize, raw = struct.unpack_from('<IIII', data, o + 8)
        if name != b'.text':
            continue
        text = np.frombuffer(data, dtype=np.uint8, count=min(vsize, rsize), offset=raw)
        start = base + vaddr
        for opcode in (0xe8, 0xe9):  # call rel32, jmp rel32
            at = np.flatnonzero(text[:-5] == opcode)
            rel = (text[at + 1].astype(np.int64) | text[at + 2].astype(np.int64) << 8 |
                   text[at + 3].astype(np.int64) << 16 | text[at + 4].astype(np.int64) << 24)
            rel = np.where(rel >= 1 << 31, rel - (1 << 32), rel)
            for site in at[start + at + 5 + rel == error]:
                # The address reaches the call in a register loaded a little earlier, so take
                # every guest code address that appears as a constant shortly before the call.
                # An extra one only becomes an extra trace head.
                window = bytes(text[max(0, site - 96):site])
                for k in range(len(window) - 3):
                    pc = struct.unpack_from('<I', window, k)[0]
                    if 0x70000000 <= pc < 0x90000000 and pc % 4 == 0:
                        found.add(pc)
    new = sorted(found - existing)
    unity = [x for x in new if x < 0x80000000]
    il2cpp = [x for x in new if x >= 0x80000000]
    Path(a.out + '-unity.json').write_text(json.dumps(unity))
    Path(a.out + '-il2cpp.json').write_text(json.dumps(il2cpp))
    print(f'{len(found)} resume points, {len(new)} are not trace heads ({len(unity)} unity, {len(il2cpp)} il2cpp)')


if __name__ == '__main__':
    main()
