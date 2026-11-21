"""Prepare the desktop player's assets from the supplied game APK."""
from pathlib import Path, PurePosixPath
import shutil
import sys
from zipfile import ZipFile
import hashlib
import json

sys.path.insert(0, str(Path(__file__).resolve().parent))
from desktop_bundles import UnityPy, convert_texture
from desktop_shader import convert as convert_shader


def stage(apk, folder, metadata):
    data = folder/'game/WindowsPlayer_Data'
    assets = folder/'data/apk-assets'
    native = folder/'data/native'
    for target in (data, assets, native):
        target.mkdir(parents=True, exist_ok=True)
    with ZipFile(apk) as z:
        for entry in z.infolist():
            if entry.is_dir():
                continue
            path = PurePosixPath(entry.filename)
            if path.parts[:1] == ('assets',):
                target = assets.joinpath(*path.parts[1:])
            elif entry.filename in ('lib/arm64-v8a/libil2cpp.so','lib/arm64-v8a/libunity.so'):
                target = native/path.name
            else:
                continue
            if not target.resolve().is_relative_to(folder.resolve()):
                raise ValueError(f'Invalid asset path {path}')
            target.parent.mkdir(parents=True, exist_ok=True)
            with z.open(entry) as source, target.open('wb') as out:
                shutil.copyfileobj(source, out)
    # Android's serialized data can be split into numbered pieces.
    android = assets/'bin/Data'
    for file in android.rglob('*'):
        if not file.is_file():
            continue
        dest = data/file.relative_to(android)
        dest.parent.mkdir(parents=True, exist_ok=True)
        if file.suffix.startswith('.split') and file.suffix[6:].isdigit():
            if file.suffix != '.split0':
                continue
            dest = dest.with_suffix('')
            base = file.with_suffix('')
            pieces = sorted(file.parent.glob(base.name+'.split*'), key=lambda p:int(p.suffix[6:]))
            if [p.suffix for p in pieces] != [f'.split{i}' for i in range(len(pieces))]:
                raise ValueError(f'Incomplete split file {base.name}')
            with dest.open('wb') as out:
                for part in pieces:
                    with part.open('rb') as stream:
                        shutil.copyfileobj(stream,out)
        else:
            shutil.copy2(file, dest)
        with dest.open('rb') as f:
            header = f.read(48)
        if header[8:12] != b'\0\0\0\x15' or header[20:31] != b'2019.4.29f1':
            continue
        asset = UnityPy.load(dest.read_bytes()).file
        changed = False
        for obj in asset.objects.values():
            if obj.type.name in ('Texture2D', 'Cubemap'):
                if convert_texture(obj.read()):
                    changed = True
            elif obj.type.name == 'Shader':
                shader = obj.read()
                if shader.platforms == [9] and shader.m_ParsedForm.m_Name != 'Hidden/VideoDecodeAndroid':
                    convert_shader(shader)
                    changed = True
            elif obj.type.name == 'BuildSettings':
                tree = obj.read_typetree()
                tree['m_GraphicsAPIs'] = [17]
                obj.save_typetree(tree)
                changed = True
            elif obj.type.name == 'PlayerSettings':
                tree = obj.read_typetree()
                tree.update(resizableWindow=True, allowFullscreenSwitch=True, productName='Nier Reincarnation',
                            defaultScreenWidth=1600, defaultScreenHeight=900, androidUseSwappy=False)
                obj.save_typetree(tree)
                changed = True
        if changed:
            asset._m_target_platform = 19
            dest.write_bytes(asset.save())
        elif header[31:36] == b'\0\x0d\0\0\0':
            # Unity rejects even platform-independent levels and prefabs when
            # their SerializedFile header still declares the Android target.
            with dest.open('r+b') as stream:
                stream.seek(32)
                stream.write((19).to_bytes(4, 'little'))
    (data/'Managed/Metadata/global-metadata.dat').write_bytes(metadata)
    # The Android IL2CPP backend opens these through its mounted private
    # storage. The Windows player does not perform Android's first-run copy.
    shutil.copytree(data/'Managed', folder/'data/storage/files/il2cpp', dirs_exist_ok=True)
    # Keep the original APK available to the Android asset manager.
    packaged = folder/'data/game-sound.apk'
    if apk.resolve() != packaged.resolve():
        shutil.copy2(apk, packaged)
    emblem = folder/'game/portable-emblem.json'
    if emblem.is_file():
        from header_emblem import image_bytes
        recipe = json.loads(emblem.read_text())
        png = image_bytes(data)
        if len(png) != recipe['size'] or hashlib.sha256(png).hexdigest() != recipe['sha256']:
            raise ValueError("The Bunker header sprite differs from the supported Global build")
        with (folder/'game/GameAssembly.dll').open('r+b') as dll:
            dll.seek(recipe['offset'])
            if dll.read(len(png)) != bytes(len(png)):
                raise ValueError("Invalid Bunker header placeholder")
            dll.seek(recipe['offset'])
            dll.write(png)


if __name__ == '__main__':
    stage(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]).read_bytes())
