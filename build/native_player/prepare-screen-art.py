"""Prepare the installed Windows player's UI textures from the game's own pixels."""
import argparse
import hashlib
import json
import os
import struct
import sys
from io import BytesIO
from pathlib import Path
from tempfile import NamedTemporaryFile

from desktop_bundles import CACHE_DIRECTORY, UnityPy, convert_bundle, convert_texture
from PIL import Image


def replace(path, data):
    with NamedTemporaryFile(dir=path.parent, delete=False) as temp:
        name = Path(temp.name)
        temp.write(data)
    try:
        name.replace(path)
    finally:
        name.unlink(missing_ok=True)


def serialized_files(data):
    for file in sorted(data.rglob('*')):
        if not file.is_file():
            continue
        with file.open('rb') as stream:
            header = stream.read(48)
        if header[8:12] != b'\0\0\0\x15' or header[20:31] != b'2019.4.29f1':
            continue
        name = file
        if file.suffix == '.split0':
            name = file.with_suffix('')
            if name.exists():
                continue
            pieces = sorted(file.parent.glob(name.name + '.split*'), key=lambda p: int(p.suffix[6:]))
            raw = b''.join(p.read_bytes() for p in pieces)
        else:
            raw = file.read_bytes()
        if len(raw) == struct.unpack_from('>I', header, 4)[0]:
            yield name.relative_to(data), raw


def prepare_packaged(root, verify_only):
    source = root / 'data/apk-assets/bin/Data'
    target = root / 'game/WindowsPlayer_Data'
    if not source.is_dir():
        source = root / 'host-probe/WindowsPlayer_Data'
        target = root / 'runtime-player/WindowsPlayer_Data'
    if not source.is_dir() or not target.is_dir():
        raise FileNotFoundError(f'Expected source and Windows player data at {source} and {target}')
    verified = updated = 0
    for name, raw in serialized_files(source):
        originals = UnityPy.load(raw).file
        images = [o for o in originals.objects.values() if o.type.name in ('Texture2D', 'Cubemap')]
        if not images:
            continue
        dest = target / name
        desktop = UnityPy.load(dest.read_bytes()).file
        expected = {}
        changed = False
        for obj in images:
            texture = obj.read()
            if texture.m_TextureFormat not in {34, 45, 46, 47, *range(48, 60)}:
                continue
            pixels = texture.image.convert('RGBA').tobytes()
            convert_texture(texture)
            expected[obj.path_id] = texture.get_image_data(), texture.m_CompleteImageSize, hashlib.sha256(pixels).digest()
            current = desktop.objects[obj.path_id].read()
            if (current.m_TextureFormat == 4 and current.m_CompleteImageSize == texture.m_CompleteImageSize
                    and current.get_image_data() == expected[obj.path_id][0]):
                continue
            if verify_only:
                raise ValueError(f'Desktop texture differs from source in {name}, object {obj.path_id}')
            current.m_TextureFormat = texture.m_TextureFormat
            current.m_CompleteImageSize = texture.m_CompleteImageSize
            current.image_data = texture.get_image_data()
            if current.m_StreamData:
                current.m_StreamData.path = ''
                current.m_StreamData.offset = current.m_StreamData.size = 0
            current.save()
            changed = True
        encoded = desktop.save() if changed else None
        check = UnityPy.load(encoded).file if changed else desktop
        for path_id, (mips, face_size, digest) in expected.items():
            saved = check.objects[path_id].read()
            if (saved.m_TextureFormat != 4 or saved.m_CompleteImageSize != face_size or saved.get_image_data() != mips or
                    hashlib.sha256(saved.image.convert('RGBA').tobytes()).digest() != digest):
                raise ValueError(f'Desktop texture differs from source in {name}, object {path_id}')
        if changed:
            replace(dest, encoded)
            updated += 1
        verified += len(expected)
    if not verified:
        raise ValueError(f'No packaged mobile textures found in {source}')
    print(f'Verified {verified} packaged mobile textures including all mip levels, updated {updated} files.', flush=True)


def prepare_sprites(root, manifest, verify_only):
    bundles = root / 'data/assets/assets/revisions/0/assetbundle'
    if bundles.is_dir():
        cache = root / 'data' / CACHE_DIRECTORY
        bunker = Path(__file__).resolve().parents[3] / 'bunker'
    else:
        bundles = root.parent / 'emu-client/data/assets/assets/revisions/0/assetbundle'
        cache = root / 'checkpoint-client' / CACHE_DIRECTORY
        bunker = root.parents[2] / 'bunker'
    scripts = Path(os.environ.get('BUNKER_SOURCE_ROOT', bunker)) / 'scripts'
    sys.path.insert(0, str(scripts))
    from export_asset_images import unmask

    if not verify_only:
        cache.mkdir(parents=True, exist_ok=True)
    entries = json.loads(manifest.read_text(encoding='utf-8'))['images']
    if not entries:
        raise ValueError(f'No sprite entries in {manifest}')
    for item in entries:
        name = item['bundle']
        source = (bundles / name).read_bytes()
        if hashlib.sha256(source).hexdigest() != item['source_sha256']:
            raise ValueError(f'Artwork source revision differs for {name}')
        raw = unmask(source, name)
        dest = cache / (hashlib.sha256(raw).hexdigest() + '-00000000.winbundle')
        existing = dest.exists()
        result = dest.read_bytes() if existing else convert_bundle(raw)
        env = UnityPy.load(result)
        obj, = (o for o in env.objects if o.path_id == item['object_id'] and o.type.name == item['object_type'])
        image = obj.read().image.convert('RGBA')
        expected = item.get('rgba_sha256')
        if expected is None:
            # Earlier character and inventory catalogs pin their original PNG.
            png = (manifest.parent / item['file']).read_bytes()
            if hashlib.sha256(png).hexdigest() != item['sha256']:
                raise ValueError(f'Reference PNG differs for {name}')
            with Image.open(BytesIO(png)) as reference:
                if reference.size != (item['width'], item['height']):
                    raise ValueError(f'Reference PNG dimensions differ for {name}')
                expected = hashlib.sha256(reference.convert('RGBA').tobytes()).hexdigest()
        if image.size != (item['width'], item['height']) or hashlib.sha256(image.tobytes()).hexdigest() != expected:
            raise ValueError(f'Desktop sprite differs from preserved RGBA artwork for {name}')
        if not verify_only and not existing:
            replace(dest, result)
    print(f'Verified {len(entries)} desktop sprites against {manifest}.', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--art-manifest', '--summon-manifest', action='append', type=Path, default=[])
    parser.add_argument('--verify-only', action='store_true', help='Compare artwork without writing game files.')
    parser.add_argument('--packaged', action='store_true')
    args = parser.parse_args()
    for manifest in args.art_manifest:
        prepare_sprites(args.root.resolve(), manifest, args.verify_only)
    if args.packaged:
        prepare_packaged(args.root.resolve(), args.verify_only)
