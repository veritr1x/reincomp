// The Unity 2019 IL2CPP C entry points take up to six integer/pointer arguments.
// Preserve SysV register contents at the boundary without reading absent C arguments.
.text
#define API(name, pc) .globl name ; .type name,@function ; name: ; mov $pc,%eax ; jmp .Linvoke ; .size name,.-name
#include "api.inc"
.Linvoke:
    push %r9
    push %r8
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    mov %rsp,%rsi
    mov %eax,%edi
    sub $8,%rsp
    call linux_il2cpp_invoke
    add $56,%rsp
    ret
.section .note.GNU-stack,"",@progbits
