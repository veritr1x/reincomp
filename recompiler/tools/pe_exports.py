"""Print "rva name" for every named export of a PE DLL (sorted by RVA)."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
opt = pe + 24; magic = struct.unpack_from('<H', d, opt)[0]
dd = opt + (112 if magic == 0x20b else 96)
erva, esz = struct.unpack_from('<II', d, dd)
nsec = struct.unpack_from('<H', d, pe + 6)[0]; sopt = struct.unpack_from('<H', d, pe + 20)[0]
secs = [struct.unpack_from('<IIII', d, pe + 24 + sopt + i * 40 + 8) for i in range(nsec)]
def off(rva):
    for vs, va, rs, ro in secs:
        if va <= rva < va + max(vs, rs): return rva - va + ro
    raise ValueError(hex(rva))
e = off(erva)
nfun, nnames, afun, aname, aord = struct.unpack_from('<IIIII', d, e + 20)
out = []
for i in range(nnames):
    nrva = struct.unpack_from('<I', d, off(aname) + 4 * i)[0]
    o = struct.unpack_from('<H', d, off(aord) + 2 * i)[0]
    frva = struct.unpack_from('<I', d, off(afun) + 4 * o)[0]
    s = d[off(nrva):d.index(b'\0', off(nrva))].decode()
    out.append((frva, s))
for r, s in sorted(out): print('%x %s' % (r, s))
