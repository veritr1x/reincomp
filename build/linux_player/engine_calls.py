"""Recover Unity internal-call names and Linux RVAs from the pinned player module."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

from stage import UNITY_FILES, UNITY_SYMBOLS_SHA256

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "recompiler/tools"))
from lift_new_traces import load_image


def registrations(module):
    player, debug = module / "UnityPlayer.so", module / "UnityPlayer_s.debug"
    for path, expected in ((player, UNITY_FILES["UnityPlayer.so"]),
                           (debug, UNITY_SYMBOLS_SHA256)):
        with path.open("rb") as stream:
            if hashlib.file_digest(stream, "sha256").hexdigest() != expected:
                raise ValueError(f"Expected the Unity 2019.4.29f1 Linux module: {path}")
    symbols = subprocess.check_output(["nm", "-C", "-S", "--defined-only", debug], text=True)
    funcs, tables = {}, {}
    for row in symbols.splitlines():
        parts = row.split(maxsplit=3)
        if len(parts) != 4:
            continue
        addr, size, kind, name = parts
        if kind in ("t", "T", "W", "w"):
            funcs[int(addr, 16)] = name
        elif name.endswith(("_IcallNames", "_IcallFuncs")):
            tables[name] = (int(addr, 16), int(size, 16))
    registrar, = [addr for addr, name in funcs.items() if name == "scripting_add_internal_call(char const*, void const*)"]
    image, base, executable = load_image(player)
    if base != 0:
        raise ValueError("Unexpected Linux Unity image base")
    calls = {}

    def register(name_addr, func):
        end = image.find(b"\0", name_addr, name_addr + 2048)
        if end < 0 or func not in funcs:
            raise ValueError(f"Invalid Unity binding at {name_addr:#x}, {func:#x}")
        name = image[name_addr:end].decode("ascii")
        if "::" not in name or calls.setdefault(name, func) != func:
            raise ValueError(f"Invalid or conflicting Unity internal call: {name}")

    def rip(operand):
        return operand + 4 + struct.unpack_from("<i", image, operand)[0]

    # Each generated export loads the callback name into RDI and the function
    # into RSI. Tail calls pop the saved stack slot before jumping.
    table_sites = []
    for lo, hi in executable:
        for match in re.finditer(b"[\xe8\xe9]", image[lo:hi - 4]):
            pc = lo + match.start()
            if rip(pc + 1) != registrar:
                continue
            end = pc
            if image[pc-1:pc+1] == b"\x58\xe9":  # pop rax followed by a tail jump
                end -= 1
            if image[end-14:end-11] == b"\x48\x8d\x3d" and image[end-7:end-4] == b"\x48\x8d\x35":
                register(rip(end - 11), rip(end - 4))
            elif image[pc-8:pc] == b"\x4a\x8b\x3c\x33\x4a\x8b\x34\x3b":
                table_sites.append(pc)
            else:
                raise ValueError(f"Unrecognized registration site at {pc:#x}")

    # The two older exporters use arrays of ELF-relative pointers. The
    # compare in each loop supplies its byte count (without the sentinel).
    relocs = subprocess.check_output(["readelf", "-rW", player], text=True)
    relative = {int(at, 16): int(val, 16) for at, val in re.findall(
        r"^([0-9a-f]+)\s+[0-9a-f]+\s+R_X86_64_RELATIVE\s+([0-9a-f]+)$", relocs, re.M)}
    used = set()
    for pc in table_sites:
        names, targets = rip(pc - 30), rip(pc - 23)
        count, = struct.unpack_from("<I", image, pc + 12)
        prefix, = [s[:-len("_IcallNames")] for s, data in tables.items()
                   if s.endswith("_IcallNames") and data == (names, count + 8)]
        if tables[prefix + "_IcallFuncs"] != (targets, count + 8) or count % 8:
            raise ValueError(f"Unexpected binding arrays for {prefix}")
        for offset in range(0, count, 8):
            register(relative[names + offset], relative[targets + offset])
        used.update((prefix + "_IcallNames", prefix + "_IcallFuncs"))
    if used != tables.keys() or len(calls) != 7718:
        raise ValueError(f"Incomplete Linux Unity binding inventory ({len(calls)} names)")
    return calls


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--unity", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    result = registrations(args.unity)
    args.out.write_text(json.dumps(result, indent=1, sort_keys=True) + "\n")
    print(f"{len(result)} Linux Unity internal calls")
