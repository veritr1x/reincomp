from collections import defaultdict,Counter
from pathlib import Path
import json
import re
import subprocess
import symbols

root=Path(__file__).resolve().parent
repo=root.parents[1]
demangler=next((repo/'work/dl/llvm18').glob('*/bin/llvm-undname.exe'))
records=[line.split(' ',1) for line in (root/'registered-api.txt').read_text().splitlines()[2:]]
by_rva=defaultdict(list)
for symbol,rva in symbols.symbols.items():by_rva[rva].append(symbol)
raw_names=sorted({item for addr,_ in records for item in by_rva[int(addr,16)] if item.startswith('?')})
result=subprocess.run([demangler],input='\n'.join(raw_names)+'\n',text=True,capture_output=True)
demangled={}
for block in result.stdout.split('\n\n'):
    pieces=block.strip().splitlines()
    if len(pieces)==2:demangled[pieces[0]]=pieces[1]
types={'void':'void','bool':'bool','char':'int8_t','signed char':'int8_t','unsigned char':'uint8_t',
       'short':'int16_t','unsigned short':'uint16_t','int':'int32_t','unsigned int':'uint32_t',
       'long':'int32_t','unsigned long':'uint32_t','__int64':'int64_t','unsigned __int64':'uint64_t','float':'float','double':'double'}
def abi(t):
    t=t.strip()
    if any(x in t for x in ('(',')','...','::*')):return None
    if t.endswith(('*','&','*const','* const')):return 'P'
    if t.startswith('enum '):return t
    return types.get(t)
def signature(symbol):
    proto=demangled.get(symbol,'')
    m=re.fullmatch(r'(.+?) __cdecl [^ (]+\((.*)\)',proto)
    if not m or '(' in m[2] or ')' in m[2]:return None
    args=[]
    if m[2]!='void':
        start=depth=0
        for i,c in enumerate(m[2]):
            if c=='<':depth+=1
            elif c=='>':depth-=1
            elif c==',' and depth==0:
                args.append(abi(m[2][start:i]))
                start=i+1
            if depth<0:return None
        if depth:return None
        args.append(abi(m[2][start:]))
    ret=abi(m[1])
    if ret is None or None in args or 'void' in args:return None
    return tuple([ret]+args)
entries=[]
enum_requests=set()
managed=defaultdict(set)
metadata=root/'runtime-player/managed-enums.tsv'
if metadata.exists():
    for line in metadata.read_text().splitlines():
        name,*layout=line.split('\t')
        managed[name,len(layout)].add(tuple(layout))
for addr,name in records:
    rva=int(addr,16)
    candidates=by_rva[rva]
    if len(candidates)>1:
        typename,method=name.split('::',1)
        typename=re.split(r'[./+]',typename)[-1]
        method=method.split('(')[0]
        needles=[f'?{typename}_CUSTOM_{method}@@']
        if method.startswith(('get_','set_')):
            op,prop=method.split('_',1)
            needles.append(f'?{typename}_{op.capitalize()}_Custom_Prop{prop}@@')
        exact=[v for v in candidates if any(v.lower().startswith(n.lower()) for n in needles)]
        if exact:candidates=exact
    sigs={signature(v) for v in candidates}
    if len(sigs)!=1 or None in sigs:continue
    sig,=sigs
    if any(t.startswith('enum ') for t in sig):
        enum_requests.add(name.split('(')[0])
        layouts=managed[name.split('(')[0],len(sig)]
        if not layouts:continue
        converted=[]
        for index,t in enumerate(sig):
            if t.startswith('enum '):
                kinds={layout[index] for layout in layouts}
                if len(kinds)!=1 or not kinds<={'int32_t','uint32_t'}:break
                t=next(iter(kinds))
            converted.append(t)
        if len(converted)!=len(sig):continue
        sig=tuple(converted)
    entries.append((name,rva,sig))
entries.sort()
(root/'runtime-player/enum-requests.txt').write_text('\n'.join(sorted(enum_requests))+'\n')
(root/'engine-calls.inc').write_text(''.join('{%s,0x%x,invoke<%s>},\n'%(json.dumps(name),rva,','.join(sig)) for name,rva,sig in entries))
print('Windows engine calls with unambiguous scalar ABI',len(entries),'distinct signatures',len({s for _,_,s in entries}),flush=True)
subprocess.run(['C:/mingw64/bin/g++.exe','-O2','-std=c++17','-static','-shared','-DADDRESS_SIZE_BITS=64',
                '-I'+str(repo/'third_party/remill-include'),root/'engine-calls.cpp',repo/'host/windows_shared_preferences.cpp',
                '-lshlwapi','-lbcrypt','-ladvapi32','-lxinput9_1_0','-ldinput8','-ldxguid','-lgdi32','-lhid','-o',root/'runtime-player/EngineCalls.dll'],check=True)
