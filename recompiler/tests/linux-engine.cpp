// Compose two real Linux Unity transforms with the translated Android Unity routine.
#include "runtime.h"
#include <array>
#include <cstdio>
#include <string>
#include <dlfcn.h>
#include <link.h>

extern "C" Memory *sub_7091bce0(State &, addr_t, Memory *);

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    void *engine = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    link_map *map = nullptr;
    if (!engine || dlinfo(engine, RTLD_DI_LINKMAP, &map) || !map) {
        std::fprintf(stderr, "Loading UnityPlayer.so failed: %s\n", dlerror());
        return 1;
    }
    using Transform = void (*)(const float *, const float *, const float *, float *);
    auto transform = reinterpret_cast<Transform>(map->l_addr + std::stoull(argv[2]));
    const float identity[] = {0, 0, 0, 1}, unit[] = {1, 1, 1};
    const float origin[] = {0, 0, 0}, offset[] = {5, 7, 11}, scale[] = {2, 3, 4};
    std::array<float, 16> parent{}, child{}, result{};
    transform(origin, identity, scale, parent.data());
    transform(offset, identity, unit, child.data());
    const recomp::Trace matrix = {0x7091bce0, sub_7091bce0};
    recomp::Runtime runtime;
    runtime.AddTraces(&matrix, 1);
    State state{};
    state.gpr.x0.qword = uintptr_t(parent.data());
    state.gpr.x1.qword = uintptr_t(child.data());
    state.gpr.x2.qword = uintptr_t(result.data());
    state.gpr.x30.qword = 0x1000;
    runtime.Run(state, matrix.address);
    const std::array<float, 16> expected{2,0,0,0, 0,3,0,0, 0,0,4,0, 10,21,44,1};
    if (result != expected || state.gpr.pc.qword != 0x1000) {
        std::fputs("Linux Unity / translated matrix composition failed\n", stderr);
        return 1;
    }
    std::puts("PASS Linux Unity TRS and translated Unity parent/child matrix composition");
}
