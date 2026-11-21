"""Make the translated GC external-stop tail call use the Windows thread adapter."""
from pathlib import Path
import re
import struct
import subprocess
import sys

path = Path(sys.argv[1])
objdump = sys.argv[2] if len(sys.argv) > 2 else 'C:/mingw64/bin/objdump.exe'
def listing(name):
    return subprocess.check_output([objdump,'-d','--disassemble='+name,path],text=True)

bridge, = re.findall(r'^([a-f0-9]+) <NativeStopWorldForLiveness>:',listing('NativeStopWorldForLiveness'),re.M)
edge, old_bytes, old_target = re.findall(
    r'^\s*([a-f0-9]+):\s+(e9(?: [0-9a-f]{2}){4})\s+jmp\s+([a-f0-9]+) <sub_825aad50>$',
    listing('sub_825b208c'),re.M)[0]
if listing('sub_825b208c').count('<sub_825aad50>')!=1:
    raise ValueError('Unexpected number of edges to the GC stop function')
address = int(edge,16)
with path.open('r+b') as f:
    header = f.read(4096)
    pe = struct.unpack_from('<I',header,60)[0]
    base = struct.unpack_from('<Q',header,pe+48)[0]
    sections = pe+24+struct.unpack_from('<H',header,pe+20)[0]
    for i in range(struct.unpack_from('<H',header,pe+6)[0]):
        n,rva,raw_size,raw = struct.unpack_from('<IIII',header,sections+i*40+8)
        delta = address-base-rva
        if 0<=delta and delta+5<=min(n,raw_size):
            f.seek(raw+delta)
            src = f.read(5)
            if src!=bytes.fromhex(old_bytes) or address+5+struct.unpack_from('<i',src,1)[0]!=int(old_target,16):
                raise ValueError('Translated tail call changed')
            f.seek(raw+delta+1)
            f.write(struct.pack('<i',int(bridge,16)-address-5))
            break
    else:
        raise ValueError('Liveness tail call outside native code')
if '<NativeStopWorldForLiveness>' not in listing('sub_825b208c'):
    raise ValueError('Converted tail call did not resolve to the native stop adapter')
