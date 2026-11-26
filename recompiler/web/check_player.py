"""Check that a browser kit's player package can install: the header emblem
recipe must point at a blank placeholder in GameAssembly.dll.

The first launch writes the emblem into that placeholder. A recipe left over
from another build of the library, or a library that still holds the image,
makes every installation fail."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
from zipfile import ZipFile


def check_emblem(player_zip):
    with ZipFile(io.BytesIO(player_zip)) as z:
        recipe = json.loads(z.read('game/portable-emblem.json'))
        dll = z.read('game/GameAssembly.dll')
    offset, size, digest = recipe['offset'], recipe['size'], recipe['sha256']
    if not (isinstance(offset, int) and isinstance(size, int) and 0 < size and 0 <= offset <= len(dll) - size
            and dll[offset:offset + size] == bytes(size)):
        raise ValueError(f'The emblem recipe does not point at a blank placeholder in GameAssembly.dll (offset {offset})')
    for found in re.finditer(rb'\x89PNG\r\n\x1a\n', dll):
        if hashlib.sha256(dll[found.start():found.start() + size]).hexdigest() == digest:
            raise ValueError(f'GameAssembly.dll still contains the emblem image at offset {found.start()}')


def check_kit(kit):
    """kit: a kit directory, or the public compiler ZIP made from one."""
    if kit.is_dir():
        read = lambda name: (kit / name).read_bytes()
        check_emblem(b''.join(read(name) for name, _ in json.loads(read('player.json'))['parts']))
    else:
        with ZipFile(kit) as z:
            check_emblem(b''.join(z.read(name) for name, _ in json.loads(z.read('player.json'))['parts']))


if __name__ == '__main__':
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('kit', type=Path, help='kit directory or bunker-windows-browser-<version>.zip')
    check_kit(cli.parse_args().kit)
    print('The player package can install its header emblem.')
