"""Build the project-owned components used by the portable Windows player."""
from argparse import Namespace
from contextlib import contextmanager
import io
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from zipfile import ZipFile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
NATIVE = HERE/'native_player'
WEB = ROOT/'recompiler/web'
sys.path[:0] = [str(HERE), str(NATIVE)]
import build as host_build
import native_release
from compile_player import compile_adapter, compile_fsr
from portable_inputs import BUNKER, PORTABLE, download, digest

PROTOC = ('protoc-31.1-win64.zip', '70381b116ab0d71cb6a5177d9b17c7c13415866603a0fd40d513dafe32d56c35')


@contextmanager
def environment(**settings):
    previous = {key:os.environ.get(key) for key in settings}
    os.environ.update({k:str(v) for k,v in settings.items()})
    try:
        yield
    finally:
        for key, value in previous.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


def run(*cmd, **opts):
    subprocess.run([str(arg) for arg in cmd], check=True, **opts)


def public_bunker(work):
    bunker = work/('bunker-'+BUNKER['release'])
    if not bunker.exists():
        run('git','-c','core.autocrlf=false','clone','--config','core.autocrlf=false','--branch',BUNKER['release'],'--depth','1',
            '--recurse-submodules',BUNKER['repository'],bunker)
    revision = subprocess.check_output(['git','rev-parse','HEAD'],cwd=bunker,text=True).strip()
    if revision != BUNKER['commit']:
        raise ValueError('Bunker source revision differs from the release')
    return bunker


def build_go(bunker, work, go, game):
    """Build the Go programs, including upstream protobuf generation."""
    protoc = work/'protoc/bin/protoc.exe'
    with ZipFile(download(work, PROTOC, 'https://github.com/protocolbuffers/protobuf/releases/download/v31.1/')) as zip_file:
        protoc.parent.mkdir(parents=True, exist_ok=True)
        protoc.write_bytes(zip_file.read('bin/protoc.exe'))
        for name in zip_file.namelist():
            if name.startswith('include/google/protobuf/') and name.endswith('.proto'):
                zip_file.extract(name,work/'protoc')
    server = game.parent/'server/bunker-server.exe'
    server.parent.mkdir(parents=True, exist_ok=True)
    env_path = os.pathsep.join([str(protoc.parent),str(go.parent),os.environ['PATH']])
    with environment(PATH=env_path, GOTOOLCHAIN='local', CGO_ENABLED='0', GOWORK='off',
                     GOCACHE=work/'go-build-cache', GOMODCACHE=work/'go-mod-cache'):
        version = subprocess.check_output([go,'version'],text=True).strip()
        if version != 'go version go1.25.8 windows/amd64':
            raise ValueError('Expected Go 1.25.8 for Windows x64, found '+version)
        source = native_release.archive_server_source(Namespace(bunker=None,no_download=False), bunker, work, go)
        native_release.build_windows_server(go,source,work,server,os.environ.copy())
        native_release.build_archive_helper(go,source,work,game,os.environ.copy())
        run(go,'build','-trimpath','-buildvcs=false','-ldflags','-H=windowsgui','-o',work/'launcher.exe','.',cwd=ROOT/'launcher')


def read_parts(directory):
    index = json.loads((directory/'player.json').read_bytes())
    return ZipFile(io.BytesIO(b''.join((directory/name).read_bytes() for name,_ in index['parts'])))


def dependencies(windows, release):
    """Extract only the redistributable vendor files, licenses and upstream Pod Programs."""
    vendor = {'game/FirebaseCppApp-9_6_0.dll','game/grpc_csharp_ext.x64.dll','game/amd_fidelityfx_vk.dll'}
    with read_parts(windows) as zip_file:
        for name in zip_file.namelist():
            if not (name in vendor or name.startswith(('licenses/','game/angle/','pods/'))):
                continue
            dest = (release/name).resolve()
            if not dest.is_relative_to(release.resolve()):
                raise ValueError('Unexpected player dependency '+name)
            dest.parent.mkdir(parents=True,exist_ok=True)
            with zip_file.open(name) as src, dest.open('wb') as out:
                shutil.copyfileobj(src,out)


def build_native(a, work, bunker, release, abi):
    game = release/'game'
    host = work/'host-build'
    run(sys.executable,HERE/'build.py','--compile-only','--embedded-player','--cxx',a.cxx,'--out',host,'--jobs',a.jobs)
    flags = ['-std=c++17','-O2',*host_build.DEFINES,'-I'+str(host/'generated'),
             *['-I'+str(ROOT/p) for p in host_build.INCLUDES],'-Wa,-mbig-obj','-I'+str(abi),'-I'+str(NATIVE)]
    run(a.cxx,*flags,'-c',NATIVE/'runtime-host.cpp','-o',host/'host/runtime-host.o')
    with environment(SOURCE_DATE_EPOCH='1795206585'):
        run(a.cxx,*flags,'-shared','-static','-Wl,--image-base,0x387690000',NATIVE/'engine-calls.cpp',
            ROOT/'host/windows_shared_preferences.cpp','-lshlwapi','-lbcrypt','-ladvapi32','-lxinput9_1_0',
            '-ldinput8','-ldxguid','-o',game/'EngineCalls.dll')
    with environment(SOURCE_DATE_EPOCH='1795206749'):
        compile_fsr(work,game,a.cxx,['-Wl,--image-base,0x1c6120000'],
                    'C__GitRepos_reincomp__fps_bench_windows_web_20261121_libvulkan_import.a')
    with environment(SOURCE_DATE_EPOCH='1795227079'):
        run(a.cxx,'-std=c++17','-O2','-static','-municode','-mwindows',NATIVE/'resource-window.cpp',
            ROOT/'host/bunker_view.cpp','-lole32','-luuid','-lcomdlg32','-lshell32','-o',game/'resource-window.exe')
    with ZipFile(a.apk) as apk:
        metadata = work/'global-metadata.dat'
        metadata.write_bytes(apk.read('assets/bin/Data/Managed/Metadata/global-metadata.dat'))
    run(release/'pods/python/python.exe','-I',NATIVE/'package_assets.py',a.apk,release,metadata)
    original = release/'data/native/libil2cpp.so'
    with environment(SOURCE_DATE_EPOCH='1795209980'):
        compile_adapter(work,game,a.cxx,[*flags,'-DREINCOMP_VIEW_REVISION=1','-Wl,--image-base,0x2da8e0000'],original,bunker,abi)


def prepare_site(a, work, windows, bunker, release):
    plan = native_release.module('portable_game_plan', WEB/'game_plan.py')
    plan.prepare(a.apk,bunker,a.cxx,windows)
    for path in ('compiler.mjs','build-objects.mjs','compile-library.mjs','toolchain.mjs','translate.mjs','apk.mjs',
                 'shard-worker.mjs','gnu-link.mjs','player-methods.mjs','liveness-edge.mjs','package-exe.mjs',
                 'master-worker.js','master_digest.py'):
        shutil.copyfile(WEB/path,windows/path)
    shutil.copyfile(bunker/'android/app/src/main/python/patcher/patch_masterdata.py',windows/'patch_masterdata.py')
    link = windows/'link'
    recipe = json.loads((link/'runtime-link.json').read_bytes())
    overrides = PORTABLE.get('hostObjects', {})
    if any(name not in recipe['files'] or
           not name.startswith(('host-','runtime-host.','elfload.','embedded-bootstrap.')) for name in overrides):
        raise ValueError('Unknown project-owned object in the portable release lock')
    for name, expected in recipe['files'].items():
        if name.startswith(('host-','runtime-host.','elfload.','embedded-bootstrap.')):
            source = work/'host-build/host'/name
            expected = overrides.get(name, expected)
            if digest(source) != expected:
                raise ValueError('Rebuilt linker input differs from the reference: '+name)
            shutil.copyfile(source,link/name)
            recipe['files'][name] = expected
    if overrides:
        (link/'runtime-link.json').write_bytes(json.dumps(recipe,separators=(',',':')).encode())
    game = release/'game'
    new = work/'player-kit'
    run(release/'pods/python/python.exe','-I',WEB/'prepare_player.py','--apk',a.apk,'--bunker',bunker,
        '--release',release,'--unity',a.unity_player,'--launcher',work/'launcher.exe',
        '--window',game/'resource-window.exe','--archive',game/'archive-media.exe',
        '--server',release/'server/bunker-server.exe','--emblem',work/'bunker_icon.h','--out',new)
    with read_parts(windows) as ref, read_parts(new) as rebuilt:
        if set(ref.namelist()) != set(rebuilt.namelist()):
            raise ValueError('Unexpected player package inventory')
        errors=[]
        for name in ref.namelist():
            content=rebuilt.read(name)
            if name in PORTABLE['playerMembers']:
                matches=hashlib.sha256(content).hexdigest()==PORTABLE['playerMembers'][name]
            else:
                matches=content==ref.read(name)
            if not matches:errors.append(name)
        if errors:
            raise ValueError('Rebuilt player members differ from the reference: '+', '.join(errors))
    for item in new.iterdir():
        shutil.copyfile(item,windows/item.name)
    manifest = windows/'kit.json'
    index = json.loads(manifest.read_bytes())
    index['bunkerVersion'] = PORTABLE['bunkerVersion']
    index['files'] = {name:digest(windows/name) for name in index['files']}
    manifest.write_text(json.dumps(index,sort_keys=True),encoding='utf-8')
