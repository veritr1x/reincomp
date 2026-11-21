"""Resolve scalar call signatures using the Unity player's matching public symbols."""
import json
import re
import subprocess

SCALARS = {
    'void':'void', 'bool':'bool', 'float':'float', 'double':'double',
    'char':'int8_t', 'signed char':'int8_t', 'unsigned char':'uint8_t',
    'short':'int16_t', 'unsigned short':'uint16_t', 'int':'int32_t',
    'unsigned int':'uint32_t', 'long':'int32_t', 'unsigned long':'uint32_t',
    '__int64':'int64_t', 'unsigned __int64':'uint64_t',
}


def scalar_type(value):
    value = value.strip()
    if any(s in value for s in ('(', ')', '...', '::*')):
        return None
    if value.endswith(('*', '&', '*const', '* const')):
        return 'P'
    return value if value.startswith('enum ') else SCALARS.get(value)


def signature(prototype):
    match = re.fullmatch(r'(.+?) __cdecl [^ (]+\((.*)\)', prototype)
    if not match or '(' in match[2] or ')' in match[2]:
        return None
    args = []
    if match[2] != 'void':
        start = depth = 0
        for i, ch in enumerate(match[2]):
            if ch == '<':
                depth += 1
            elif ch == '>':
                depth -= 1
            elif ch == ',' and depth == 0:
                args.append(scalar_type(match[2][start:i]))
                start = i+1
        args.append(scalar_type(match[2][start:]))
    result = (scalar_type(match[1]), *args)
    return None if None in result or 'void' in args else result


def engine_table(registrations, symbols, demangler, layouts):
    names = sorted({s for addr in registrations.values() for s in symbols[addr] if s.startswith('?')})
    result = subprocess.run([demangler], input='\n'.join(names)+'\n', capture_output=True, text=True, check=True)
    prototypes = {}
    for block in result.stdout.split('\n\n'):
        lines = block.strip().splitlines()
        if len(lines) == 2:
            prototypes[lines[0]] = signature(lines[1])
    entries = []
    for name, address in sorted(registrations.items()):
        candidates = symbols[address]
        if len(candidates) > 1:
            typename, method = name.split('::', 1)
            typename = re.split(r'[./+]', typename)[-1]
            method = method.partition('(')[0]
            needles = [f'?{typename}_CUSTOM_{method}@@']
            if method.startswith(('get_', 'set_')):
                op, prop = method.split('_', 1)
                needles.append(f'?{typename}_{op.capitalize()}_Custom_Prop{prop}@@')
            prefixes = tuple(n.lower() for n in needles)
            candidates = [s for s in candidates if s.lower().startswith(prefixes)] or candidates
        sigs = {prototypes.get(s) for s in candidates}
        if len(sigs) != 1 or None in sigs:
            continue
        sig = list(sigs.pop())
        for i, item in enumerate(sig):
            if item.startswith('enum '):
                choices = {a[i] for a in layouts.get((name.partition('(')[0],len(sig)), ())}
                if len(choices) != 1 or not choices <= {'int32_t', 'uint32_t'}:
                    break
                sig[i] = choices.pop()
        else:
            entries.append('{%s,0x%x,invoke<%s>},' % (json.dumps(name),address,','.join(sig)))
    return entries
