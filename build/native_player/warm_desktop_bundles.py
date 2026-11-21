"""Convert the private save's installed Octo bundles using the matching asset catalog."""
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
import hashlib
import os
from pathlib import Path
import sys
from tempfile import NamedTemporaryFile

sys.dont_write_bytecode = True
bunker_root = Path(os.environ.get('BUNKER_SOURCE_ROOT', Path(__file__).resolve().parents[3] / 'bunker'))
sys.path.insert(0, str(bunker_root / 'scripts'))
from export_asset_images import unmask
from desktop_bundles import CACHE_DIRECTORY, convert_bundle

root = Path(__file__).resolve().parent
client = root / 'checkpoint-client'
cache = client / CACHE_DIRECTORY


def varint(data, index):
    value = shift = 0
    while index < len(data) and shift < 70:
        b = data[index]
        index += 1
        value |= (b & 127) << shift
        if b < 128:
            return value, index
        shift += 7
    raise ValueError('Invalid catalog varint')


def fields(data):
    i = 0
    while i < len(data):
        tag, i = varint(data, i)
        if tag & 7 == 0:
            val, i = varint(data, i)
        elif tag & 7 == 2:
            size, i = varint(data, i)
            val = data[i:i+size]
            i += size
        elif tag & 7 in (1, 5):
            i += 8 if tag & 7 == 1 else 4
            continue
        else:
            raise ValueError(f'Invalid catalog wire type {tag & 7}')
        yield tag >> 3, val


def bake(item):
    file, name = item
    try:
        encrypted = file.read_bytes()
        if hashlib.md5(encrypted).hexdigest() != file.name:
            raise ValueError('Cached file differs from the catalog MD5')
        raw = unmask(encrypted, name.replace(')', '/') + '.assetbundle')
        key = hashlib.sha256(raw).hexdigest() + '-00000000'
        dest = cache / (key + '.winbundle')
        status = 'ready'
        if not dest.exists():
            encoded = convert_bundle(raw)
            temp = cache / (key + '.warm-' + file.name)
            temp.write_bytes(encoded)
            temp.replace(dest)
            status = 'converted'
        return status, file.name, key, name
    except Exception as exc:
        return 'failed', file.name, repr(exc), name


def index_verified_files(installed):
    # Windows file identity and last-write time allow the player to reuse a
    # verified, immutable Octo file without reading its entire managed stream.
    with (cache/'warm.tsv').open(encoding='utf-8') as records, NamedTemporaryFile(
            mode='w', dir=cache, delete=False, encoding='utf-8') as output:
        staging = Path(output.name)
        indexed = 0
        for record in records:
            status, digest, key, mask = record.rstrip('\n').split('\t', 3)
            source = installed.get(digest)
            if status not in ('ready', 'converted') or not source or not (cache/(key+'.winbundle')).is_file():
                continue
            with source.open('rb') as data:
                before = os.fstat(data.fileno())
                version = data.read(1)
                if version not in (b'1', b'2'):
                    continue
                data.seek(0)
                original = hashlib.file_digest(data, 'md5').hexdigest()
                after = os.fstat(data.fileno())
            if original != digest or (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                continue
            filetime = after.st_mtime_ns // 100 + 116444736000000000
            header = min(after.st_size, 256) if version == b'1' else after.st_size
            print(digest, key, after.st_ino, after.st_size, filetime, header, mask, sep='\t', file=output)
            indexed += 1
    staging.replace(cache/'verified-files.tsv')
    print(f'Indexed {indexed} verified Octo files', flush=True)


if __name__ == '__main__':
    cache.mkdir(exist_ok=True)
    installed = {p.name: p for p in (client/'storage/files/octo').rglob('*') if len(p.name)==32 and p.is_file()}
    if '--index-only' in sys.argv:
        index_verified_files(installed)
        sys.exit(0)
    catalog = root.parent / 'emu-client/data/assets/assets/revisions/0/list.bin'
    matches = {}
    for tag, msg in fields(catalog.read_bytes()):
        if tag == 2:
            entry = dict(fields(msg))
            key = entry[10].decode()
            if key in installed:
                matches[key] = entry[3].decode()
    print(f'{len(matches)} catalogued bundles in private save', flush=True)
    counts = Counter()
    with ProcessPoolExecutor(max_workers=3) as pool, (cache/'warm.tsv').open('w',encoding='utf-8') as log:
        for result in pool.map(bake, [(installed[k], name) for k,name in matches.items()], chunksize=4):
            counts[result[0]] += 1
            print(*result,sep='\t',file=log,flush=True)
            if sum(counts.values()) % 500 == 0:
                print(dict(counts),flush=True)
    print(dict(counts),flush=True)
    index_verified_files(installed)
