#include "runtime.h"
#include <iterator>

#define TRACE(pc) extern "C" Memory *sub_##pc(State &, addr_t, Memory *);
#include "game-traces.inc"
#undef TRACE

void addLinuxGameTraces(recomp::Runtime &runtime) {
    static const recomp::Trace traces[] = {
#define TRACE(pc) {0x##pc, sub_##pc},
#include "game-traces.inc"
#undef TRACE
    };
    runtime.AddTraces(traces,std::size(traces));
}

// Partial bootstrap builds still dispatch any external direct calls. This
// preserves import handling and reports the first untranslated guest address.
extern "C" Memory *__remill_jump(State &, addr_t, Memory *);
#define UNLINKED(pc) extern "C" Memory *sub_##pc(State &s, addr_t, Memory *m) { return __remill_jump(s,0x##pc,m); }
#include "unlinked.inc"
#undef UNLINKED
