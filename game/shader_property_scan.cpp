// Inner loops shared by ShaderLab::shaderprops::GetFloat and GetVector in
// the global 3.7.1 Unity player. The rest of each trace stays in the lifter.
#include "../runtime/cooperative_poll.h"
#include <cstdint>

namespace {
struct __attribute__((packed, may_alias)) Key { uint32_t value; };

const recomp::PollFastView *Binding(uint64_t offset) {
  const recomp::PollFastView *binding;
  asm volatile("movq %%gs:(%1),%0" : "=r"(binding) : "r"(offset) : "memory");
  return binding;
}

void Compare(State &s, uint64_t lhs, uint64_t rhs) {
  const uint64_t difference = lhs - rhs;
  s.sr.n = difference >> 63;
  s.sr.z = lhs == rhs;
  s.sr.c = lhs >= rhs;
  s.sr.v = ((lhs ^ rhs) & (lhs ^ difference)) >> 63;
}

// A handler can edit registers, replace its TLS binding or leave the request
// pending. Resume individual guest iterations on this path after delivery.
int ObservedScan(State &s, addr_t head, addr_t hit, addr_t trace, uint64_t ordinal) {
  for (;;) {
    s.gpr.pc.qword = head;
    const auto index = s.gpr.x8.qword;
    const auto address = s.gpr.x11.qword + index * 4;
    const auto value = reinterpret_cast<const volatile Key *>(address)->value;
    s.gpr.x13.qword = value;
    if (value == s.gpr.x20.dword) {
      Compare(s, 0, 0);
      s.gpr.pc.qword = hit;
      return 1;
    }
    const auto next = index + 1;
    const auto end = s.gpr.x9.qword;
    s.gpr.x8.qword = next;
    s.gpr.x10.qword += s.gpr.x12.qword;
    Compare(s, next, end);
    const bool more = static_cast<int64_t>(next) < static_cast<int64_t>(end);
    s.gpr.pc.qword = more ? head : head + 28;
    const auto tls = __bunker_poll_tls_offset;
    const auto *binding = tls ? Binding(tls) : nullptr;
    if ((!tls || (binding && binding->requested->load(std::memory_order_acquire) != binding->delivered)) &&
        __bunker_guest_poll_requested())
      __bunker_guest_poll_slow(&s, trace, ordinal, 0);
    if (!more) return 0;
  }
}
}

extern "C" int shader_scan(State *state, addr_t entry, addr_t trace, uint64_t ordinal) {
  auto &s = *state;
  const auto head = entry + (ordinal == 1 ? 164 : 348);
  const auto hit = head + (ordinal == 1 ? 108 : 32);
  const auto begin = s.gpr.x8.qword, end = s.gpr.x9.qword, ids = s.gpr.x11.qword;
  const auto tls = __bunker_poll_tls_offset;
  if (!tls || end > INT32_MAX || begin >= end || ids > UINT64_MAX - end * 4)
    return ObservedScan(s, head, hit, trace, ordinal);
  const auto registers = reinterpret_cast<uintptr_t>(state);
  if (ids + begin * 4 < registers + sizeof(State) && ids + end * 4 > registers)
    return ObservedScan(s, head, hit, trace, ordinal);

  // TLS and delivered belong to this worker until it calls a handler. Another
  // worker can request a poll at any time, including at the final comparison.
  const auto *binding = Binding(tls);
  const auto *epoch = binding ? binding->requested : nullptr;
  const auto delivered = binding ? binding->delivered : 0;
  const auto key = s.gpr.x20.dword;
  const auto initial = s.gpr.x10.qword, step = s.gpr.x12.qword;
  uint64_t index = begin;
  uint32_t value;
  bool pending = false;
  // Check the bound once for each complete group. Still load one key and
  // check for a pending poll after every failed guest comparison.
  while (end - index >= 4) {
#pragma GCC unroll 4
    for (unsigned lane = 0; lane < 4; ++lane) {
      value = reinterpret_cast<const volatile Key *>(ids + index * 4)->value;
      if (value == key) goto found;
      ++index;
      pending = epoch && epoch->load(std::memory_order_acquire) != delivered;
      if (pending) goto missed;
    }
  }
  while (index < end) {
    value = reinterpret_cast<const volatile Key *>(ids + index * 4)->value;
    if (value == key) goto found;
    ++index;
    pending = epoch && epoch->load(std::memory_order_acquire) != delivered;
    if (pending) break;
  }
missed:
  s.gpr.x8.qword = index;
  s.gpr.x10.qword = initial + (index - begin) * step;
  s.gpr.x13.qword = value;
  Compare(s, index, end);
  s.gpr.pc.qword = index == end ? head + 28 : head;
  if (pending && __bunker_guest_poll_requested())
    __bunker_guest_poll_slow(&s, trace, ordinal, 0);
  if (index == end) return 0;
  return ObservedScan(s, head, hit, trace, ordinal);
found:
  s.gpr.x8.qword = index;
  s.gpr.x10.qword = initial + (index - begin) * step;
  s.gpr.x13.qword = value;
  Compare(s, 0, 0);
  s.gpr.pc.qword = hit;
  return 1;
}
