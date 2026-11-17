#pragma once
// Opt-in translated-code polling. Signal policy, native-provider root scopes
// and collector suspension are separate responsibilities of its caller.
#include <cfenv>
#include "runtime.h"
#include <atomic>
#include <memory>

namespace recomp {
struct PollBindingState;
class PollRequest {
 public:
  // May be called by another thread; requests arriving during delivery remain
  // pending for the next poll. Overflow throws rather than wrapping epochs.
  uint64_t Request();
  PollRequest() = default;
  PollRequest(const PollRequest &) = delete;
  PollRequest &operator=(const PollRequest &) = delete;
  uint64_t Sequence() const noexcept { return requested_.load(std::memory_order_acquire); }
 private:
  std::atomic<uint64_t> requested_{0};
  std::atomic<uint64_t> delivered_{0};
  std::atomic<bool> bound_{false};
  friend class PollBinding;
  friend struct PollBindingState;
};
struct PollSite { addr_t trace; uint64_t ordinal, epoch; };
struct PollAdmissionContext {
  const State &state;
  PollSite site;
  // At ordinal zero this is the incoming trace PC, before Remill's prologue
  // updates state.gpr.pc. State.pc is unmodified and can still name the caller.
  addr_t program_counter;
  const std::vector<const State *> &frames;
};
class PollBinding {
 public:
  using Handler = std::function<void(State &, PollSite, const std::vector<State *> &)>;
  using Admission = std::function<bool(const PollAdmissionContext &)>;
  // Bind on the guest's native worker, surrounding Runtime::Run. One request
  // object can bind to only one worker at a time. Nested bindings restore their
  // predecessor; nested guest calls inside a handler do not recursively poll it.
  // Optional admission runs before any State/reservation change or delivery.
  // False or throw leaves the request pending and restores native errno/fenv.
  // It must be bounded and read-only: no parking, guest execution or locks.
  // Context references are borrowed only for the duration of this decision.
  PollBinding(PollRequest &, Handler, Admission = {});
  ~PollBinding();
  PollBinding(const PollBinding &) = delete;
  PollBinding &operator=(const PollBinding &) = delete;
 private:
  std::unique_ptr<PollBindingState> state_;
};
}  // namespace recomp
// Compiled poll sites (poll-instrument --inline-poll) read this thread's
// binding inline and call __bunker_guest_poll_requested only when its request
// epoch differs from the epoch it last delivered. The binding starts with
// PollFastView. On Windows the binding pointer is read from the TEB TLS slot
// at byte offset __bunker_poll_tls_offset (0 = not inline-readable: always
// call); other hosts read the thread_local __bunker_poll_binding.
namespace recomp { struct PollFastView { const std::atomic<uint64_t> *requested; uint64_t delivered; }; }
#ifdef _WIN32
extern "C" uint64_t __bunker_poll_tls_offset;
#else
extern "C" thread_local recomp::PollFastView *__bunker_poll_binding;
#endif
extern "C" uint32_t __bunker_guest_poll_requested();
extern "C" void __bunker_guest_poll_slow(State *, uint64_t trace, uint64_t ordinal, addr_t entry_pc);
