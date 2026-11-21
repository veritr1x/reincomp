from pathlib import Path
import json
import re
import struct
import subprocess
import sys
root=Path(__file__).resolve().parent
exe=root/'release/UnityPlayer.dll'
binary=exe.read_bytes()
pe=struct.unpack_from('<I',binary,60)[0]
base=struct.unpack_from('<Q',binary,pe+48)[0]
sections=pe+24+struct.unpack_from('<H',binary,pe+20)[0]
table=[struct.unpack_from('<I',binary,sections+i*40+12)[0] for i in range(struct.unpack_from('<H',binary,pe+6)[0])]
symbols={name:table[int(sec,16)-1]+int(addr) for name,sec,addr in re.findall(
    r'S_PUB32 [^\n]*`([^`]+)`\r?\n\s*flags = function, addr = (\w+):(\d+)',(root/'publics.txt').read_text())}
(root/'symbols.json').write_text(json.dumps({n:v for n,v in symbols.items() if '@Umbra@@' in n or '@UmbraModule@@' in n},indent=1))
if __name__=='__main__':
    tool=next((root.parents[1]/'work/dl/llvm18').glob('*/bin/llvm-objdump.exe'))
    for key in sys.argv[1:]:
        addr,= [base+v for n,v in symbols.items() if n.startswith(key)]
        nextaddr=min((base+v for v in symbols.values() if base+v>addr), default=addr+1024)
        result=subprocess.check_output([tool,'-d','--no-show-raw-insn','--x86-asm-syntax=intel','--start-address='+hex(addr),'--stop-address='+hex(min(addr+2048,nextaddr)),exe],text=True)
        print(key,hex(addr),result[:6500])
