#include <remill/Arch/AArch64/Runtime/State.h>
#include <cstdint>

#ifndef TRACE_NAME
#define TRACE_NAME sub_1000
#define TRACE_PC 0x1000
#define TRACE_RESULT 42
#endif

extern "C" {
uint64_t __bunker_poll_tls_offset = 0;
static unsigned deliveries = 0;
int __bunker_guest_poll_requested() { return 1; }
void __bunker_guest_poll_slow(State *, uint64_t entry, uint64_t ordinal, uint64_t pc) {
    if (entry == TRACE_PC && ordinal == 0 && pc == TRACE_PC) ++deliveries;
}
void *TRACE_NAME(State *, uint64_t, void *);
int start() {
    static State registers{};
    TRACE_NAME(&registers, TRACE_PC, nullptr);
    return deliveries == 1 && registers.gpr.x0.qword == TRACE_RESULT ? 42 : 1;
}
}
