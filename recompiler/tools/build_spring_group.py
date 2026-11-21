"""Build the Windows spring-animation object and its GNU linker response file.

Pass the patched IL2CPP image, original trace heads, Remill lifter and semantics
directory, poll instrumenter, LLVM clang, and a new output directory. Include
spring.rsp in the normal link, replacing any previous wrappers for these entries.
"""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile


IMAGE_HASHES = {
    '5d59352f9da76f27646d225b2e8fb0551501e30c56e5c49a0d19332cfe283b0d',
    # Same 78 animation functions in the Bunker 1.12.13 Global image.
    'f45788400605cc7e5873e3815ca5dddf5648d0e53cbac8fafb01e8a610b98ad1',
    # Global release with Bunker's capacity, countdown and camera fixes.
    # The byte ranges of all 78 spring entries match the image above.
    '74c9f2a705a245c3f31471962789663ab32d8a74d900afd8410722b929f7638e',
}
DEFINITION = re.compile(r'^(define ptr @sub_([0-9a-f]+)_spring\([^\n]*\)) \{(.*?)\n\}', re.M | re.S)


def build(args):
    digest = hashlib.sha256()
    with args.image.open('rb') as image:
        for chunk in iter(lambda: image.read(1024 * 1024), b''):
            digest.update(chunk)
    if digest.hexdigest() not in IMAGE_HASHES:
        raise ValueError('IL2CPP image differs from the profiled patched build')
    names = {f'sub_{int(s, 16):x}' for s in args.entries.read_text().splitlines()}
    if args.output.exists():
        raise FileExistsError(args.output)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.output.parent) as scratch:
        work = Path(scratch)
        raw, poll = work / 'raw.ll', work / 'poll.ll'
        subprocess.run([args.lifter, '--arch', 'aarch64', '--os', 'linux', '--semantics_dir', args.semantics,
                        '--bytes_file', args.image, '--address', str(0x80000000), '--entry_addresses_file',
                        args.entries, '--trace_heads_file', args.heads, '--guest_call_transfer=true', '--ir_out', raw], check=True)
        subprocess.run([args.instrumenter, '--speed', '--inline-poll=teb', '--direct-calls', raw, poll], check=True)
        ir = poll.read_text()
        if set(re.findall(r'^define ptr @(sub_[0-9a-f]+)\(', ir, re.M)) != names:
            raise ValueError('Lifted entries differ from the spring group')
        ir = re.sub(r'@(sub_[0-9a-f]+)\b', lambda m: m[0] + '_spring' if m[1] in names else m[0], ir)

        def inline_leaf(match):
            header, _, body = match.groups()
            if len(body.splitlines()) > 650 or re.search(r'(?:call|invoke).*@(?:sub_|__bunker_guest_call\()', body):
                return match[0]
            return header + ' alwaysinline {' + body + '\n}'

        group = work / 'group.ll'
        group.write_text(DEFINITION.sub(inline_leaf, ir))
        output = work / 'output'
        output.mkdir()
        subprocess.run([args.clang, '-target', 'x86_64-w64-windows-gnu', '-O2', '-ffp-contract=off',
                        '-Wno-override-module', '-c', group, '-o', output / 'spring.o'], check=True)
        flags = [f'"{(args.output.resolve() / "spring.o").as_posix()}"']
        flags.extend(f'"-Wl,--wrap={n},--defsym=__wrap_{n}={n}_spring"' for n in sorted(names))
        (output / 'spring.rsp').write_text('\n'.join(flags) + '\n')
        output.rename(args.output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'heads', 'lifter', 'semantics', 'instrumenter', 'clang', 'output','entries'):
        parser.add_argument('--' + name, type=Path, required=True)
    build(parser.parse_args())
