"""Fill missing English notice portraits from the same notice pack and original game dump."""
import csv
from contextlib import ExitStack
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import zipfile

from PIL import Image, ImageOps
import UnityPy


def render_sprite(bundle_file, row, unmask):
    raw = bundle_file.read_bytes()
    if hashlib.sha256(raw).hexdigest() != row['bundle_sha256']:
        raise ValueError(f"Unexpected portrait bundle: {row['bundle']}")
    env = UnityPy.load(unmask(raw, row['bundle']))
    obj, = [item for item in env.objects if item.path_id == int(row['object_id'])]
    sprite = obj.read()
    if sprite.m_Name != row['texture']:
        raise ValueError(f"Unexpected portrait sprite: {sprite.m_Name}")
    image = ImageOps.contain(sprite.image.convert('RGBA'), (460, 426), Image.Resampling.LANCZOS)
    canvas = Image.new('RGBA', (460, 426))
    canvas.paste(image, ((460 - image.width) // 2, (426 - image.height) // 2))
    output = io.BytesIO()
    canvas.save(output, 'WEBP', lossless=True, method=4)
    return output.getvalue()


def fill_portraits(pack, bundles, unmask, preserved):
    manifest = Path(__file__).with_name('notice-portraits.csv')
    prefix = '/images/en/operations/'
    additions = {}
    reused = 0
    missing = 0
    with ExitStack() as files:
        stream = files.enter_context(manifest.open(encoding='utf-8', newline=''))
        archive = files.enter_context(zipfile.ZipFile(pack))
        recovered = files.enter_context(zipfile.ZipFile(preserved)) if preserved.is_file() else None
        members = set(archive.namelist())
        catalog_time = archive.getinfo('notices.json').date_time
        bodies = '\n'.join(n.get('body', '') for n in json.loads(archive.read('notices.json'))['notices'])
        for row in csv.DictReader(stream):
            url = prefix + row['notice_path']
            member = 'web' + url
            if member in members or url not in bodies:
                continue
            category, key = row['notice_path'].split('/')
            sources = ['img/' + key + '.webp']
            if other := row['alternate_notice_id']:
                sources.extend(['web' + prefix + category + '/' + other, 'img/' + other + '.webp'])
            original = next((name for name in sources if name in members), None)
            if original:
                additions[member] = archive.read(original)
                reused += 1
            elif recovered and row['archived_webp_sha256'] and sources[0] in recovered.NameToInfo:
                data = recovered.read(sources[0])
                if hashlib.sha256(data).hexdigest() != row['archived_webp_sha256']:
                    raise ValueError(f'Unexpected archived portrait: {url}')
                additions[member] = data
                reused += 1
            elif (bundle := bundles / row['bundle']).is_file():
                additions[member] = render_sprite(bundle, row, unmask)
            else:
                missing += 1
    if additions:
        with tempfile.NamedTemporaryFile(dir=pack.parent, delete=False) as file:
            stage = Path(file.name)
        try:
            shutil.copyfile(pack, stage)
            with zipfile.ZipFile(stage, 'a') as archive:
                for name, data in additions.items():
                    archive.writestr(zipfile.ZipInfo(name, catalog_time), data)
            os.replace(stage, pack)
        finally:
            stage.unlink(missing_ok=True)
    print(f'Notices: reused {reused} packaged portraits, rendered {len(additions) - reused} game sprites, '
          f'{missing} source bundles absent.', flush=True)


if __name__ == '__main__':
    notice_pack, asset_bundles, bunker_scripts = map(Path, sys.argv[1:])
    sys.path.insert(0, str(bunker_scripts))
    from export_asset_images import unmask
    preserved = bunker_scripts.parent / 'inputs/bunker-notices-recovered.zip'
    fill_portraits(notice_pack, asset_bundles, unmask, preserved)
