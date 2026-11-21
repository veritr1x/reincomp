"""Generate the Windows adapter tables from the original APK and Unity player."""
import argparse
from collections import defaultdict
import csv
import hashlib
import io
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
from zipfile import ZipFile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from engine_signatures import engine_table
from spring_methods import spring_entries

APK_HASH = '9ded0797ee607b6892b531ba3a02f4c4510774f5fe098ccada78ccff881c9209'
PLAYER_HASH = '2c0409bc76f3c69f741a94e0bc8c167ecab231dfc1a62f7c20e5ca8160d781d5'
PDB_HASH = '513ad9ff220565bfb202107256183b7960b39e7b7441f9e4de4138e274a55e99'
ROOT = Path(__file__).resolve().parents[2]


def check_file(path, expected):
    with path.open('rb') as stream:
        if hashlib.file_digest(stream, 'sha256').hexdigest() != expected:
            raise ValueError(f'Unexpected version of {path.name}')


class ArmImage:
    def __init__(self, data):
        self.data = data
        h = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
        if not h[0].startswith(b'\x7fELF\x02\x01') or h[2] != 183:
            raise ValueError('Expected an ARM64 little-endian ELF image')
        self.sections = [struct.unpack_from('<IIQQQQIIQQ', data, h[6] + i*h[11]) for i in range(h[12])]
        headers = [struct.unpack_from('<IIQQQQQQ', data, h[5] + i*h[9]) for i in range(h[10])]
        self.loads = [(va, va+size, offset) for kind, _, offset, va, _, size, _, _ in headers if kind == 1]

    def offset(self, address):
        for start, end, offset in self.loads:
            if start <= address < end:
                return address - start + offset
        raise ValueError(f'Unmapped ELF address {address:#x}')

    def word(self, address):
        return struct.unpack_from('<I', self.data, self.offset(address))[0]

    def exports(self):
        result = {}
        for section in self.sections:
            if section[1] != 11:  # SHT_DYNSYM
                continue
            string_table = self.sections[section[6]][4]
            for pos in range(section[4], section[4]+section[5], section[9]):
                name, info, _, index, addr, _ = struct.unpack_from('<IBBHQQ', self.data, pos)
                if index and (info & 15) == 2 and info >> 4 in (1, 2):
                    start = string_table + name
                    key = self.data[start:self.data.index(0, start)].decode('ascii')
                    result[key] = addr
        return result


def player_apis(player, il2cpp, heads):
    names = {s[:-1].decode() for s in re.findall(rb'\b(?:il2cpp_[a-zA-Z0-9_]+|UnityPluginLoad|UnityPluginUnload)\x00', player)}
    exports = il2cpp.exports()
    trace_entries = {int(s, 16)-0x80000000 for s in heads.read_text().split()}
    output = io.StringIO(newline='')
    writer = csv.writer(output)
    writer.writerow(['windows_player_name', 'arm_export_rva', 'translated_entry'])
    for name in sorted(names):
        addr = exports.get(name)
        writer.writerow((name, '' if addr is None else hex(addr), addr in trace_entries))
    return output.getvalue().encode()


def signed(bits, width):
    return (bits ^ (1 << (width-1))) - (1 << (width-1))


def android_jni(image):
    # This pinned registration routine branches to wrappers that pass the
    # managed name in x0 and the function address in x1 to the registrar.
    def registration(wrapper):
        regs = {}
        for pc in range(wrapper, wrapper+64, 4):
            word = image.word(pc)
            if word & 0x9f000000 == 0x90000000:  # ADRP
                imm = (((word >> 5) & 0x7ffff) << 2) | ((word >> 29) & 3)
                regs[word & 31] = (pc & ~4095) + signed(imm, 21) * 4096
            elif word & 0xffc00000 == 0x91000000 and ((word >> 5) & 31) in regs:  # ADD immediate
                regs[word & 31] = regs[(word >> 5) & 31] + ((word >> 10) & 4095)
            elif word >> 26 == 5 and pc + 4*signed(word & 0x3ffffff, 26) == 0x3afe28:
                start = image.offset(regs[0])
                name = image.data[start:image.data.index(0, start)]
                if name.startswith((b'UnityEngine.AndroidJNI::', b'UnityEngine.AndroidJNIHelper::')):
                    return '{"%s",%#x},' % (name.decode(), 0x70000000 + regs[1])
                return None
            else:
                return None
        return None

    rows = []
    for pc in range(0xdd318, 0xdd318+4*2400, 4):
        word = image.word(pc)
        if word >> 26 in (5, 37):
            row = registration(pc + 4*signed(word & 0x3ffffff, 26))
            if row:
                rows.append(row)
        if word >> 26 == 5 or word == 0xd65f03c0:
            break
    return rows


class WindowsPlayer:
    def __init__(self, data):
        self.data = data
        pe = struct.unpack_from('<I', data, 60)[0]
        self.base = struct.unpack_from('<Q', data, pe+48)[0]
        sections = pe + 24 + struct.unpack_from('<H', data, pe+20)[0]
        count = struct.unpack_from('<H', data, pe+6)[0]
        self.sections = [struct.unpack_from('<8sIIII', data, sections+i*40) for i in range(count)]

    def registrations(self):
        sections = {s[0].rstrip(b'\0'): s[1:] for s in self.sections}
        _, code_va, code_size, code_file = sections[b'.text']
        _, ro_va, ro_size, ro_file = sections[b'.rdata']
        _, _, data_size, data_file = sections[b'.data']
        code = self.data[code_file:code_file+code_size]
        ro = self.data[ro_file:ro_file+ro_size]

        def name_at(rva):
            at = rva-ro_va
            if not 0 <= at < len(ro):
                return None
            end = ro.find(0, at, at+1024)
            value = ro[at:end] if end >= 0 else b''
            return value.decode() if value.isascii() and b'::' in value else None

        def relative(i):
            return code_va+i+7+struct.unpack_from('<i', code, i+3)[0]

        result = {}
        for match in re.finditer(rb'\x48\x8d\x15....\x48\x8d\x0d', code, re.DOTALL):
            i = match.start()
            name = name_at(relative(i+7))
            if name:
                result[name] = relative(i)
        # Older modules use parallel, null-terminated arrays of name and
        # function pointers. The latter starts on the next 16-byte boundary.
        words = [v[0]-self.base for v in struct.iter_unpack('<Q', self.data[data_file:data_file+data_size])]
        i = 0
        while i < len(words):
            start = i
            names = []
            while i < len(words) and (name := name_at(words[i])):
                names.append(name)
                i += 1
            if names and i < len(words) and words[i] == -self.base:
                first_fn = (i+2) & ~1
                funcs = words[first_fn:first_fn+len(names)]
                if len(funcs) == len(names) and all(code_va <= f < code_va+code_size for f in funcs):
                    result.update(zip(names, funcs))
            i = max(start+1, i)
        if len(result) != 7722:
            raise ValueError(f'Unexpected Windows engine registration count {len(result)}')
        return result


def managed_enum_layouts(metadata):
    # Unity 2019.4 Global metadata v24. The native type indices used by these
    # internal calls are the by-value indices of their managed definitions.
    if struct.unpack_from('<III', metadata) != (0xfab11baf, 24, 264):
        raise ValueError('Unexpected IL2CPP metadata format')
    sections = [struct.unpack_from('<II', metadata, pos) for pos in range(8, 264, 8)]
    def table(index, fmt):
        start, size = sections[index]
        return list(struct.iter_unpack(fmt, memoryview(metadata)[start:start+size]))
    def name(index):
        pos = sections[2][0]+index
        return metadata[pos:metadata.index(0, pos)].decode()
    types = table(19, '<17i8H2I')
    methods = table(5, '<5iI4H')
    params = table(10, '<iIi')
    indices = {t[2]:t for t in types}
    storage = {}
    for t in types:
        if t[-2] & 2:
            base = indices.get(t[6])
            if base:
                storage[t[2]] = {'Int32':'int32_t', 'UInt32':'uint32_t'}.get(name(base[0]), 'other-enum')
    layouts = defaultdict(set)
    for t in types:
        parent, cname = t, name(t[0])
        while parent[4] in indices:
            parent = indices[parent[4]]
            cname = name(parent[0])+'/'+cname
        qualified = name(parent[1])+'.'+cname
        if not qualified.startswith(('Unity.', 'UnityEngine.')):
            continue
        for method in methods[t[10]:t[10]+t[17]]:
            key, _, ret, first, _, _, flags, iflags, _, count = method
            if not iflags & 0x1000:
                continue
            layout = [storage.get(ret, '-')]
            if not flags & 0x10:
                layout.append('-')
            layout.extend(storage.get(p[2], '-') for p in params[first:first+count])
            layouts[qualified+'::'+name(key),len(layout)].add(tuple(layout))
    return layouts


def generate(apk, unity, llvm, out, pdb=None):
    player = unity/'UnityPlayer.dll'
    for tool in ('llvm-pdbutil.exe','llvm-undname.exe'):
        if not (llvm/tool).is_file():
            raise ValueError(f'Missing {tool} in {llvm}. Use the complete LLVM Windows distribution.')
    pdb = pdb or unity/'UnityPlayer_Win64_il2cpp_x64.pdb'
    for path, expected in [(apk, APK_HASH), (player, PLAYER_HASH), (pdb, PDB_HASH)]:
        check_file(path, expected)
    windows = WindowsPlayer(player.read_bytes())
    with ZipFile(apk) as z:
        arm = ArmImage(z.read('lib/arm64-v8a/libil2cpp.so'))
        apis = player_apis(windows.data, arm, ROOT/'game/heads-il2cpp.txt')
        jni = android_jni(ArmImage(z.read('lib/arm64-v8a/libunity.so')))
        metadata = z.read('assets/bin/Data/Managed/Metadata/global-metadata.dat')
        layouts = managed_enum_layouts(metadata)
        springs = spring_entries(arm,metadata,ROOT/'game/heads-il2cpp.txt')
    publics = subprocess.check_output([llvm/'llvm-pdbutil.exe', 'dump', '-publics', pdb]).decode(errors='replace')
    symbols = defaultdict(list)
    for key, section, offset in re.findall(r'S_PUB32 [^\n]*`([^`]+)`\r?\n\s*flags = function, addr = (\w+):(\d+)', publics):
        symbols[windows.sections[int(section,16)-1][2]+int(offset)].append(key)
    engine = engine_table(windows.registrations(), symbols, llvm/'llvm-undname.exe', layouts)
    contents = {'player-apis.csv': apis, 'original-android-jni.inc': lines(jni), 'engine-calls.inc': lines(engine)}
    out.mkdir(parents=True, exist_ok=True)
    for filename, data in contents.items():
        if hashlib.sha256(data).hexdigest() != HASHES[filename]:
            raise ValueError(f'{filename} differs from the release ({len(data.splitlines())} rows)')
    with tempfile.TemporaryDirectory(dir=out) as tmp:
        contents['spring_group.entries']=springs
        for filename, data in contents.items():
            path = Path(tmp)/filename
            path.write_bytes(data)
            path.replace(out/filename)
    print(f'Generated {len(engine)} Windows engine calls, {len(jni)} Android JNI bindings, {len(apis.splitlines())-1} player APIs and {len(springs.splitlines())} spring entries.')


def lines(rows):
    return ('\r\n'.join(rows)+'\r\n').encode()


HASHES = {
    'player-apis.csv': '8495fb3c9893508dd395df39bd25ac7d8c14be9017e2bd84ae79232aef517ec6',
    'original-android-jni.inc': '47952b6f24342cd1124c2541016a6dfe1ec2c0554fdb4d84b368fb4cdf33ca3b',
    'engine-calls.inc': 'a35326d5a1760208f086133efe811cde765e8efa8c20bc749b4991bb9cdfbefb',
}

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    for flag in ['apk','unity','llvm','out']:
        ap.add_argument('--'+flag, type=Path, required=True)
    ap.add_argument('--unity-symbols', type=Path)
    args = ap.parse_args()
    generate(args.apk, args.unity, args.llvm, args.out, args.unity_symbols)
