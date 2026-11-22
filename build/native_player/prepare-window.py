"""Enable Unity's desktop resize border and built-in Alt+Enter handling."""
import os
from pathlib import Path
import sys
from tempfile import NamedTemporaryFile

if extra_packages := os.environ.get('REINCOMP_ASSET_PYTHON'):
    sys.path.append(extra_packages)
import UnityPy

path = Path(__file__).resolve().parent / 'runtime-player/WindowsPlayer_Data/globalgamemanagers'
asset = UnityPy.load(path.read_bytes()).file
settings, = (o for o in asset.objects.values() if o.type.name == 'PlayerSettings')
tree = settings.read_typetree()
settings_values = dict(resizableWindow=True, allowFullscreenSwitch=True, productName='Nier Reincarnation')
if any(tree[k] != v for k, v in settings_values.items()):
    tree.update(settings_values)
    settings.save_typetree(tree)
    data = asset.save()
    checked = UnityPy.load(data).file
    if checked.objects.keys() != asset.objects.keys() or checked.objects[settings.path_id].read_typetree() != tree:
        raise ValueError('Window configuration did not round trip')
    for key, obj in asset.objects.items():
        if key != settings.path_id and obj.get_raw_data() != checked.objects[key].get_raw_data():
            raise ValueError(f'Unexpected change to {obj.type.name} in globalgamemanagers')
    with NamedTemporaryFile(dir=path.parent, delete=False) as temp:
        temp.write(data)
    try:
        Path(temp.name).replace(path)
    finally:
        Path(temp.name).unlink(missing_ok=True)
print('Nier Reincarnation player settings verified with window resizing and fullscreen switching enabled.')
