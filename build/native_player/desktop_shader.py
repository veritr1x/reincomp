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

stats=Counter()
gpu_types=Counter()
def convert(shader):
    if shader.platforms!=[9]: raise ValueError((shader.m_ParsedForm.m_Name,shader.platforms))
    chunks=[]
    for pos,size,raw_size in zip(shader.offsets[0],shader.compressedLengths[0],shader.decompressedLengths[0],strict=True):
        compressed=bytes(shader.compressedBlob[pos:pos+size])
        chunks.append(bytearray(lz4.block.decompress(compressed,raw_size)))
    directory=chunks[0]
    entries=struct.unpack_from('<I',directory)[0]
    for j in range(entries):
        offset,n,segment=struct.unpack_from('<III',directory,4+12*j)
        chunk=chunks[segment]
        if not n or offset+n>len(chunk):raise ValueError((j,offset,n,segment))
        version,kind=struct.unpack_from('<II',chunk,offset)
        if version!=201806140 or kind not in (2,3,4,5):continue
        block=chunk[offset:offset+n]
        if b'#ifdef VERTEX' not in block or b'#ifdef FRAGMENT' not in block:
            sub=ShaderSubProgram(EndianBinaryReader(block,endian='<'))
            if sub.m_ProgramCode:raise ValueError((j,version,kind,bytes(sub.m_ProgramCode[:70])))
            struct.pack_into('<I',chunk,offset+4,8)
            stats['empty_stage_entries']+=1
            continue
        def version_line(match):
            replacement=b'#version 450'
            return replacement.ljust(len(match[0]))
        fixed,count=re.subn(rb'(?m)^#version (?:300|310|320) es',version_line,block)
        stages=re.findall(rb'#ifdef (\w+)\s*#version',block)
        if stages not in ([b'VERTEX',b'FRAGMENT'],[b'VERTEX',b'FRAGMENT',b'GEOMETRY']) or count!=len(stages) or len(block)!=len(fixed):
            raise ValueError(('version lines',shader.m_ParsedForm.m_Name,stages,count,j,block[block.index(b'#ifdef VERTEX'):][:140]))
        struct.pack_into('<I',fixed:=bytearray(fixed),4,8)
        chunk[offset:offset+n]=fixed
        gpu_types[kind]+=1
        stats['stage_pairs']+=1
    packed=bytearray()
    for i,chunk in enumerate(chunks):
        compressed=lz4.block.compress(bytes(chunk),store_size=False)
        shader.offsets[0][i]=len(packed)
        shader.compressedLengths[0][i]=len(compressed)
        packed+=compressed
    shader.compressedBlob=bytes(packed)
    shader.platforms=[15]
    for sub in shader.m_ParsedForm.m_SubShaders:
        for p in sub.m_Passes:
            if p.m_Platforms:raise ValueError(('pass platforms',p.m_Platforms))
            for key in ('progVertex','progFragment','progDomain','progHull','progGeometry','progRayTracing'):
                prog=getattr(p,key,None)
                if not prog:continue
                for subprog in prog.m_SubPrograms:
                    if subprog.m_GpuProgramType not in (2,3,4,5):raise ValueError(subprog.m_GpuProgramType)
                    subprog.m_GpuProgramType=8
    shader.save()
    stats['shaders']+=1

