"""Compile the Windows player's patched navigation and scroll methods."""
import re
import struct
import subprocess
from pathlib import Path


def build(root, image, tools=None):
    return lift_method(root, image, tools, 'follow-path', 0x830cfc3c, 0x830d0010, 'StartFollowPathRelease')


def build_scroll(root, image, tools=None):
    return lift_method(root, image, tools, 'snap-scroll', 0x830df4e8, 0x830df69c, 'UpdateSnapPositionBunker')


def lift_method(root, image, tools, directory, entry, end, symbol):
    bench = root.parent
    work = root / directory
    work.mkdir(exist_ok=True)
    output = work / (directory+'.o')
    if tools is None:
        clang, = (bench.parent / 'work/dl/llvm18').glob('*/bin/clang.exe')
        tools = (bench/'managed-hot/all.heads', bench/'population/remill-lift.exe', bench/'packed-vector/poll-vector.exe', clang)
    heads, lifter, poll, clang = tools
    dependencies = (image, Path(__file__), heads, lifter, poll, clang)
    if output.exists() and all(p.stat().st_mtime < output.stat().st_mtime for p in dependencies):
        return output

    with image.open('rb') as original:
        original.seek(entry - 0x80000000)
        code = bytearray(original.read(end - entry))
    # The path, threshold and IsFollowing flag are already committed. The block
    # boxes scalars, converts every Vector3 to a string and calls String.Format.
    # Its result in x0 is overwritten immediately. Keep the final class-init
    # check, both return values, every bounds check and the original epilogue.
    if directory=='follow-path':
        skip, resume = 0x830cfe74, 0x830cffb0
        if (struct.unpack_from('<II', code, skip-entry-4) != (0x39011674, 0xf0021089)
                or struct.unpack_from('<II', code, resume-entry-4) != (0x944d1dbc, 0xf9400340)):
            raise ValueError('The original path formatting block changed')
        struct.pack_into('<I', code, skip-entry, 0x14000000 | ((resume-skip)//4))
    else:
        # Bunker display_patch.c, kSnapArrive. At high frame rates a long
        # banner list can stop within one layout unit and never enable taps.
        offset = 0x830df5a0-entry
        if struct.unpack_from('<I',code,offset)[0]!=0xbd401900:
            raise ValueError('The original scroll arrival check changed')
        struct.pack_into('<I',code,offset,0x1e2e1000)
    (work / 'method.bin').write_bytes(code)
    (work / 'entry.txt').write_text(hex(entry) + '\n')
    with (work / 'build.log').open('w') as log:
        def run(*args):
            subprocess.run(args, check=True, stdout=log, stderr=subprocess.STDOUT)

        run(lifter, '--arch', 'aarch64', '--os', 'linux', '--semantics_dir', lifter.parent,
            '--bytes_file', work / 'method.bin', '--address', str(entry),
            '--entry_addresses_file', work / 'entry.txt', '--trace_heads_file', heads,
            '--guest_call_transfer=true', '--ir_out', work / 'method.ll')
        run(poll, '--speed', '--inline-poll=teb', '--direct-calls', work / 'method.ll', work / 'method.poll.ll')
        ir = (work / 'method.poll.ll').read_text()
        name = f'sub_{entry:x}'
        if re.findall(r'^define ptr @(sub_\w+)\(', ir, re.M) != [name]:
            raise ValueError(f'Unexpected method definitions for {symbol}')
        if directory=='follow-path' and any('@sub_' + pc in ir for pc in ('8377b814', '84411e08', '8441769c')):
            raise ValueError('Unused path formatting is still reachable')
        if directory=='follow-path' and not all('@sub_' + pc in ir for pc in ('853d03b4', '853d1dc4', '830d0010', '830d0130')):
            raise ValueError('Expected navigation calls missing from path method')
        (work / 'method.poll.ll').write_text(ir.replace('@' + name + '(', '@'+symbol+'('))
        temporary = output.with_suffix('.tmp.o')
        run(clang, '-target', 'x86_64-w64-windows-gnu', '-O2', '-ffp-contract=off',
            '-Wno-override-module', '-c', work / 'method.poll.ll', '-o', temporary)
        temporary.replace(output)
    return output
