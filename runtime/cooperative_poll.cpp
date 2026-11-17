#include "cooperative_poll.h"
#include "fast_tls.h"
#include <cerrno>
#include <cfenv>
#include <limits>
#include <stdexcept>
#include <thread>

static_assert(sizeof(std::atomic<uint64_t>) == 8 && std::atomic<uint64_t>::is_always_lock_free, "request epochs are plain 64-bit words for compiled code");
namespace recomp {
struct PollBindingState {
  // First member: compiled poll sites read it through the thread's binding pointer.
  PollFastView fast;
  PollRequest *request;
  PollBinding::Handler handler;
  PollBinding::Admission admission;
  PollBindingState *previous;
  std::thread::id owner;
  bool delivering = false;
  void Complete(uint64_t epoch) {
    fast.delivered = epoch;
    request->delivered_.store(epoch, std::memory_order_release);
  }
};
namespace {
struct BindingTag;
#ifdef _WIN32
constexpr ThreadLocalRef<PollBindingState *, BindingTag> binding{};
void SetBinding(PollBindingState *state) { binding = state; }
#else
// The thread_local is itself the binding; compiled code reads it directly.
struct BindingRef {
  operator PollBindingState *() const noexcept { return reinterpret_cast<PollBindingState *>(__bunker_poll_binding); }
};
constexpr BindingRef binding{};
void SetBinding(PollBindingState *state) { __bunker_poll_binding = state ? &state->fast : nullptr; }
#endif
}  // namespace
uint64_t PollRequest::Request() {
  auto current = requested_.load(std::memory_order_relaxed);
  do {
    if (current == std::numeric_limits<uint64_t>::max()) throw std::overflow_error("poll request epoch exhausted");
  } while (!requested_.compare_exchange_weak(current, current + 1, std::memory_order_release, std::memory_order_relaxed));
  return current + 1;
}
PollBinding::PollBinding(PollRequest &request, Handler handler, Admission admission) {
  if (!handler) throw std::invalid_argument("empty cooperative poll handler");
  auto state = std::make_unique<PollBindingState>(PollBindingState{{&request.requested_, 0}, &request, std::move(handler), std::move(admission), binding, std::this_thread::get_id()});
  if (static_cast<void *>(&state->fast) != static_cast<void *>(state.get())) std::terminate();
  if (request.bound_.exchange(true, std::memory_order_acq_rel)) throw std::invalid_argument("poll request already bound to a worker");
  state->fast.delivered = request.delivered_.load(std::memory_order_acquire);
  state_ = std::move(state);
  SetBinding(state_.get());
}
PollBinding::~PollBinding() {
  if (binding != state_.get() || state_->owner != std::this_thread::get_id()) std::terminate();
  SetBinding(state_->previous);
  state_->request->bound_.store(false, std::memory_order_release);
}
}  // namespace recomp
#ifdef _WIN32
// Byte offset of the binding's TEB TLS slot, published once during static
// initialization (before any guest code runs). 0 keeps sites on the call path.
uint64_t __bunker_poll_tls_offset = [] {
  const auto index = recomp::binding.slot().index();
  return index < 64 ? uint64_t(0x1480 + index * 8) : uint64_t(0);
}();
#else
thread_local recomp::PollFastView *__bunker_poll_binding = nullptr;
#endif
extern "C" uint32_t __bunker_guest_poll_requested() {
  const recomp::PollBindingState *binding = recomp::binding;
  return binding && !binding->delivering && binding->request->Sequence() != binding->fast.delivered;
}
extern "C" void __bunker_guest_poll_slow(State *state, uint64_t trace, uint64_t ordinal, addr_t entry_pc) {
  recomp::PollBindingState *binding = recomp::binding;
  if (!binding || binding->delivering || !state) throw std::invalid_argument("invalid cooperative poll boundary");
  const auto epoch = binding->request->Sequence();
  if (epoch == binding->fast.delivered) return;
  const int saved_errno = errno;
  // Admission is a host predicate that runs no guest code and leaves the FP
  // environment alone. A pending request is commonly rejected at every compiled
  // poll site until the thread reaches an admitted provider wait, so that path
  // must not pay for a full FP environment save/restore.
  struct Delivering {
    recomp::PollBindingState &binding; int error;
    Delivering(recomp::PollBindingState &b, int e) : binding(b), error(e) { binding.delivering = true; }
    ~Delivering() { binding.delivering = false; errno = error; }
  } delivering(*binding, saved_errno);
  const auto frames = recomp::CurrentExecutionFrames(*state);
  // Entry State.pc can still name the caller. Expose the authoritative PC to
  // admission without modifying State until the decision actually admits it.
  const addr_t interrupted_pc = ordinal ? state->gpr.pc.qword : entry_pc;
  const recomp::PollSite site{trace, ordinal, epoch};
  if (binding->admission) {
    const std::vector<const State *> read_only_frames(frames.begin(), frames.end());
    if (!binding->admission({*state, site, interrupted_pc, read_only_frames})) return;
  }
  struct Scope {
    fenv_t environment;
    State &state;
    explicit Scope(State &s) : state(s) { std::fegetenv(&environment); }
    ~Scope() {
      std::fesetenv(&environment);
      const int modes[] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
      std::fesetround(modes[state.fpcr.rmode]);
    }
  } scope(*state);
  if (!ordinal) state->gpr.pc.qword = entry_pc;
  // Explicit delivery invalidates exclusive reservations. Fast predicates do
  // not. No memory/provider lock is held while invoking the user handler.
  __remill_aarch64_clear_exclusive(*state);
  binding->handler(*state, site, frames);
  if (state->gpr.pc.qword != interrupted_pc)
    throw std::invalid_argument("cooperative poll cannot redirect the translated continuation PC");
  // A later request cannot be consumed by this delivery. Throwing handlers
  // leave this request pending, and all scope state is restored during unwind.
  binding->Complete(epoch);
}
