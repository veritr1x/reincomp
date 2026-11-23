#include "cooperative_poll.h"
#include <cstdio>
#include <iterator>

// Four exported functions from the supported Global 3.7.1 IL2CPP library.
extern "C" {
Memory *sub_8254d1b0(State &, addr_t, Memory *);
Memory *sub_8254daf0(State &, addr_t, Memory *);
Memory *sub_8254db84(State &, addr_t, Memory *);
Memory *sub_8254db88(State &, addr_t, Memory *);
}

int main() {
    struct Array {
        uintptr_t klass, monitor, bounds, length;
    } array{0x123456789abcdef0, 0, 0, 71};
    struct String {
        uintptr_t klass, monitor;
        int32_t length;
        char16_t chars[4];
    } string{0, 0, 3, u"abc"};

    const recomp::Trace traces[] = {
        {0x8254d1b0, sub_8254d1b0}, {0x8254daf0, sub_8254daf0},
        {0x8254db84, sub_8254db84}, {0x8254db88, sub_8254db88},
    };
    const struct {
        const char *name;
        const void *object;
        uintptr_t expected;
    } calls[] = {
        {"array_length", &array, array.length}, {"object_get_class", &array, array.klass},
        {"string_length", &string, 3}, {"string_chars", &string, uintptr_t(string.chars)},
    };
    static_assert(std::size(calls) == std::size(traces));
    recomp::Runtime runtime;
    runtime.AddTraces(traces, std::size(traces));
    recomp::PollRequest request;
    addr_t polled = 0;
    recomp::PollBinding binding(request, [&](State &, recomp::PollSite site, const auto &) {
        polled = site.trace;
    });
    for (size_t i = 0; i < std::size(calls); ++i) {
        State state{};
        state.gpr.x0.qword = uintptr_t(calls[i].object);
        state.gpr.x30.qword = 0x1000;
        request.Request();
        runtime.Run(state, traces[i].address);
        if (state.gpr.x0.qword != calls[i].expected || state.gpr.pc.qword != 0x1000 ||
            polled != traces[i].address) {
            std::fprintf(stderr, "il2cpp_%s failed (result, return address or TLS poll)\n", calls[i].name);
            return 1;
        }
        std::printf("PASS il2cpp_%s\n", calls[i].name);
    }
}
