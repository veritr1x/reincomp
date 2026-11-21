"""Select the managed spring-animation call group from the supplied IL2CPP image."""
from bisect import bisect_right
import hashlib
import re
import struct


def spring_entries(image, metadata, heads_path):
    header = struct.unpack_from('<66I',metadata)
    def string(index):
        start=header[6]+index
        return metadata[start:metadata.index(0,start)].decode()
    types=[(string(namespace),string(name)) for name,namespace in
           (struct.unpack_from('<II',metadata,p) for p in range(header[40],header[40]+header[41],92))]
    assemblies=[None]*len(types)
    for p in range(header[42],header[42]+header[43],40):
        name,_,start,count=struct.unpack_from('<4I',metadata,p)
        assemblies[start:start+count]=[string(name)]*count
    names=set(assemblies)
    modules={}
    strings={}
    for match in re.finditer(rb'[A-Za-z0-9_.-]+\.dll\x00',image.data):
        name=match[0][:-1].decode()
        if name in names:
            for lo,hi,offset in image.loads:
                if offset<=match.start()<offset+hi-lo:
                    strings[lo+match.start()-offset]=name
    relocs={}
    for sec in image.sections:
        if sec[1]==4:  # SHT_RELA, R_AARCH64_RELATIVE
            for pos,kind,addend in struct.iter_unpack('<QQq',memoryview(image.data)[sec[4]:sec[4]+sec[5]]):
                if kind&0xffffffff==1027:relocs[pos]=addend
    def pointer(address):
        if address in relocs:return relocs[address]
        return struct.unpack_from('<Q',image.data,image.offset(address))[0]
    for pos,target in relocs.items():
        if target in strings:
            count=image.word(pos+8)
            if 0<count<300000:
                table=pointer(pos+16)
                try:image.offset(table+count*8-1)
                except ValueError:continue
                modules[strings[target]]=(count,table)
    methods={}
    bias=0x80000000
    for p in range(header[12],header[12]+header[13],32):
        name,owner,_,_,_,token=struct.unpack_from('<6I',metadata,p)
        module=modules.get(assemblies[owner])
        if not module:continue
        idx=(token&0xffffff)-1
        if not 0<=idx<module[0]:raise ValueError('Invalid managed code token')
        address=pointer(module[1]+8*idx)
        if address:methods[bias+address]=(*types[owner],name)
    heads=sorted(set(int(line,16) for line in heads_path.read_text().split()))
    known=set(heads)
    animation={'UpdateSpring','ComputeRotation','CheckForCollision','CheckForCollisionAndReact','ApplyAngleLimits','ConstrainVector'}
    math_types={'Vector3','Mathf','Quaternion','Transform','Vector2','Matrix4x4'}
    eligible=set()
    pending=[]
    for pc,(namespace,owner,name) in methods.items():
        if pc not in known:continue
        if owner=='ActorAnimationComponent' and string(name)=='LateUpdateSpring':
            pending.append(pc)
        elif namespace=='UTJ' and ('Spring' in owner or owner=='AngleLimits'):
            eligible.add(pc)
            if string(name) in animation:pending.append(pc)
        elif namespace=='UnityEngine' and owner in math_types:
            eligible.add(pc)
    selected=set()
    while pending:
        pc=pending.pop()
        if pc in selected:continue
        selected.add(pc)
        end=heads[bisect_right(heads,pc)]
        for pos in range(pc,end,4):
            word=image.word(pos-bias)
            if word>>26 in (5,37):  # B or BL, signed imm26
                imm=word&0x3ffffff
                target=pos+4*(imm-(1<<26) if imm&(1<<25) else imm)
                if target in eligible:pending.append(target)
    data=''.join(f'{pc:#x}\r\n' for pc in sorted(selected)).encode()
    if hashlib.sha256(data).hexdigest()!='7b4cb96adc5d7f75b31a2a8fa987276ce3eee56cbf4fcbc00f8f9060ad54df2f':
        raise ValueError(f'Unexpected Global animation group ({len(selected)} entries)')
    return data
