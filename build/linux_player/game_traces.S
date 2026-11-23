// Each original ARM address and its translated Linux entry form one recomp::Trace.
// Emit the table directly so a full game does not need half a million C++ declarations.
.section .data.rel.ro,"aw",@progbits
.p2align 3
.globl linux_game_traces
.hidden linux_game_traces
.type linux_game_traces,@object
linux_game_traces:
#define TRACE(pc) .quad 0x##pc, sub_##pc
#include "game-traces.inc"
#undef TRACE
.size linux_game_traces, .-linux_game_traces
.globl linux_game_trace_count
.hidden linux_game_trace_count
.type linux_game_trace_count,@object
linux_game_trace_count:
    .quad (linux_game_trace_count-linux_game_traces)/16
.size linux_game_trace_count, 8

// Missing direct targets in partial builds go through normal guest dispatch.
// SysV passes State and Memory in RDI and RDX. Replace only the guest PC in RSI.
.text
.macro unresolved pc
    .globl sub_\pc
    .hidden sub_\pc
    .type sub_\pc,@function
sub_\pc:
    movabs $0x\pc, %rsi
    jmp __remill_jump
    .size sub_\pc, .-sub_\pc
.endm
#define UNLINKED(pc) unresolved pc
#include "unlinked.inc"
#undef UNLINKED
.section .note.GNU-stack,"",@progbits
