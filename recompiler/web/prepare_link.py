"""Export only the host adapters and MinGW libraries from a native build's link plan."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


def prepare(response, cxx, out):
    base = response.parent
    lines = [line.strip('"') for line in response.read_text().splitlines()]
    out.mkdir(parents=True, exist_ok=True)
    source = {}

    def member(path):
        path = path.resolve(strict=True)
        name = path.name
        if name in source and path != source[name]:
            raise ValueError(f'Ambiguous linker member {name}')
        source[name] = path
        return '/src/' + name

    def tool_file(name):
        path = Path(subprocess.check_output([cxx, '-print-file-name=' + name], text=True).strip())
        if not path.is_absolute() or not path.is_file():
            raise ValueError(f'Missing MinGW library {name}')
        return member(path)

    args = ['-m', 'i386pep', '--shared', '-Bstatic', '-e', 'DllMainCRTStartup',
            '--no-keep-memory', '--no-insert-timestamp', '--strip-debug',
            tool_file('dllcrt2.o'), tool_file('crtbegin.o')]
    host = base.parent / 'build/host'
    special = {'follow-path.o', 'snap-scroll.o', 'walk.o', 'spring.o'}
    for item in lines[lines.index('-static') + 1:lines.index('-o')]:
        path = Path(item)
        if re.fullmatch(r'(il2cpp|unity)-shard-\d+\.o', path.name):
            if '@game' not in args:
                args.append('@game')
        elif item.startswith('@'):
            group = Path(item[1:]).read_text().splitlines()
            if Path(group[0].strip('"')).name != 'spring.o':
                raise ValueError('Unexpected guest object group')
            args.append('/src/spring.o')
            for flag in group[1:]:
                if not re.fullmatch(r'"-Wl,--wrap=sub_[a-f0-9]+,--defsym=__wrap_sub_[a-f0-9]+=sub_[a-f0-9]+_spring"', flag):
                    raise ValueError('Unexpected spring wrapper')
                args.extend(flag.strip('"')[4:].split(','))
        elif path.name in special:
            args.append('/src/' + path.name)
        elif item.startswith('-Wl,'):
            args.extend(item[4:].split(','))
        elif item.startswith('-l'):
            args.append(tool_file('lib' + item[2:] + '.a'))
        elif item == '-shared':
            continue
        elif path.parent.resolve() == host.resolve() and re.fullmatch(r'(host-\d+|elfload|runtime)\.o', path.name):
            args.append(member(path))
        elif path.resolve() in [(base/name).resolve() for name in ('embedded-bootstrap.o', 'runtime-host.o')]:
            args.append(member(path))
        elif path.resolve() == Path(__file__).resolve().parents[2] / 'third_party/zlib/libz-inflate.a':
            args.append(member(path))
        else:
            raise ValueError(f'Unrecognized release linker input {item}')
    for name in ('stdc++', 'mingw32', 'gcc', 'gcc_eh', 'mingwex', 'msvcrt', 'kernel32', 'pthread',
                 'advapi32', 'shell32', 'user32', 'kernel32', 'mingw32', 'gcc', 'gcc_eh', 'mingwex', 'msvcrt', 'kernel32'):
        args.append(tool_file(f'lib{name}.a'))
    args.append(tool_file('crtend.o'))
    hashes = {}
    for name, path in source.items():
        with path.open('rb') as stream:
            hashes[name] = hashlib.file_digest(stream, 'sha256').hexdigest()
        shutil.copyfile(path, out/name)
    (out/'runtime-link.json').write_text(json.dumps({'arguments': args, 'files': hashes}, separators=(',', ':')))


if __name__ == '__main__':
    cli = argparse.ArgumentParser(description=__doc__)
    for name in ('response','cxx','out'):
        cli.add_argument('--'+name, type=Path, required=True)
    prepare(**vars(cli.parse_args()))
