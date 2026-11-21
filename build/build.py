#!/usr/bin/env python3
"""Build the game executable: one command.

    python build/build.py --pack PACK_DIR --out OUT_DIR

Compiles the host layer from this repository (build/host_sources.txt), links it
with the recompiled game objects of a pack (build/make_pack.py) and runs the
post-link helper inliner. Host objects are rebuilt only when a file they
include, or their compiler command, changed. Needs MinGW-w64 g++ (GCC 15) and
Python 3.

The pack holds only objects. What describes the game comes from game/: the
trace heads (the list of where each recompiled function starts) and three
headers; game_traces.h is written from the heads here.
"""
import argparse, concurrent.futures, json, os, shutil, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFINES = ['-DADDRESS_SIZE_BITS=64', '-DBUNKER_GAME_COOPERATIVE_POLLS=1', '-DBUNKER_GAME_COLLECTOR_DELIVERY=1']
INCLUDES = ['game', 'third_party/remill-include', 'third_party/zlib', 'third_party/angle-include']
# Game functions the host replaces: the linker routes calls to its __wrap_ versions.
WRAPS = ['sub_825b0544', 'sub_708d905c', 'sub_70a8a5c4', 'sub_70a8aa08', 'sub_703aea94']
LIBRARIES = ['atomic', 'bcrypt', 'ws2_32', 'user32', 'gdi32', 'mfplat', 'mfreadwrite', 'mfuuid', 'ole32', 'uuid',
             'advapi32', 'synchronization', 'winmm']
HEADS = ['game/heads-unity.txt', 'game/heads-il2cpp.txt']


def read_heads():
    heads = set()
    for name in HEADS:
        heads |= {int(line, 16) for line in (ROOT / name).read_text().split()}
    return sorted(heads)


def write_traces(path, heads):
    """game_traces.h: a declaration and a table row for every head, sorted by address."""
    text = ''.join(f'extern "C" Memory *sub_{h:x}(State &,addr_t,Memory *);\n' for h in heads)
    text += 'static const recomp::Trace kTraces[] = {\n' + ''.join(f'  {{{h:#x},sub_{h:x}}},\n' for h in heads) + '};\n'
    if not path.exists() or path.read_text() != text:
        path.write_text(text, newline='\n')


def stale(obj, dep):
    """True when obj is missing or older than a file in its dependency list."""
    if not obj.exists() or not dep.exists():
        return True
    built = obj.stat().st_mtime
    names = dep.read_text().replace('\\\n', ' ').split(': ', 1)[-1]
    for name in names.split():
        p = Path(name)
        if not p.exists() or p.stat().st_mtime > built:
            return True
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--pack', type=Path, help='the pack of recompiled game objects (not needed with --compile-only)')
    ap.add_argument('--spring-group', type=Path, help='directory produced by build_spring_group.py')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--cxx', default=shutil.which('g++') or 'C:/mingw64/bin/g++.exe')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--no-inline', action='store_true', help='skip the post-link helper inliner')
    ap.add_argument('--compile-only', action='store_true', help='compile the host layer and stop: needs no pack')
    ap.add_argument('--embedded-player', action='store_true', help='compile the bootstrap embedded in the portable player DLL')
    ap.add_argument('--link-plan', action='store_true', help='prepare host objects and the link response for the Windows Unity player')
    a = ap.parse_args()
    if not a.compile_only and not a.pack:
        ap.error('--pack is required unless --compile-only is given')
    out = a.out.resolve()
    (out / 'host').mkdir(parents=True, exist_ok=True)
    (out / 'generated').mkdir(exist_ok=True)
    write_traces(out / 'generated/game_traces.h', read_heads())
    tool = lambda name: str(Path(a.cxx).with_name(Path(a.cxx).name.replace('g++', name)))
    flags = ['-std=c++17', '-O2', *DEFINES, f'-I{(out / "generated").as_posix()}',
             *[f'-I{(ROOT / name).as_posix()}' for name in INCLUDES], '-Wa,-mbig-obj']
    sources = [line.strip() for line in (ROOT / 'build/host_sources.txt').read_text().splitlines()
               if line.strip() and not line.startswith('#')]
    bootstrap_flags = ['-DREINCOMP_EMBEDDED_PLAYER'] if a.embedded_player else []
    bootstrap_name = 'embedded-bootstrap.o' if a.embedded_player else 'game-bootstrap.o'
    jobs = [('host/game_bootstrap.cpp', out / 'host' / bootstrap_name, [a.cxx, *flags, *bootstrap_flags]),
            ('runtime/elfload.c', out / 'host/elfload.o', [tool('gcc'), '-O2'])]
    for i, source in enumerate(sources):
        view_flags = ['-DREINCOMP_VIEW_REVISION=0'] if a.embedded_player and source == 'host/bunker_view.cpp' else []
        jobs.append((source, out / 'host' / f'host-{i:03d}.o', [a.cxx, *flags, *view_flags]))

    def compile_one(job):
        source, obj, command = job
        dep, record = obj.with_suffix('.d'), obj.with_suffix('.cmd')
        line = ' '.join([*command, source])
        # Rebuild when an included file changed or the compiler command did.
        if not stale(obj, dep) and record.exists() and record.read_text() == line:
            return source, 0, '', False
        r = subprocess.run([*command, '-MMD', '-MF', str(dep), '-c', str(ROOT / source), '-o', str(obj)],
                           capture_output=True, text=True)
        if not r.returncode:
            record.write_text(line)
        return source, r.returncode, r.stderr, True

    t0 = time.time()
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        results = list(pool.map(compile_one, jobs))
    failed = [(s, e) for s, code, e, _ in results if code]
    for source, errors in failed:
        print(f'FAILED {source}\n{errors[-3000:]}', file=sys.stderr)
    if failed:
        sys.exit(1)
    print(f'host: {sum(1 for r in results if r[3])} compiled, {sum(1 for r in results if not r[3])} current '
          f'({time.time() - t0:.0f}s)')

    if a.compile_only:
        return
    pack = a.pack.resolve()
    manifest = json.loads((pack / 'pack.json').read_text())
    exe = out / 'bunker-game-bootstrap.exe'
    link = [*flags, '-static', *[job[1].as_posix() for job in jobs],
            *[(pack / 'objects' / name).as_posix() for name in manifest['objects']],
            (ROOT / 'third_party/zlib/libz-inflate.a').as_posix(), *[f'-Wl,--wrap={name}' for name in WRAPS],
            *[f'-l{name}' for name in LIBRARIES], '-o', exe.as_posix()]
    response = out / 'link.rsp'
    if a.spring_group:
        link.insert(link.index('-o'), '@' + (a.spring_group / 'spring.rsp').resolve(strict=True).as_posix())
    response.write_text('\n'.join(f'"{x}"' for x in link) + '\n')
    if a.link_plan:
        return
    t0 = time.time()
    subprocess.run([a.cxx, f'@{response.as_posix()}'], check=True)
    print(f'link: {time.time() - t0:.0f}s')
    if not a.no_inline:
        symbols = out / 'symbols.nm'
        with symbols.open('w') as f:
            subprocess.run([tool('nm'), '-n', '--defined-only', str(exe)], check=True, stdout=f)
        r = subprocess.run([sys.executable, str(ROOT / 'recompiler/tools/inline_lifted_helpers.py'), str(exe), '--nm',
                            str(symbols), '--fpsr-unobserved', '--all-sites', '--inline-stores'],
                           check=True, capture_output=True, text=True)
        print('inline: ' + ' | '.join(r.stdout.strip().splitlines()[-2:]))
    print(exe)


if __name__ == '__main__':
    main()
