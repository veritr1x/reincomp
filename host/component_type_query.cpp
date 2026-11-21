#include "../runtime/runtime.h"
#include "../runtime/cooperative_poll.h"
#include "component_type_query.h"

extern "C" Memory *__real_sub_703aea94(State &, addr_t, Memory *);

extern "C" Memory *__wrap_sub_703aea94(State &state, addr_t pc, Memory *memory) {
  using bunker::windows::ComponentTypeQueryFrame;
  // At this component enumeration call site the caller tests bit zero of w0.
  constexpr addr_t resume = 0x703bb070;
  auto *frame = ComponentTypeQueryFrame::current;
  const auto component = state.gpr.x0.qword;
  const auto requested = state.gpr.x1.qword;
  if (!frame || state.gpr.x30.qword != resume || !component || !requested)
    return __real_sub_703aea94(state, pc, memory);

  const auto hash = ((component >> 4) * 2654435761ull) ^ ((requested >> 4) * 2246822519ull)
                    ^ (component >> 16) ^ (requested >> 16);
  auto &entry = frame->entries[hash % 256];
  if (entry.component_class == component && entry.requested_class == requested &&
      !__bunker_guest_poll_requested()) {
    state.gpr.x0.qword = entry.result;
    state.gpr.pc.qword = resume;
    return memory;
  }
  auto *result = __real_sub_703aea94(state, pc, memory);
  if (state.gpr.pc.qword == resume)
    entry = {component, requested, state.gpr.x0.qword};
  return result;
}
