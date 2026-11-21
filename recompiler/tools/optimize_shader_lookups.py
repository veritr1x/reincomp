"""Build a Windows object pack with faster Unity shader-property lookups.

Supply the input pack, its shader lookup object and the polled LLVM IR used
to build that object. The other objects are reused from the input pack.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TRACES = {
    '7035c9d0': 'aea471575f130edff118a0b37151e84fd56fd8f78df12140254cb751ce1f1929',
    '7035cbac': '3a57d6ad9a3d9a637dc828ee5cb57d5ea779278f932ad816f10c8a984caf81ff',
}
TARGET = 'x86_64-w64-windows-gnu'
DEFINITION = re.compile(r'^define ptr @sub_([0-9a-f]+)\(.*?^}', re.MULTILINE | re.DOTALL)


def specialize(function):
    body = function[0]
    trace = function[1]
    normalized = ' '.join(re.sub(r';[^\n]*', '', body).split())
    if hashlib.sha256(normalized.encode()).hexdigest() != TRACES[trace]:
        raise ValueError(f'Unrecognized shader lookup IR at {trace}')
    # The two loops search the local property sheet and its fallback. Enter
    # at their original first load and preserve their hit and miss continuations.
    loops = re.findall(r'^poll\.continue[13]:[^\n]*\n  br i1 [^,]+, label %(\d+), label %(\d+)', body, re.M)
    for ordinal, (miss, head) in enumerate(loops, 1):
        block = re.search(r'^' + head + r':[^\n]*\n(.*?)^  br i1 [^,]+, label %(\d+), label %(\d+)', body, re.S | re.M)
        insert = body.index('\n', block.start()) + 1
        fast = (f'  %scan.{ordinal} = call i32 @shader_scan(ptr %state, i64 %program_counter, '
                f'i64 {int(trace, 16)}, i64 {ordinal})\n'
                f'  %found.{ordinal} = icmp ne i32 %scan.{ordinal}, 0\n'
                f'  br i1 %found.{ordinal}, label %{block[2]}, label %{miss}\n\nold.scan.{ordinal}:\n')
        body = body[:insert] + fast + body[insert:]
    return body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('pack', 'shard-object', 'shard-ir', 'llvm-bin', 'cxx', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    pack = args.pack.resolve()
    out = args.out.resolve()
    if pack == out:
        parser.error('Choose a separate output pack directory.')
    manifest = json.loads((pack / 'pack.json').read_text())
    objects = [(pack / 'objects' / path).resolve() for path in manifest['objects']]
    old = args.shard_object.resolve()
    if objects.count(old) != 1:
        parser.error('The shader lookup object must occur once in the input pack.')
    out_objects = out / 'objects'
    out_objects.mkdir(parents=True, exist_ok=True)

    def llvm(tool, *arguments, **kwargs):
        return subprocess.run([args.llvm_bin / (tool + '.exe'), *arguments], check=True, **kwargs)

    def symbols(obj):
        result = llvm('llvm-nm', '--defined-only', obj, capture_output=True, text=True)
        return set(re.findall(r'^\w+ T (sub_[0-9a-f]+)$', result.stdout, re.M))

    def compile_ir(source, output, level):
        llvm('clang', '-target', TARGET, level, '-ffp-contract=off', '-Wno-override-module', '-c', source, '-o', output)

    with tempfile.TemporaryDirectory(dir=out) as directory:
        work = Path(directory)
        optimized = work / 'shard.bc'
        llvm('opt', '-mtriple=' + TARGET, '-passes=default<O1>', args.shard_ir, '-o', optimized)
        pair = work / 'lookups.ll'
        llvm('llvm-extract', *['--func=sub_' + pc for pc in TRACES], optimized, '-S', '-o', pair)
        ir, count = DEFINITION.subn(specialize, pair.read_text())
        if count != len(TRACES):
            raise ValueError('Missing shader lookup traces')
        # Compile the scan with the same Windows target as the two lifted callers.
        # Each caller supplies a constant trace and loop ordinal. Inlining also
        # lets it pass its live register values directly into the scan.
        helper = work / 'scan.ll'
        compiler = shutil.which(str(args.cxx))
        if compiler is None:
            parser.error(f'Cannot locate MinGW C++ compiler {args.cxx}')
        sysroot = Path(compiler).resolve().parent.parent
        llvm('clang++', '-target', TARGET, '--sysroot=' + str(sysroot), '-mno-ms-bitfields',
             '-O2', '-std=c++17', '-ffp-contract=off', '-DADDRESS_SIZE_BITS=64',
             '-I' + str(ROOT / 'third_party/remill-include'), '-emit-llvm', '-S',
             ROOT / 'game/shader_property_scan.cpp', '-o', helper)
        layout = re.search(r'^target datalayout = .+$', helper.read_text(), re.M).group()
        ir = re.sub(r'^target datalayout = .+$', layout, ir, count=1, flags=re.M)
        pair.write_text(ir + '\ndeclare i32 @shader_scan(ptr, i64, i64, i64)\n')
        llvm('opt', '-passes=verify', pair, '-disable-output')
        remaining = work / 'remaining.ll'
        def keep_other_traces(m):
            if m[1] in TRACES:
                return f'declare ptr @sub_{m[1]}(ptr, i64, ptr)'
            return m[0]
        remaining.write_text(DEFINITION.sub(keep_other_traces, args.shard_ir.read_text()))
        combined = work / 'combined.bc'
        llvm('llvm-link', pair, helper, '--internalize', '-o', combined)
        expanded = work / 'expanded.bc'
        llvm('opt', '-passes=forceattrs,always-inline,globaldce,verify',
             '-force-attribute=shader_scan:alwaysinline', combined, '-o', expanded)
        parts = [work / 'remaining.o', work / 'lookups.o']
        compile_ir(remaining, parts[0], '-O1')
        compile_ir(expanded, parts[1], '-O2')
        selected = {'sub_' + pc for pc in TRACES}
        if symbols(parts[0]) != symbols(old) - selected or symbols(parts[1]) != selected:
            raise ValueError('Replacement shard changed the set of game traces')
        replacements = []
        for obj in parts:
            digest = hashlib.sha256(obj.read_bytes()).hexdigest()
            name = f'{obj.stem}-{digest}.o'
            destination = out_objects / name
            if not destination.exists():
                os.replace(obj, destination)
            replacements.append(name)
        at = objects.index(old)
        manifest['objects'] = [p.as_posix() for p in objects[:at]] + replacements + [p.as_posix() for p in objects[at + 1:]]
        ready = work / 'pack.json'
        ready.write_text(json.dumps(manifest, indent=1) + '\n')
        os.replace(ready, out / 'pack.json')
    print(out / 'pack.json')


if __name__ == '__main__':
    main()
