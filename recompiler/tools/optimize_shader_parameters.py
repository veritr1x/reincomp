"""Compile the Unity shader parameter preparers into their draw-submission caller."""
import re

from render_shard_pack import DEFINITION, build_pack

SHARD_SHA256 = 'acfb531ed6def52d9d0e4459c6e00bb53ea9edff1b39f68b4210ee7e325b95b0'
PARENT = '705f1798'
PREPARERS = ('705f44b8', '705f45cc', '705f46e0')


def specialize(match):
    name = match[1]
    body = match[0]
    if name == PARENT:
        for child in PREPARERS:
            # Keep the public address used for guest dispatch and explicit
            # call frames. Only the resolved fast path calls this private copy.
            body, count = re.subn(r'(\bcall ptr )@sub_' + child + r'\(', r'\1@local_' + child + '(', body)
            if count != 1:
                raise ValueError(f'Expected one direct call to {child}.')
        return body
    if name in PREPARERS:
        body = body.replace('define ptr @sub_' + name, 'define internal ptr @local_' + name, 1)
        return body.replace('local_unnamed_addr {', 'local_unnamed_addr alwaysinline {', 1)
    return f'declare ptr @sub_{name}(ptr,i64,ptr)'


def compile_parameters(shard_ir, work, compile_ir, *, inline_stores=False):
    optimized = work / 'optimized.ll'
    compile_ir(shard_ir, optimized, '-O1', '-S', '-emit-llvm')
    parent = work / 'parameters.ll'
    declarations = ''.join(f'\ndeclare ptr @sub_{child}(ptr,i64,ptr)\n' for child in PREPARERS)
    ir = DEFINITION.sub(specialize, optimized.read_text())
    # The runtime returns the incoming Memory* after each store. LLVM can
    # keep that pointer in a register across successive parameter writes.
    def store_declaration(match):
        width = match[1]
        if inline_stores and width in ('8', '32', '64'):
            return f"""define internal ptr @__remill_write_memory_{width}(ptr %token, i64 %address, i{width} %value) alwaysinline {{
    %destination = inttoptr i64 %address to ptr
    store volatile i{width} %value, ptr %destination, align 1
    ret ptr %token
}}"""
        if inline_stores:
            return match[0]
        return match[0].replace('(ptr noundef,', '(ptr noundef returned,', 1)

    ir, annotated = re.subn(
        r'^declare [^\n]*?@__remill_write_memory_(8|16|32|64|f32|f64)\(ptr noundef,[^\n]*',
        store_declaration, ir, flags=re.M)
    if annotated != 6:
        raise ValueError('Unexpected store declarations in the shader parameter shard.')
    parent.write_text(ir + declarations)
    remaining = work / 'remaining.ll'
    remaining.write_text(DEFINITION.sub(
        lambda m: f'declare ptr @sub_{PARENT}(ptr,i64,ptr)' if m[1] == PARENT else m[0], shard_ir.read_text()))
    parts = [work / 'remaining.o', work / 'parameters.o']
    compile_ir(remaining, parts[0], '-O1', '-c')
    compile_ir(parent, parts[1], '-O2', '-c')
    return parts, [{'sub_' + PARENT}]


if __name__ == '__main__':
    build_pack(__doc__, SHARD_SHA256, compile_parameters, allow_inline_stores=True)
