"""Lower only original single-argument IL2CPP exports that are a field read or address."""
import csv
import re
import struct


def lower(source, library, inventory):
    with library.open('rb') as stream:
        header = stream.read(64)
        if header[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', header, 18)[0] != 183:
            raise ValueError('Expected the original little-endian AArch64 library')
        ph = struct.unpack_from('<Q', header, 32)[0]
        size, count = struct.unpack_from('<HH', header, 54)
        segments = []
        for i in range(count):
            stream.seek(ph + size * i)
            kind, flags, position, address, _, length = struct.unpack('<IIQQQQ', stream.read(40))
            if kind == 1 and flags & 1:
                segments.append((address, position, length))

        def instruction(address):
            for base, offset, length in segments:
                if base <= address <= base + length - 4:
                    stream.seek(offset + address - base)
                    return struct.unpack('<I', stream.read(4))[0]
            return 0

        def accessor(address):
            for _ in range(5):
                word = instruction(address)
                if word >> 26 != 5:  # B imm26
                    break
                jump = word & 0x03ffffff
                address += 4 * (jump if jump < (1 << 25) else jump - (1 << 26))
            if instruction(address + 4) != 0xd65f03c0:  # RET x30
                return None
            offset = word >> 10 & 4095
            opcode = word & 0xffc003ff
            if opcode == 0x91000000:  # ADD x0,x0,#imm12
                return '0', offset
            widths = {0xf9400000: 8, 0xb9400000: 4, 0x79400000: 2, 0x39400000: 1}
            if opcode in widths:  # LDR x0/w0, LDRH w0 or LDRB w0 from [x0,#imm]
                width = widths[opcode]
                return str(width), offset * width
            return None

        lowered = {}
        with inventory.open() as records:
            for row in csv.DictReader(records):
                if row['translated_entry'] == 'True':
                    name = row['windows_player_name']
                    leaf = accessor(int(row['arm_export_rva'], 16))
                    if leaf:
                        lowered[name] = leaf

    indices = {name: i for i, name in enumerate(sorted(lowered))}
    if len(indices) > 64:
        raise ValueError('More accessors than validation mask bits')
    def replace(match):
        name, result = match.groups()
        if name not in lowered:
            return match[0]
        width, offset = lowered[name]
        return f'LEAF({name},{result},{width},{offset},{indices[name]})'

    return re.sub(r'^R1\((il2cpp_\w+),([^,]+),P\)$', replace, source, flags=re.M)
