"""Assemble the playable native Windows directory from the user's inputs."""
import ctypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT/'build/native_player'
MASTER = '20240404193219.bin.e'
MASTER_HASH = '60555788242de9a0580b0d86f6d109d67bd9cabd16d7bcda9b9a99745f270778'
UNITY_FILES = (
    ('WindowsPlayer.exe', 'WindowsPlayer.exe', '4d9db0c89373dea23d8c976ef7c302910979656aacd4df86e156e6d07ddaf48d'),
    ('UnityPlayer.dll', 'UnityPlayer.dll', '2c0409bc76f3c69f741a94e0bc8c167ecab231dfc1a62f7c20e5ca8160d781d5'),
    ('Data/Resources/unity default resources', 'WindowsPlayer_Data/Resources/unity default resources',
     '28f34dd870e9ef576828fca791d86a90f932a3d6814b947dc191fbed2354b888'),
)


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream,'sha256').hexdigest()


def run(*args, **kwargs):
    subprocess.run(list(map(str, args)), check=True, **kwargs)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    obj = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(obj)
    return obj


def archive_server_source(a, bunker, work, go):
    """Prepare the pinned Go module shared by Bunker's server and Archive."""
    server = bunker/'server'
    if (server/'gen/proto/gacha.pb.go').is_file() and (server/'mobile').is_dir():
        return server
    if a.no_download and not (server/'mobile').is_dir():
        raise ValueError('Supply a prepared Bunker server checkout with --bunker for an offline build')
    if a.bunker:
        # An explicitly supplied checkout belongs to the player. Generate in our cache.
        copy = work/('bunker-build-source-'+(bunker/'VERSION').read_text().strip())
        if not copy.exists():
            run('git', 'clone', '--config', 'core.autocrlf=false', '--no-hardlinks', bunker, copy)
        bunker = copy
        server = bunker/'server'
    if not (server/'mobile').is_dir():
        run('git','submodule','update','--init','--recursive', *(['--no-fetch'] if a.no_download else []), cwd=bunker)
        # Bunker runs git archive and git apply in its own subprocesses. Keep
        # their Go and embedded SQL output identical on every Windows checkout.
        git_env = os.environ.copy()
        index = int(git_env.get('GIT_CONFIG_COUNT', '0'))
        git_env.update(GIT_CONFIG_COUNT=str(index+1))
        git_env[f'GIT_CONFIG_KEY_{index}'] = 'core.autocrlf'
        git_env[f'GIT_CONFIG_VALUE_{index}'] = 'false'
        run(sys.executable, '-X', 'utf8', '-c',
            'import runpy,sys; from pathlib import Path; sys.path.insert(0,str(Path(sys.argv[1]).parent)); '
            'runpy.run_path(sys.argv[1],run_name="__main__")', bunker/'scripts/prepare.py', cwd=bunker, env=git_env)
    if not (server/'gen/proto/gacha.pb.go').is_file():
        protoc = shutil.which('protoc')
        if not protoc:
            raise ValueError('Install protoc on PATH, or supply a prepared Bunker server with --bunker')
        env = os.environ.copy()
        plugins = work/'proto-tools'
        plugins.mkdir(exist_ok=True)
        env['GOBIN'] = str(plugins)
        if a.no_download:
            env['GOPROXY'] = 'off'
            env['GOTOOLCHAIN'] = 'local'
        for name, pkg in [('protoc-gen-go','google.golang.org/protobuf/cmd/protoc-gen-go@v1.36.11'),
                          ('protoc-gen-go-grpc','google.golang.org/grpc/cmd/protoc-gen-go-grpc@v1.6.1')]:
            if not (plugins/(name+'.exe')).exists() and not shutil.which(name):
                run(go,'install',pkg,env=env)
        env['PATH'] = str(plugins)+os.pathsep+env['PATH']
        manifest = (server/'Makefile').read_text(encoding='utf-8').splitlines()
        protos, = [line.partition('=')[2].split() for line in manifest if line.startswith('PROTO_USED =')]
        run(protoc, '-I', '.', *protos, '--go_out=.', '--go_opt=module=lunar-tear/server',
            '--go-grpc_out=.', '--go-grpc_opt=module=lunar-tear/server', cwd=server, env=env)
    return server


def patch_runtime_library(image, bunker, work, cxx):
    """Apply Bunker's checked Global word patches to the input of the recompiler."""
    mobile = bunker/'android/app/src/main/cpp'
    helper = work/'bunker-library-patches.dll'
    cc = cxx.with_name(cxx.name.replace('g++','gcc'))
    run(cc, '-std=c11','-O2','-shared','-static','-I'+str(mobile),
        NATIVE/'bunker-library-patches.c', *(mobile/(name+'.c') for name in
        ('text_buffer','remain_days','gimmick_capacity','camera_edge')), '-o', helper)
    # In the SHA-verified Global ELF the executable segment is addressed at
    # its file offset. The upstream modules verify every original instruction.
    library = ctypes.CDLL(str(helper))
    try:
        apply = library.apply_bunker_library_patches
        apply.argtypes = [ctypes.POINTER(ctypes.c_ubyte)]
        apply.restype = ctypes.c_char_p
        patched = bytearray(image)
        failure = apply((ctypes.c_ubyte * len(patched)).from_buffer(patched))
        if failure:
            raise ValueError(f'Bunker code patch did not match: {failure.decode()}')
        return patched
    finally:
        # The caller can retry or remove its build directory in this process.
        from _ctypes import FreeLibrary
        FreeLibrary(library._handle)


def prepare_windows_pod_files(pods, bunker):
    # Keep the packaged Python runtime and wheels, and take the app itself
    # from the same prepared Bunker checkout as the Windows server.
    app = pods/'app'
    shutil.copytree(bunker/'android/app/src/main/python', app, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns('__pycache__', '*.py[co]'))
    version = pods/'VERSION'
    with version.open('rb') as stream:
        previous = stream.readline()
        packages = stream.read()
    if not previous.startswith(b'Bunker '):
        raise ValueError('Unexpected Pod Programs version file')
    version.write_bytes(b'Bunker '+(bunker/'VERSION').read_bytes().strip()+b'\n'+packages)
    # Windows _commit needs a writable descriptor, including when the bytes
    # were just written by copyfile. Keep Bunker's fsync before each replace.
    for name, files in [('android_patcher.py', ('backup', 'candidate')), ('android_runtime.py', ('stage',))]:
        path = app/name
        source = path.read_text(encoding='utf-8')
        updated = source
        for file in files:
            original = f'with {file}.open("rb") as handle:'
            writable = f'with {file}.open("r+b") as handle:'
            if updated.count(original) + updated.count(writable) != 1:
                raise ValueError(f'The pinned Bunker {name} file flush changed')
            updated = updated.replace(original, writable)
        if source != updated:
            path.write_text(updated, encoding='utf-8')

    templates = app/'web/templates'
    shutil.copy2(NATIVE/'desktop-display.html', templates)
    base = templates/'base.html'
    source = base.read_text(encoding='utf-8')
    anchor = '<a href="/users"'
    if anchor not in source:
        raise ValueError('The Bunker navigation template changed')
    if '<a href="/bunker/archive">' not in source:
        base.write_text(source.replace(anchor, '<a href="/bunker/archive">Archive</a>\n    '+anchor, 1),encoding='utf-8')
    patcher = templates/'patcher.html'
    patcher.write_text(patcher.read_text(encoding='utf-8').replace('"bunker-archive:" + i.ref','"/bunker/archive" + i.ref'), encoding='utf-8')


def build_windows_server(go, source, work, destination, env):
    # The pinned server keeps a file handle for lazy table reads. Windows cannot
    # replace that master while the calendar is changing, even after /pause.
    # Keep the encrypted snapshot Init already reads and decrypt the same slices.
    original = (source/'internal/masterdata/memorydb/memorydb.go').resolve()
    code = original.read_text(encoding='utf-8')
    replacements = (
        ('file *os.File', 'file *bytes.Reader'),
        ('defer func() {\n\t\tif err != nil {\n\t\t\tf.Close()\n\t\t}\n\t}()', 'defer f.Close()'),
        ('file: f, base:', 'file: bytes.NewReader(encrypted), base:'),
    )
    for before, after in replacements:
        if code.count(before) != 1:
            raise ValueError('The pinned Bunker master data reader changed')
        code = code.replace(before, after)
    patched = (work/'windows-memorydb.go').resolve()
    patched.write_text(code, encoding='utf-8')
    web = (source/'internal/service/bunker_panel_web.go').resolve()
    pages = web.read_text(encoding='utf-8')
    category = 'panel_mission|thumbnail|banner|character|weapon'
    if pages.count(category) != 1:
        raise ValueError('The pinned Bunker notice image routes changed')
    portraits = (work/'windows-notice-images.go').resolve()
    portraits.write_text(pages.replace(category, category+'|companion'), encoding='utf-8')
    overlay = windows_archive_overlay(source, work, {original: patched, web: portraits})
    release = json.loads((ROOT/'bunker.lock.json').read_text())['bunker']['release'].removeprefix('v')
    flags = '-s -w -X lunar-tear/server/internal/bunkerversion.Version='+release
    run(go, 'build', '-trimpath', '-buildvcs=false', '-overlay='+str(overlay), '-ldflags='+flags,
        '-o', destination.resolve(), './cmd/bunker-server', cwd=source, env={**env, 'CGO_ENABLED': '0'})


def windows_archive_overlay(source, work, extra=None):
    # Apply the same streaming verifier to the server's local imports and
    # the Archive helper's resumable HTTP imports.
    archive = (source/'mobile/archive.go').resolve()
    scratch = (work/'import-overlay').resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    checked = scratch/'archive.go'
    checked.write_text(archive.read_text(encoding='utf-8'), encoding='utf-8', newline='\n')
    run('git', '-c', 'core.autocrlf=false', 'apply', '--no-index', NATIVE/'windows-resource-import.patch', cwd=scratch,
        env={**os.environ, 'GIT_CEILING_DIRECTORIES': str(scratch.parent)})
    replacements = {archive: checked, archive.with_name('resource_workers_windows.go'): NATIVE/'resource_workers.go', **(extra or {})}
    overlay = (work/('windows-server-overlay.json' if extra else 'windows-archive-overlay.json')).resolve()
    overlay.write_text(json.dumps({'Replace': {str(k): str(v) for k,v in replacements.items()}}), encoding='utf-8')
    return overlay


def build_archive_helper(go, source, work, game, env):
    overlay = windows_archive_overlay(source, work)
    run(go, 'build', '-trimpath', '-overlay='+str(overlay), '-o', game/'archive-media.exe',
        NATIVE/'archive_media.go', NATIVE/'resource_archive.go', cwd=source, env=env)


def build_player_entry(cxx, original, destination, work):
    # Keep the verified Unity manifest, version information and icon groups.
    # The existing branding step still applies the APK's icons afterwards.
    resources = work/'player-resources.o'
    windres = cxx.with_name(cxx.name.replace('g++', 'windres'))
    run(windres, '-J', 'coff', '-O', 'coff', '-i', original, '-o', resources)
    # The linker embeds the output filename in the GPU-selection export table.
    entry = work/'player-entry.exe'
    run(cxx, '-std=c++17', '-O2', '-static', '-municode', '-mwindows', '-Wl,--no-insert-timestamp',
        NATIVE/'player-entry.cpp', resources, '-o', entry)
    if entry.resolve() != destination.resolve():
        shutil.copyfile(entry, destination)


def prepare(a, folder, work, server, deps, pods, bunker, cxx):
    if digest(a.master) != MASTER_HASH:
        raise ValueError(f'Expected the original Global {MASTER} ({MASTER_HASH})')
    if a.assets.suffix.lower() not in ('.7z','.zip') or not a.assets.is_file():
        raise ValueError('--assets requires a resource dump in .7z or .zip format')
    data = folder/'data'
    game = folder/'game'
    for path in (data, game, folder/'server'):
        path.mkdir(parents=True, exist_ok=True)
    for name in ('FirebaseCppApp-9_6_0.dll','grpc_csharp_ext.x64.dll'):
        shutil.copy2(deps/name, game/name)
    shutil.copy2(a.fsr_runtime, game/'amd_fidelityfx_vk.dll')
    shutil.copytree(deps/'angle', game/'angle', dirs_exist_ok=True)
    shutil.copytree(deps/'licenses', folder/'licenses', dirs_exist_ok=True)
    shutil.copy2(ROOT/'LICENSE', folder/'licenses/LICENSE.reincomp.txt')
    for name in ('fidelityfx-api', 'vulkan-headers'):
        shutil.copy2(ROOT/'third_party'/name/'LICENSE.txt', folder/'licenses'/('LICENSE.'+name+'.txt'))
    shutil.copy2(ROOT/'THIRD-PARTY.md', folder/'licenses/THIRD-PARTY.md')
    shutil.copytree(pods,folder/'pods',dirs_exist_ok=True)
    python = folder/'pods/python/python.exe'
    packages = folder/'pods/packages'
    if not (packages/'UnityPy').is_dir():
        if sys.version_info[:2] != (3,12) or sys.maxsize <= 2**32 or os.name != 'nt':
            raise ValueError('Use 64-bit Windows Python 3.12 to install the packaged converter')
        run(sys.executable, '-m', 'pip', 'install', *(['--no-index'] if a.no_download else []),
            '--no-compile', '--target', packages, 'UnityPy==1.25.4')
    run(python, '-I', '-c', 'import UnityPy; from UnityPy.export.Texture2DConverter import parse_image_data')
    for name in ('desktop_services.py','desktop_display.py','desktop_bundles.py','desktop_shader.py','octo_cache.py',
                 'resource_setup.py','resource-setup.html','resource-setup.css'):
        shutil.copy2(NATIVE/name,game/name)
    go = a.go or shutil.which('go')
    if not go:
        raise ValueError('Install Go or provide --go to compile the launcher, Bunker server and Archive helper')
    server_source = archive_server_source(a, bunker, work, go)
    prepare_windows_pod_files(folder/'pods',server_source.parent)
    shutil.copy2(a.master, data/'original-master.bin.e')
    patched = work/MASTER
    run(python,'-I','-X','utf8',folder/'pods/app/patcher/patch_masterdata.py','--input',a.master,'--output',patched)
    shutil.copy2(patched, data/'import-master.bin.e')
    imported = data/'assets'
    receipt = work/'imported-assets.json'
    source = {'master':digest(patched), 'archive':digest(a.assets), 'server':digest(server)}
    revision = imported/'assets/revisions/0'
    installed = imported/'assets/release'/MASTER
    if (not receipt.is_file() or json.loads(receipt.read_text())!=source or
            not any((revision/name).is_file() for name in ('list.bin','android/list.bin')) or
            not installed.is_file() or digest(installed)!=source['master']):
        run(server,'import','-assets',imported,'-dump',a.assets,'-master',patched)
        receipt.write_text(json.dumps(source))
    notices = imported/'notices/notices.zip'
    notices.parent.mkdir(parents=True,exist_ok=True)
    if a.notices:
        shutil.copy2(a.notices,notices)
    else:
        from portable_inputs import NOTICES, download
        pinned = work/NOTICES[0]
        if a.no_download and (not pinned.is_file() or digest(pinned)!=NOTICES[1]):
            raise ValueError('Offline builds require --notices or the pinned public pack in the build cache')
        shutil.copyfile(download(work,NOTICES),notices)
    with zipfile.ZipFile(notices) as z:
        if not json.loads(z.read('notices.json'))['notices']:
            raise ValueError('Empty notice pack')
    patch = module('bunker_game_patch',bunker/'android/tools/game_patch.py')
    with zipfile.ZipFile(a.apk) as z:
        metadata = z.read('assets/bin/Data/Managed/Metadata/global-metadata.dat')
        image = z.read('lib/arm64-v8a/libil2cpp.so')
    profile = patch.identify(image,metadata,'global')
    (work/'global-metadata.dat').write_bytes(patch.patch_metadata(metadata,profile,30000))
    patched_lib = work/'il2cpp-bunker.so'
    patched_lib.write_bytes(patch_runtime_library(patch.patch_library(image,profile,30000),bunker,work,cxx))
    del image
    # Embed the same original data and the same Bunker endpoint literals as the APK build.
    run(python,'-I',NATIVE/'package_assets.py',a.apk,folder,work/'global-metadata.dat')
    player = a.unity_player
    if not player:
        player = Path(os.environ.get('ProgramFiles','C:/Program Files'))/'Unity/Hub/Editor/2019.4.29f1/Editor/Data/PlaybackEngines/WindowsStandaloneSupport/Variations/win64_nondevelopment_il2cpp'
    for name, destination, sha in UNITY_FILES:
        if not (player/name).is_file() or digest(player/name) != sha:
            raise ValueError(f'Need Unity 2019.4.29f1 Windows IL2CPP release {name}. Set --unity-player to its directory.')
        (game/destination).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(player/name, game/destination)
    build_player_entry(cxx, player/'WindowsPlayer.exe', game/'WindowsPlayer.exe', work)
    env = os.environ.copy()
    if a.no_download:
        env.update(GOPROXY='off', GOTOOLCHAIN='local')
    packaged_server = folder/'server/bunker-server.exe'
    if a.unpinned_server:
        shutil.copy2(server, packaged_server)
    else:
        build_windows_server(go, server_source, work, packaged_server, env)
    build_archive_helper(go, server_source, work, game, env)
    run(cxx, '-std=c++17', '-O2', '-static', '-municode', '-mwindows', NATIVE/'resource-window.cpp',
        ROOT/'host/bunker_view.cpp', '-lole32', '-luuid', '-lcomdlg32', '-lshell32', '-o', game/'resource-window.exe')
    if a.launcher:
        shutil.copy2(a.launcher,folder/'reincomp.exe')
    else:
        run(go,'build','-trimpath','-o',folder/'reincomp.exe','.',cwd=ROOT/'launcher')
    run(python,'-I',NATIVE/'brand-player.py',a.apk,game/'WindowsPlayer.exe',folder/'reincomp.exe')
    return patched_lib
