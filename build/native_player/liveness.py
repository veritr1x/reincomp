"""Compile the pinned IL2CPP liveness walkers together with sparse bitmap iteration."""
from pathlib import Path
import re
import subprocess


def build(root, image, tools=None):
    bench = root.parent
    first, end = 0x8257d5ac, 0x8257dfa4
    directory = root / 'liveness'
    directory.mkdir(exist_ok=True)
    output = directory / 'walk.o'
    if tools is None:
        clang, = (bench.parent / 'work/dl/llvm18').glob('*/bin/clang.exe')
        tools = (bench/'managed-hot/all.heads', bench/'population/remill-lift.exe', bench/'packed-vector/poll-vector.exe', clang)
    heads, lifter, poll, clang = tools
    assembler = clang.with_name('llvm-mc.exe')
    copy = clang.with_name('llvm-objcopy.exe')
    deps = (image, Path(__file__), heads, lifter, poll, clang, assembler, copy)
    if output.exists() and all(p.stat().st_mtime < output.stat().st_mtime for p in deps):
        return output
    entries = [line for line in heads.read_text().splitlines() if first <= int(line, 16) < end]
    names = {f'sub_{int(line, 16):x}' for line in entries}
    (directory / 'entries').write_text('\n'.join(entries) + '\n')
    with image.open('rb') as original:
        original.seek(first - 0x80000000)
        code = bytearray(original.read(end - first))
    # The old loop tests all 62 bits. CLZ chooses the next reference in the
    # same order. Keep BL and the saved registers at their original addresses.
    source = """
        mov x23, #0x8000000000000000
        and x8, x8, #0xfffffffffffffffe
        ldr x21, [x8, #8]
        and x21, x21, #0xfffffffffffffffc
        cbz x21, done
    next:
        clz x22, x21
        nop
        ldr x0, [x20, x22, lsl #3]
        mov x1, x19
        .inst 0x940000a5
        lsr x8, x23, x22
        bic x21, x21, x8
        cbnz x21, next
    done:
        cmp xzr, xzr
    """
    with (directory / 'build.log').open('w') as log:
        def run(*args):
            subprocess.run(args, cwd=directory, check=True, stdout=log, stderr=subprocess.STDOUT)

        (directory / 'loop.s').write_text(source)
        run(assembler, '--triple=aarch64', '--filetype=obj', 'loop.s', '-o', 'loop.o')
        run(copy, '-O', 'binary', '--only-section=.text', 'loop.o', 'loop.bin')
        loop = (directory / 'loop.bin').read_bytes()
        length = 0x8257d768 - 0x8257d730
        if len(loop) != length or loop[0x24:0x28] != bytes.fromhex('a5000094'):
            raise ValueError('The liveness visitor call moved')
        code[0x8257d730-first:0x8257d768-first] = loop
        (directory / 'walk.bin').write_bytes(code)
        run(lifter, '--arch', 'aarch64', '--os', 'linux', '--semantics_dir', lifter.parent,
            '--bytes_file', 'walk.bin', '--address', str(first), '--entry_addresses_file', 'entries',
            '--trace_heads_file', heads, '--guest_call_transfer=true', '--ir_out', 'raw.ll')
        run(poll, '--speed', '--inline-poll=teb', '--direct-calls', 'raw.ll', 'walk.ll')
        ir = (directory / 'walk.ll').read_text()
        if set(re.findall(r'^define ptr @(sub_[0-9a-f]+)\(', ir, re.M)) != names:
            raise ValueError('Unexpected liveness traversal entry points')
        # Keep the original cooperative poll before the bounded native path.
        marker = re.compile(r'(^define ptr @sub_8257d9e8\([^\n]+\n(?:(?!^\}).)*?^poll\.continue:[^\n]*\n)', re.M | re.S)
        ir, count = marker.subn(lambda m: m[0] + """
  %marked.in.place = call zeroext i1 @MarkLivenessInPlace(ptr %state)
  br i1 %marked.in.place, label %mark.done, label %mark.original
mark.done:
  ret ptr %memory
mark.original:
""", ir)
        if count != 1:
            raise ValueError('Missing lifted liveness visitor entry poll')
        ir += '\ndeclare zeroext i1 @MarkLivenessInPlace(ptr)\n'
        ir = re.sub(r'@(sub_[0-9a-f]+)\b', lambda m: m[0] + '_liveness' if m[1] in names else m[0], ir)
        (directory / 'walk.ll').write_text(ir)
        run(clang, '-target', 'x86_64-w64-windows-gnu', '-O2', '-ffp-contract=off',
            '-Wno-override-module', '-c', 'walk.ll', '-o', 'walk.tmp.o')
        (directory / 'walk.tmp.o').replace(output)
    return output
