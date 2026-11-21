"""Compile Unity portal traversal and cell finalization tail calls into native loops."""
from render_shard_pack import DEFINITION, build_pack

LOOPS = {
    '70a2240c': ('70a223f8', '70a22404'),
    '70a25130': ('70a2511c', '70a25120', '70a25128'),
}
SHARD_SHA256 = '4459003ddd54ee58bb14fe5912ffd086fa2f4a71462cb51e557398cbde48d180'


def specialize(match, entry, hops):
    name = match[1]
    if name not in (*hops, entry):
        return f'declare ptr @sub_{name}(ptr, i64, ptr)'
    body = match[0]
    if name in hops:
        body = body.replace('define ptr @', 'define internal ptr @', 1)
        body = body.replace('local_unnamed_addr {', 'local_unnamed_addr alwaysinline {', 1)
    # The direct tail calls can share a native loop. Guest entry pointers
    # passed as arguments still resolve to their original public entry points.
    for target in (*hops, entry):
        private = f'sub_{entry}_fused' if target == entry else 'traversal_hop_' + target
        body = body.replace('@sub_' + target + '(', '@' + private + '(')
    return body


def compile_loops(shard_ir, work, compile_ir):
    optimized = work / 'optimized.ll'
    compile_ir(shard_ir, optimized, '-O1', '-S', '-emit-llvm')
    shard = optimized.read_text()
    remaining = work / 'remaining.ll'
    remaining.write_text(DEFINITION.sub(
        lambda m: f'declare ptr @sub_{m[1]}(ptr,i64,ptr)' if m[1] in LOOPS else m[0],
        shard_ir.read_text()))
    parts = [work / 'remaining.o']
    compile_ir(remaining, parts[0], '-O1', '-c')
    for entry, hops in LOOPS.items():
        private = work / 'private.ll'
        ir = DEFINITION.sub(lambda match: specialize(match, entry, hops), shard)
        ir += ''.join(f'\ndeclare ptr @sub_{name}(ptr,i64,ptr)\n' for name in (*hops, entry))
        private.write_text(ir)
        compile_ir(private, optimized, '-O2', '-S', '-emit-llvm')
        fused = optimized.read_text()
        function = f'@sub_{entry}_fused('
        begin = fused.index('define ptr ' + function)
        body = fused[begin:fused.index('\n}', begin)]
        if 'tailrecurse:' not in body or 'call ptr ' + function in body:
            raise ValueError(f'Compiler did not close the loop at {entry}.')
        optimized.write_text(fused.replace(function, f'@sub_{entry}(')
                             .replace(f'declare ptr @sub_{entry}(ptr, i64, ptr)', ''))
        obj = work / f'portal-{entry}.o'
        compile_ir(optimized, obj, '-O2', '-c')
        parts.append(obj)
    return parts, [{'sub_' + entry} for entry in LOOPS]


if __name__ == '__main__':
    build_pack(__doc__, SHARD_SHA256, compile_loops)
