"""Build and publish a replacement for a known Windows render shard."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

DEFINITION = re.compile(r'^define [^\n]*@sub_([0-9a-f]+)\([^\n]*\{.*?^}', re.M | re.S)
FLAGS = ['-target', 'x86_64-w64-windows-gnu', '-ffp-contract=off', '-Wno-override-module']


def build_pack(description, shard_hash, compile_shard, *, allow_inline_stores=False):
    parser = argparse.ArgumentParser(description=description)
    for name in ('pack', 'shard-object', 'shard-ir', 'llvm-bin', 'out'):
        parser.add_argument('--' + name, required=True, type=Path)
    if allow_inline_stores:
        parser.add_argument('--inline-stores', action='store_true',
                            help='Use the release plain-store fast path in shader parameter preparation.')
    args = parser.parse_args()
    pack, out = args.pack.resolve(), args.out.resolve()
    if pack == out:
        parser.error('Choose a separate output pack directory.')
    manifest = json.loads((pack / 'pack.json').read_text())
    objects = [(pack / 'objects' / obj).resolve() for obj in manifest['objects']]
    old = args.shard_object.resolve()
    if objects.count(old) != 1:
        parser.error('Expected the render shard once in the input pack.')
    if hashlib.sha256(args.shard_ir.read_bytes()).hexdigest() != shard_hash:
        parser.error('Unrecognized render shard IR. The supported input is the global 3.7.1 polled Unity shard.')
    compiler = args.llvm_bin.resolve() / 'clang.exe'
    published = out / 'objects'
    published.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out) as directory:
        work = Path(directory)

        def compile_ir(source, dest, level, *mode):
            subprocess.run([compiler, *FLAGS, level, *mode, source, '-o', dest], check=True)

        options = {'inline_stores': args.inline_stores} if allow_inline_stores else {}
        parts, expected = compile_shard(args.shard_ir, work, compile_ir, **options)
        def entries(obj):
            table = subprocess.check_output([compiler.with_name('llvm-nm.exe'), '--defined-only', obj], text=True)
            return set(re.findall(r'^\w+ T (sub_[0-9a-f]+)$', table, re.M))

        original = entries(old)
        replaced = set().union(*expected)
        if not replaced <= original or entries(parts[0]) != original - replaced or any(
                entries(obj) != names for obj, names in zip(parts[1:], expected, strict=True)):
            raise ValueError('Render objects do not reproduce the supplied shard entry points.')
        replacements = []
        for obj in parts:
            digest = hashlib.sha256(obj.read_bytes()).hexdigest()
            name = f'{obj.stem}-{digest}.o'
            os.replace(obj, published / name)
            replacements.append(name)
        i = objects.index(old)
        manifest['objects'] = [p.as_posix() for p in objects[:i]] + replacements + [p.as_posix() for p in objects[i+1:]]
        ready = work / 'pack.json'
        ready.write_text(json.dumps(manifest, indent=1) + '\n')
        os.replace(ready, out / 'pack.json')
    print(out / 'pack.json')
