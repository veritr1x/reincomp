"""Build the Windows header emblem against the supplied Global APK's Notices sprite."""
import io
import statistics
import sys
from pathlib import Path

import UnityPy
from PIL import Image


def image_bytes(player_data, bunker=None):
    # Unity GUID of header_notice in the supported Global client. UnityPy
    # follows its atlas reference into the adjacent, APK-derived player data.
    asset = UnityPy.load(str(player_data / '02255c4252c0a47a88e49b3d33d2de18'))
    sprite, = [o.read() for o in asset.objects if o.type.name == 'Sprite']
    if sprite.m_Name != 'header_notice':
        raise ValueError('Expected the original Global header_notice sprite')
    plate = sprite.image.convert('RGBA')
    sys.path.insert(0, str(bunker / 'android/tools' if bunker else Path(__file__).parent))
    from bunker_icon import draw

    # The atlas includes compression variation. Replace the old glyph with
    # nearby interior plate texels while preserving the translucent perimeter.
    pixels = plate.load()
    points = [(x, y) for y in range(plate.height) for x in range(plate.width)]
    ink = [pixels[p] for p in points if min(pixels[p][:3]) > 185 and pixels[p][3] > 240]
    cream = tuple(round(statistics.median(channel)) for channel in zip(*ink, strict=True))
    dark = [p for p in points if max(pixels[p][:3]) < 55 and 170 <= pixels[p][3] <= 185]
    for x, y in points:
        if max(pixels[x, y][:3]) >= 55 or pixels[x, y][3] > 185:
            nearest = min(dark, key=lambda p: (p[0]-x)**2 + (p[1]-y)**2)
            pixels[x, y] = pixels[nearest]
    glyph = draw(size=plate.width, plate=None, cream=(255, 255, 255, 255))
    plate.paste(Image.new('RGBA', plate.size, cream), mask=glyph.getchannel('A'))
    stream = io.BytesIO()
    plate.save(stream, format='PNG')
    return stream.getvalue()


def build(player_data, bunker, header):
    data = image_bytes(player_data, bunker)
    header.write_text(f'static const unsigned int kBunkerIconPngSize = {len(data)};\n'
                      'static const unsigned char kBunkerIconPng[] = {' +
                      ','.join(str(b) for b in data) + '};\n', encoding='ascii')


if __name__ == '__main__':
    build(*(Path(arg) for arg in sys.argv[1:]))
