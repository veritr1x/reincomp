"""Convert the staging copy of Unity 2019.4 Android OpenGL ES shader programs."""
from collections import Counter
from pathlib import Path
import os
import re
import struct
import sys
sys.dont_write_bytecode=True
if extra_packages := os.environ.get('REINCOMP_ASSET_PYTHON'):
    sys.path.append(extra_packages)
import UnityPy
import lz4.block
from UnityPy.export.ShaderConverter import ShaderSubProgram
from UnityPy.streams import EndianBinaryReader

root=Path(__file__).resolve().parent
original=root/'host-probe/WindowsPlayer_Data'
target=root/'runtime-player/WindowsPlayer_Data'
sources=sorted({line.split(' ',1)[0] for line in (root/'native-asset-inventory.txt').read_text().splitlines()
                if "('kShader" in line and not line.startswith('Shader')})
if '--builtin-only' in sys.argv:sources=['Resources/unity_builtin_extra']
from desktop_shader import convert, gpu_types, stats

for name in sources:
    source=original/name
    if source.exists():
        raw=source.read_bytes()
    else:
        parts=sorted(original.glob(name+'.split*'),key=lambda p:int(p.suffix[6:]))
        assert parts
        raw=b''.join(p.read_bytes() for p in parts)
    env=UnityPy.load(raw)
    asset=env.file
    for obj in asset.objects.values():
        if obj.type.name!='Shader':continue
        shader=obj.read()
        if shader.m_ParsedForm.m_Name=='Hidden/VideoDecodeAndroid':
            stats['android_video_decoders_preserved']+=1
            continue
        convert(shader)
    asset._m_target_platform=19
    encoded=asset.save()
    trial=UnityPy.load(encoded).file
    assert set(asset.objects)==set(trial.objects)
    for key,obj in trial.objects.items():
        if obj.type.name=='Shader':
            shader=obj.read()
            if shader.m_ParsedForm.m_Name=='Hidden/VideoDecodeAndroid':
                assert obj.get_raw_data()==asset.objects[key].get_raw_data()
            else:assert shader.platforms==[15]
        else:
            assert obj.get_raw_data()==asset.objects[key].get_raw_data()
    destination=target/name
    pending=destination.with_name(destination.name+'.converted')
    pending.write_bytes(encoded)
    pending.replace(destination)
    stats['files']+=1
print(dict(stats),dict(gpu_types),flush=True)
(root/('converted-builtins.txt' if '--builtin-only' in sys.argv else 'converted-shaders.txt')).write_text(str(dict(stats))+'\n'+str(dict(gpu_types))+'\n')
