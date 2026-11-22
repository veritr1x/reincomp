"""Convert the original splash, title and referenced sprite atlas pixels for the Windows test player."""
from collections import defaultdict
from pathlib import Path
import os
import re
import sys

sys.dont_write_bytecode = True
if extra_packages := os.environ.get('REINCOMP_ASSET_PYTHON'):
    sys.path.append(extra_packages)
import UnityPy
from UnityPy.enums import TextureFormat

root = Path(__file__).resolve().parent
original = root / 'host-probe/WindowsPlayer_Data'
target = root / 'runtime-player/WindowsPlayer_Data'
pending = ['76931d78223ec4879b00d78af9e1f507', 'b0f57398ad10240b3b7255bd943082ab',
           '4784b4366ff6f412098650d8b7e35bbd']
walked = set()
textures = defaultdict(set)

while pending:
    name = pending.pop()
    if name in walked:
        continue
    walked.add(name)
    asset = UnityPy.load((original / name).read_bytes()).file
    for obj in asset.objects.values():
        if obj.type.name == 'Texture2D':
            textures[name].add(obj.path_id)
        elif obj.type.name == 'SpriteAtlas':
            for _, entry in obj.read().m_RenderDataMap:
                for ref in (entry.texture, entry.alphaTexture):
                    if ref.m_PathID:
                        other = asset.externals[ref.m_FileID - 1].path if ref.m_FileID else name
                        textures[other].add(ref.m_PathID)
    for ext in asset.externals:
        if re.fullmatch('[0-9a-f]{32}', ext.path) and not ext.path.startswith('0000000000000000'):
            pending.append(ext.path)

count = 0
for name, ids in textures.items():
    src = UnityPy.load((original / name).read_bytes()).file
    dst = UnityPy.load((target / name).read_bytes()).file
    expected = {}
    for path_id in sorted(ids):
        texture = src.objects[path_id].read()
        if texture.m_TextureFormat not in (34, 47, 48, 49, 50, 51, 52, 53):
            continue
        if texture.m_MipCount != 1:
            raise ValueError(f'Expected a single UI mip level for {texture.m_Name}')
        pixels = texture.image.convert('RGBA')
        expected[path_id] = pixels.tobytes()
        converted = dst.objects[path_id].read()
        converted.set_image(pixels, target_format=TextureFormat.RGBA32, mipmap_count=1)
        converted.save()
    if not expected:
        continue
    encoded = dst.save()
    checked = UnityPy.load(encoded).file
    for path_id, pixels in expected.items():
        texture = checked.objects[path_id].read()
        if texture.image.convert('RGBA').tobytes() != pixels:
            raise ValueError(f'Desktop pixels differ from {texture.m_Name}')
    temp = target / (name + '.converted')
    temp.write_bytes(encoded)
    temp.replace(target / name)
    count += len(expected)
print(f'Verified {count} desktop textures against original splash, title and atlas pixels.')
