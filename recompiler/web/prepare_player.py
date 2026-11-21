"""Assemble the reusable Windows player files and empty APK icon slots for the web builder."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import struct
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

HERE = Path(__file__).resolve().parent
NATIVE = HERE.parents[1]/'build/native_player'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def prepare(args):
    release = load('windows_release', NATIVE.parent/'native_release.py')
    brand = load('windows_icon', NATIVE/'brand-player.py')
    if release.digest(args.apk) != '9ded0797ee607b6892b531ba3a02f4c4510774f5fe098ccada78ccff881c9209':
        raise ValueError('Expected the original Global APK')
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.out) as tmp:
        work = Path(tmp)
        icons, placeholders = {}, {}
        with ZipFile(args.apk) as apk:
            for density in ('ldpi','mdpi','hdpi','xhdpi','xxhdpi','xxxhdpi'):
                path = f'res/mipmap-{density}-v4/app_icon.png'
                png = apk.read(path)
                width,height = struct.unpack_from('>II',png,16)
                if png[:8] != b'\x89PNG\r\n\x1a\n' or width!=height or width>256:
                    raise ValueError('Unexpected Android icon')
                icons[width] = path
                placeholders[width] = hashlib.shake_256(path.encode()).digest(len(png))
        members = {}
        for name in ('EngineCalls.dll','GameAssembly.dll','Fsr31.dll','amd_fidelityfx_vk.dll',
                     'grpc_csharp_ext.x64.dll','FirebaseCppApp-9_6_0.dll'):
            members['game/'+name] = args.release/'game'/name
        if args.adapter:
            members['game/GameAssembly.dll'] = args.adapter
        for directory in ('licenses','pods','game/angle'):
            for path in (args.release/directory).rglob('*'):
                if not path.is_file() or '__pycache__' in path.parts or path.suffix.lower() in ('.pyc','.pyo'):
                    continue
                if path.suffix.lower() in ('.apk','.ipa','.bin.e','.so') or path.name.endswith('.bin.e') or path.name in ('game.db','auth.db','auth.key'):
                    raise ValueError(f'Unexpected game data in public dependency bundle: {path.name}')
                members[path.relative_to(args.release).as_posix()] = path
        for name, dest, digest in release.UNITY_FILES:
            path = args.unity/name
            if release.digest(path)!=digest: raise ValueError(f'Unexpected stock Unity {name}')
            members['game/'+dest] = path
        for name in ('resource-setup.html','resource-setup.css','resource_setup.py','package_assets.py','desktop_shader.py',
                     'desktop_bundles.py','desktop_display.py','desktop_services.py','header_emblem.py'):
            members['game/'+name] = NATIVE/name
        members['game/bunker_icon.py'] = args.bunker/'android/tools/bunker_icon.py'
        members['game/resource-window.exe'] = args.window
        members['game/archive-media.exe'] = args.archive
        members['server/bunker-server.exe'] = args.server
        # The embedded emblem's plate comes from the supplied game. Keep only
        # its position and hash in the kit. First run reconstructs it locally.
        text = args.emblem.read_text()
        png = bytes(map(int,re.search(r'kBunkerIconPng\[\]\s*=\s*\{([\d,\s]+)\}',text)[1].split(',')))
        adapter = members['game/GameAssembly.dll'].read_bytes()
        if adapter.count(png)!=1: raise ValueError('Unexpected Bunker emblem in GameAssembly.dll')
        desc = {'offset':adapter.index(png),'size':len(png),'sha256':hashlib.sha256(png).hexdigest()}
        (work/'portable-emblem.json').write_text(json.dumps(desc))
        (work/'GameAssembly.dll').write_bytes(adapter.replace(png,bytes(len(png))))
        members.update({'game/GameAssembly.dll':work/'GameAssembly.dll','game/portable-emblem.json':work/'portable-emblem.json'})
        index = {'members':sorted(members),'icons':{},'parts':[]}
        members['launcher.exe'] = args.launcher
        for key,path in [('launcher','launcher.exe'),('player','game/WindowsPlayer.exe')]:
            copy = work/Path(path).name
            shutil.copyfile(members[path],copy)
            brand.embed(copy,placeholders)
            image = copy.read_bytes()
            slots = []
            for width,marker in placeholders.items():
                offsets=[m.start() for m in re.finditer(re.escape(marker),image)]
                if not offsets: raise ValueError('Missing Windows icon slot')
                slots.append({'path':icons[width],'size':len(marker),'offsets':offsets})
            index['icons'][key]=slots
            members[path]=copy
        packed = work/'player.zip'
        with ZipFile(packed,'w',compression=ZIP_DEFLATED,compresslevel=5) as z:
            for name,path in sorted(members.items()):
                z.writestr(ZipInfo(name),path.read_bytes(),ZIP_DEFLATED,compresslevel=5)
        with packed.open('rb') as src:
            while block:=src.read(48*1024*1024):
                name=f'player.part{len(index["parts"])+1:02d}'
                (args.out/name).write_bytes(block)
                index['parts'].append((name,hashlib.sha256(block).hexdigest()))
        (args.out/'player.json').write_text(json.dumps(index,separators=(',',':')))


if __name__=='__main__':
    cli=argparse.ArgumentParser(description=__doc__)
    for arg in ('apk','bunker','release','unity','launcher','window','archive','server','emblem','out'):
        cli.add_argument('--'+arg,type=Path,required=True)
    cli.add_argument('--adapter',type=Path,help='Rebuilt Windows GameAssembly adapter for this kit')
    prepare(cli.parse_args())
