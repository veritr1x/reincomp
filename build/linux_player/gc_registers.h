#pragma once
#include "nonlocal_jmp.h"
#include <atomic>

// The original Boehm register-spill helper allocates its own ARM jmp_buf.
// Keep it registered only for the lifetime of that particular invocation.
extern "C" Memory *sub_825b0544(State &, addr_t, Memory *);
namespace linux_gc {
using Jumps = bunker::windows::NonlocalJumps;
struct SpillFrame {
    addr_t entrySp;
    uint64_t generation;
    SpillFrame *parent;
};
inline thread_local SpillFrame *currentFrame;
inline std::atomic<uint64_t> nextGeneration{1};
inline Jumps *jumps;
inline Memory *spill(State &state, addr_t pc, Memory *memory) {
    SpillFrame frame{state.gpr.sp.qword,nextGeneration.fetch_add(1),currentFrame};
    currentFrame = &frame;
    auto result = sub_825b0544(state,pc,memory);
    jumps->Forget(frame.entrySp - 0x140 + 8);
    currentFrame = frame.parent;
    return result;
}
inline std::optional<Jumps::Region> environment(addr_t env, size_t bytes) {
    for (auto frame=currentFrame; frame; frame=frame->parent) {
        if (bytes == Jumps::kJmpBytes && env == frame->entrySp - 0x140 + 8)
            return Jumps::Region{env,env+bytes,frame->generation};
    }
    return {};
}
}
