import csv
import hashlib
import re
from pathlib import Path
import shutil
import subprocess
import sys
from metadata_accessors import lower
from follow_path import build as build_follow_path, build_scroll
from liveness import build as build_liveness

root = Path(__file__).resolve().parent
bench = root.parent
repo = bench.parent
test = root / 'runtime-player'
test.mkdir(exist_ok=True)
original = root / 'host-probe'
if not (test/'UnityPlayer.dll').exists():
    shutil.copy2(original/'UnityPlayer.dll', test/'UnityPlayer.dll')
if not (test / 'WindowsPlayer_Data').exists():
    shutil.copytree(original / 'WindowsPlayer_Data', test / 'WindowsPlayer_Data')
for first in sorted((test/'WindowsPlayer_Data').rglob('*.split0')):
    joined = first.with_suffix('')
    chunks = sorted(first.parent.glob(joined.name+'.split*'),key=lambda path:int(path.suffix[6:]))
    if [path.suffix for path in chunks] != [f'.split{i}' for i in range(len(chunks))]:
        raise RuntimeError(f'Noncontiguous APK chunks for {joined.name}')
    if joined.exists():
        continue
    partial = joined.with_suffix(joined.suffix+'.partial')
    with partial.open('wb') as output:
        for chunk in chunks:
            with chunk.open('rb') as source:
                shutil.copyfileobj(source,output)
    partial.replace(joined)

compiler = 'C:/mingw64/bin/g++.exe'
sys.path.insert(0, str(repo/'build'))
from native_release import build_player_entry
build_player_entry(Path(compiler), original/'WindowsPlayer.exe', test/'WindowsPlayer.exe', root)
def run(*args):
    subprocess.run(args, cwd=repo, check=True)

if 'adapter-only' not in sys.argv:
    with (bench/'emu-client/data/native/libil2cpp.so').open('rb') as image:
        image.seek(0x254da00)
        if image.read(8)!=bytes.fromhex('220000f9c0035fd6'):
            raise RuntimeError('Managed field setter is no longer the audited store and return')
        image.seek(0)
        digest=hashlib.sha256()
        for part in iter(lambda:image.read(1<<20),b''):
            digest.update(part)
        if digest.hexdigest()!='ff5c66ad2fdaa957fa005f1957988b3277331abefcdedcdb0c82e59ba2e80522':
            raise RuntimeError('The field optimizations require the verified game binary and its type registration')
    inventory=(repo/'work/windows-unity-icalls/icall_inventory.h').read_text()
    bindings=re.findall(r'\{"(UnityEngine\.AndroidJNI(?:Helper)?::[^"]+)",(0x[\da-f]+)ULL,0x[\da-f]+ULL,false\}',inventory)
    if len(bindings)<100:raise RuntimeError('The original Android JNI registration inventory is incomplete')
    (root/'original-android-jni.inc').write_text(''.join('{"%s",%s},\n'%entry for entry in bindings))
    args = (bench / 'main-bridge-mutex/control/link.rsp').read_text().splitlines()
    flags = [v.strip('"') for v in args[:args.index('"-static"')]]
    obj = root / 'runtime-host.o'
    run(compiler, *flags, '-c', root / 'runtime-host.cpp', '-o', obj)
    bootstrap=root/'embedded-bootstrap.o'
    dependencies=[repo/'host/game_bootstrap.cpp',repo/'host/game_collector_delivery.h',repo/'host/avpro_video.h',
                  repo/'host/movie_audio.h',repo/'host/opensles_audio.h',repo/'runtime/elfload.h',repo/'runtime/fast_tls.h']
    if not bootstrap.exists() or any(p.stat().st_mtime>bootstrap.stat().st_mtime for p in dependencies):
        run(compiler,*flags,'-DREINCOMP_EMBEDDED_PLAYER','-c',dependencies[0],'-o',bootstrap)
    old,=[arg for arg in args if arg.endswith('/main-bridge-mutex/bootstrap.o"')]
    args[args.index(old)]=f'"{bootstrap.as_posix()}"'
    loader=root/'elfload.o'
    run(Path(compiler).with_name('gcc.exe'),'-O2','-c',repo/'runtime/elfload.c','-o',loader)
    previous,=[arg for arg in args if arg.endswith('/elfload.o"')]
    args[args.index(previous)]=f'"{loader.as_posix()}"'
    memory_runtime=root/'memory-runtime.o'
    runtime_deps=[repo/'runtime/runtime.cpp',*sorted((repo/'runtime').glob('*.h'))]
    if not memory_runtime.exists() or any(p.stat().st_mtime>memory_runtime.stat().st_mtime for p in runtime_deps):
        run(compiler,*flags,'-c',runtime_deps[0],'-o',memory_runtime)
    previous,=[arg for arg in args if arg.endswith('/main-reservation-readers/release/runtime.o"')]
    args[args.index(previous)]=f'"{memory_runtime.as_posix()}"'
    sources = [line.strip() for line in (repo/'build/host_sources.txt').read_text().splitlines() if line.strip() and not line.startswith('#')]
    for source in ('host/pthread_host.cpp','host/filesystem.cpp','host/jni_host.cpp'):
        index=sources.index(source)
        replacement=root/('runtime-'+Path(source).stem+'.o')
        if not replacement.exists() or replacement.stat().st_mtime<(repo/source).stat().st_mtime:
            run(compiler,*flags,'-c',repo/source,'-o',replacement)
        previous, = [arg for arg in args if arg.endswith(f'/host-{index:03d}.o"')]
        args[args.index(previous)] = f'"{replacement.as_posix()}"'
    path_method = build_follow_path(root, bench/'emu-client/data/native/libil2cpp.so')
    scroll_method = build_scroll(root, bench/'emu-client/data/native/libil2cpp.so')
    walkers = build_liveness(root, bench/'emu-client/data/native/libil2cpp.so')
    args[-2:] = ['"-shared"', '"-Wl,--exclude-all-symbols,--image-base,0x240000000"',
        '"-Wl,--wrap=sub_836175ec"',
        '"-Wl,--wrap=_ZN6bunker7windows14ModuleRegistry4OpenEPKci,--wrap=_ZN6bunker7windows8Pthreads3RunEySt16initializer_listIyE,--wrap=_ZN6bunker7windows8Pthreads7InstallEv,--wrap=_ZN6recomp7Runtime9AddTracesEPKNS_5TraceEy,--wrap=_ZN6bunker7windows10SignalHost11BindCurrentEy,--wrap=_ZN6recomp7Runtime9AddImportEySt8functionIFvR5StateEE,--wrap=_ZN6recomp21PublishGuestOverridesEv"',
        f'"{obj.as_posix()}"', f'"{path_method.as_posix()}"', f'"{scroll_method.as_posix()}"', f'"{walkers.as_posix()}"', '"-o"', f'"{(test / "GuestRuntime.dll").as_posix()}"']
    rsp = root / 'runtime-link.rsp'
    rsp.write_text('\n'.join(args) + '\n')
    run(compiler, '@' + str(rsp))
    run(sys.executable,root/'route-liveness-stop.py',test/'GuestRuntime.dll')

source = lower((root / 'runtime-adapter.cpp').read_text(),
               bench/'emu-client/data/native/libil2cpp.so', root/'player-apis.csv')
exports = [item['windows_player_name'] for item in csv.DictReader((root/'player-apis.csv').open())
           if item['translated_entry'] == 'True']
assert len(exports) == 222
for name in exports:
    if f' {name}(' not in source and f'({name},' not in source:
        source += f'\nextern "C" __declspec(dllexport) void {name}() {{ boundary("{name}"); }}\n'
(root/'runtime-adapter.generated.cpp').write_text(source)
run(compiler, '-std=c++17', '-O2', '-static', '-shared', root/'runtime-adapter.generated.cpp',
    repo/'host/bunker_view.cpp','-lole32','-luuid','-lcomctl32','-o', test/'GameAssembly.dll')
