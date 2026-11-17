#ifdef _WIN32
#include <windows.h>
#endif
#include <atomic>
#include <cfenv>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>

#include "runtime.h"
#include "fast_tls.h"
#include "fast_fenv.h"
#include "aarch64_fallback.h"
#include <functional>
#include <unordered_map>

namespace {
struct ActiveTag; constexpr recomp::ThreadLocalRef<recomp::Runtime *, ActiveTag> active{};
// ARM IDC is not x86's denormal-operand status bit. Instructions that flush
// an input explicitly raise IDC; ordinary subnormal inputs must not set it.
struct DenormalTag; constexpr recomp::ThreadLocalRef<int, DenormalTag> explicit_denormal{};
// See __bunker_set_fpsr_unobserved. Written before guest code runs.
bool fpsr_unobserved = false;
[[noreturn]] void fail(const char *reason, addr_t pc) {
  std::fprintf(stderr, "recomp: %s at %#llx\n", reason, static_cast<unsigned long long>(pc));
  std::abort();
}

constexpr addr_t kReservationGranule = 64;
struct Reservation {
  addr_t address = 0;
  uint64_t size = 0;
  uint128_t value = 0;
  uint64_t epoch = 0;
  bool pair = false;
  bool held = false;
};
struct GuestCallFrame {
  GuestCallFrame *previous;
  addr_t expected_pc;
  addr_t caller_sp;
  bool root;
};
struct PendingGuestTransfer {
  bool active = false;
  addr_t pc = 0;
  addr_t sp = 0;
  Memory *memory = nullptr;
};
constexpr bool kNoTerminalPending = false;
// The first fields are read and written inline by lifted call sites
// (poll_instrument --direct-calls); their offsets are fixed below.
struct ExecutionContext {
  GuestCallFrame *calls = nullptr;
  const bool *terminal_pending = &kNoTerminalPending;  // &terminal->requested_
  PendingGuestTransfer transfer{};
  bool call_protocol_used = false;
  State *state = nullptr;
  ExecutionContext *previous = nullptr;
  recomp::ThreadExitDomain *terminal = nullptr;
  Reservation reservation{};
};
static_assert(offsetof(GuestCallFrame, previous) == 0 && offsetof(GuestCallFrame, expected_pc) == 8 &&
              offsetof(GuestCallFrame, caller_sp) == 16 && offsetof(GuestCallFrame, root) == 24 &&
              sizeof(GuestCallFrame) == 32, "inline guest-call frame layout");
static_assert(offsetof(ExecutionContext, calls) == 0 && offsetof(ExecutionContext, terminal_pending) == 8 &&
              offsetof(ExecutionContext, transfer) == 16 && offsetof(PendingGuestTransfer, active) == 0 &&
              offsetof(ExecutionContext, call_protocol_used) == 48, "inline guest-call context layout");
struct ExecutionTag; constexpr recomp::ThreadLocalRef<ExecutionContext *, ExecutionTag> execution{};

bool returns_to(const State &state, const GuestCallFrame &frame) {
  return state.gpr.pc.qword == frame.expected_pc && state.gpr.sp.qword == frame.caller_sp;
}
GuestCallFrame *matching_return(ExecutionContext &context, addr_t pc, addr_t sp) {
  for (auto *frame=context.calls; frame; frame=frame->previous)
    if (frame->expected_pc==pc && frame->caller_sp==sp) return frame;
  return nullptr;
}
Memory *finish_guest_call(ExecutionContext &context, GuestCallFrame &frame,
                          State &state, Memory *memory) {
  for (;;) {
    if (context.terminal && context.terminal->Pending()) {
      if (memory!=context.terminal->MemoryToken())
        fail("terminal thread exit changed its Memory token",state.gpr.pc.qword);
      return memory; // The caller's existing IR guard retires this native frame.
    }
    if (context.transfer.active) {
      if (state.gpr.pc.qword!=context.transfer.pc || state.gpr.sp.qword!=context.transfer.sp ||
          memory!=context.transfer.memory)
        fail("pending guest transfer State/token changed during propagation",state.gpr.pc.qword);
    } else {
      if (returns_to(state,frame)) return memory;
      context.transfer={true,state.gpr.pc.qword,state.gpr.sp.qword,memory};
    }
    auto *destination=matching_return(context,context.transfer.pc,context.transfer.sp);
    if (destination==&frame) {
      context.transfer={};
      return memory;
    }
    if (destination) return memory; // The caller's IR guard unwinds one host frame.
    if (frame.caller_sp!=context.transfer.sp) {
      if (frame.root) fail("guest transfer has no owner in this Run",context.transfer.pc);
      return memory;
    }
    const auto resume=context.transfer;
    if (!active || !active->HasGuestTrace(resume.pc))
      fail("unselectable guest resume target",resume.pc);
    // The intervening native guest frames have already returned via their
    // guards. Dispatch only at the frame with the restored caller SP, without
    // resetting FENV, constructing a new Run, or changing architectural State.
    context.transfer={};
    memory=active->Dispatch(state,resume.pc,memory);
    if (!context.transfer.active && state.gpr.pc.qword==resume.pc && state.gpr.sp.qword==resume.sp)
      fail("guest resume returned without a continuation",resume.pc);
    // A nested rethrow may have installed a distinct pending transfer while
    // this landing body was running. The same loop handles it independently.
  }
}
// Every exclusive load/store and every plain store made while a reservation is
// live takes this lock for a few hundred nanoseconds. MinGW's std::mutex
// (winpthreads) parks contending threads on a kernel semaphore; a slim
// reader/writer lock spins briefly first, which suits these short sections.
class TrackingLock {
 public:
#ifdef _WIN32
  void lock() noexcept { AcquireSRWLockExclusive(&lock_); }
  void unlock() noexcept { ReleaseSRWLockExclusive(&lock_); }
 private:
  SRWLOCK lock_ = SRWLOCK_INIT;
#else
  void lock() { lock_.lock(); }
  void unlock() noexcept { lock_.unlock(); }
 private:
  std::mutex lock_;
#endif
};
// Each reservation granule hashes to one epoch in a fixed table. Writes bump
// the epochs of the granules they touch; a store-exclusive succeeds only if
// its granule's epoch is unchanged since the load-exclusive. Two granules that
// share a slot can only make a store-exclusive fail spuriously, which AArch64
// permits (guest code retries). Unlike per-granule heap records, this
// allocates nothing on the exclusive path. 64-bit epochs never wrap in practice.
constexpr size_t kEpochTableBits = 16;
struct MemoryTracking {
  TrackingLock lock;
  // Atomic so plain guest stores can bump them without the lock (see
  // invalidate_if_reserved). Exclusive operations still run under the lock.
  std::atomic<uint64_t> epochs[size_t(1) << kEpochTableBits] = {};
};
MemoryTracking &memory_tracking() {
  static MemoryTracking tracking;
  return tracking;
}
inline std::atomic<uint64_t> &granule_epoch_unlocked(addr_t granule) {
  return memory_tracking().epochs[size_t((granule / kReservationGranule) * UINT64_C(0x9E3779B97F4A7C15) >> (64 - kEpochTableBits))];
}
// Number of held reservations, changed only under the tracking lock. Plain
// guest stores read it without the lock: with no live reservation there is
// nothing to invalidate, so ordinary loads/stores never touch the lock.
std::atomic<size_t> live_reservations{0};
void check_range(addr_t address, size_t size) {
  if (size && size - 1 > std::numeric_limits<addr_t>::max() - address)
    throw std::invalid_argument("guest memory range overflows address space");
}
void invalidate_unlocked(addr_t address, size_t size) {
  if (!size) return;
  const addr_t first = address & ~(kReservationGranule - 1);
  const addr_t last = (address + size - 1) & ~(kReservationGranule - 1);
  if ((last - first) / kReservationGranule >= (size_t(1) << kEpochTableBits)) {
    for (auto &epoch : memory_tracking().epochs) epoch.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  for (addr_t granule = first;; granule += kReservationGranule) {
    granule_epoch_unlocked(granule).fetch_add(1, std::memory_order_relaxed);
    if (granule == last) break;
  }
}
void clear_reservation_unlocked(Reservation &reservation) {
  if (reservation.held) live_reservations.fetch_sub(1, std::memory_order_seq_cst);
  reservation = Reservation{};
}
void clear_current_reservation() {
  if (!execution || !execution->reservation.held) return;
  std::lock_guard<TrackingLock> lock(memory_tracking().lock);
  clear_reservation_unlocked(execution->reservation);
}
ExecutionContext &exclusive_context(State &state, addr_t address, uint64_t size, bool pair = false) {
  if (!execution || execution->state != &state) fail("exclusive access outside active execution", address);
  if (pair ? (size != 8 && size != 16) : (size != 1 && size != 2 && size != 4 && size != 8))
    fail("unsupported exclusive access width", address);
  if (address % size) fail("unaligned exclusive access", address);
  return *execution;
}

template <typename T> uint64_t exclusive_load_unlocked(addr_t address) {
  return __atomic_load_n(reinterpret_cast<const T *>(address), __ATOMIC_SEQ_CST);
}
template <typename T> bool exclusive_store_unlocked(addr_t address, uint64_t expected, uint64_t desired) {
  T previous = static_cast<T>(expected);
  return __atomic_compare_exchange_n(reinterpret_cast<T *>(address), &previous, static_cast<T>(desired),
                                     false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
// The live count is raised before the value is loaded, so a concurrent
// lock-free plain store either precedes the load or observes the live
// reservation and invalidates it. Returns the granule's current epoch.
uint64_t publish_granule_unlocked(addr_t address) {
  live_reservations.fetch_add(1, std::memory_order_seq_cst);
  return granule_epoch_unlocked(address & ~(kReservationGranule - 1)).load(std::memory_order_seq_cst);
}
void reserve_unlocked(ExecutionContext &context, uint64_t epoch,
                      addr_t address, uint64_t size, uint128_t value, bool pair) {
  context.reservation = {address, size, value, epoch, pair, true};
}
bool reservation_current_unlocked(const Reservation &reservation) {
  return reservation.held && granule_epoch_unlocked(reservation.address & ~(kReservationGranule - 1)).load(std::memory_order_seq_cst) == reservation.epoch;
}
// Typed guest accesses compile to single moves: aligned 1/2/4/8-byte accesses
// stay single-copy atomic as on AArch64, and take no lock.
template <typename T> T load(addr_t address) {
  T value;
  std::memcpy(&value, reinterpret_cast<const void *>(address), sizeof(value));
  return value;
}
// A plain store's data is written before this runs, outside any lock, so the
// tracking lock never ordered it against a store-exclusive; it only serialized
// the epoch bump. A failed compare-and-swap leaves a reservation held, so some
// thread almost always has one and every guest store took the lock (3% of
// UnityMain). Atomic bumps keep the same semantics without it: the
// store-exclusive's compare-and-swap still fails on any changed value, and an
// epoch bump is never lost.
void invalidate_if_reserved(addr_t address, size_t size) {
  if (live_reservations.load(std::memory_order_relaxed)) {
#if !(defined(__x86_64__) || defined(_M_X64))
    // x86-64: the epoch bump below is a locked RMW, already a full barrier
    // that orders the data store before it; an extra MFENCE per guest store
    // only cost time (Windows and Linux hosts alike).
    std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
    invalidate_unlocked(address, size);
  }
}
template <typename T> Memory *store(Memory *memory, addr_t address, T value) {
  std::memcpy(reinterpret_cast<void *>(address), &value, sizeof(value));
  invalidate_if_reserved(address, sizeof(value));
  return memory;
}
Memory *dispatch(State &state, addr_t address, Memory *memory) {
  if (!active) fail("no active runtime for indirect target", address);
  return active->Dispatch(state, address, memory);
}
inline __attribute__((always_inline)) int host_exceptions(int mask) {
  int flags = 0;
  if (mask & kFPUExceptionInvalid) flags |= FE_INVALID;
  if (mask & kFPUExceptionDivByZero) flags |= FE_DIVBYZERO;
  if (mask & kFPUExceptionOverflow) flags |= FE_OVERFLOW;
  if (mask & kFPUExceptionUnderflow) flags |= FE_UNDERFLOW;
  if (mask & kFPUExceptionPrecision) flags |= FE_INEXACT;
  return flags;
}
inline __attribute__((always_inline)) int guest_exceptions(int flags) {
  int mask = 0;
  if (flags & FE_INVALID) mask |= kFPUExceptionInvalid;
  if (flags & FE_DIVBYZERO) mask |= kFPUExceptionDivByZero;
  if (flags & FE_OVERFLOW) mask |= kFPUExceptionOverflow;
  if (flags & FE_UNDERFLOW) mask |= kFPUExceptionUnderflow;
  if (flags & FE_INEXACT) mask |= kFPUExceptionPrecision;
  return mask;
}
int host_rounding(int mode) {
  switch (mode) {
    case kFPURoundToNearestEven: return FE_TONEAREST;
    case kFPURoundUpInf: return FE_UPWARD;
    case kFPURoundDownNegInf: return FE_DOWNWARD;
    case kFPURoundToZero: return FE_TOWARDZERO;
    default: fail("invalid guest floating-point rounding mode", mode);
  }
}
}  // namespace

void recomp::ReadGuestMemory(void *destination, addr_t address, size_t size) {
  check_range(address, size);
  if (!size) return;
  std::memmove(destination, reinterpret_cast<const void *>(address), size);
}
void recomp::WithGuestMemoryRead(const std::function<void()> &read) {
  std::lock_guard<TrackingLock> lock(memory_tracking().lock);
  read();
}
void recomp::WriteGuestMemory(addr_t address, const void *source, size_t size) {
  check_range(address, size);
  if (!size) return;
  std::memmove(reinterpret_cast<void *>(address), source, size);
  invalidate_if_reserved(address, size);
}
void recomp::WithGuestMemoryWrite(addr_t address, size_t size, const std::function<void()> &write) {
  check_range(address, size);
  std::lock_guard<TrackingLock> lock(memory_tracking().lock);
  // Invalidate before invoking the write, including its partial effects if it
  // throws. The callback must not reenter this non-recursive memory protocol.
  invalidate_unlocked(address, size);
  write();
}
bool recomp::WithGuestMemoryMutation(addr_t address, size_t size,
    const std::function<bool()> &prepare, const std::function<void()> &commit) {
  if (!size || !prepare || !commit) throw std::invalid_argument("guest memory mutation requires a nonempty range and native callbacks");
  check_range(address, size);
  std::lock_guard<TrackingLock> lock(memory_tracking().lock);
  if (!prepare()) return false;
  invalidate_unlocked(address, size);
  commit();
  return true;
}
void recomp::InvalidateGuestMemory(addr_t address, size_t size) {
  check_range(address, size);
  std::lock_guard<TrackingLock> lock(memory_tracking().lock);
  invalidate_unlocked(address, size);
}

void recomp::Runtime::AddTraces(const Trace *traces, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (!traces[i].entry || imports_.count(traces[i].address) ||
        !traces_.emplace(traces[i].address, traces[i].entry).second)
      throw std::invalid_argument("duplicate or invalid guest trace");
  }
}
void recomp::Runtime::AddImport(addr_t address, std::function<void(State &)> bridge) {
  if (!bridge || traces_.count(address) || !imports_.emplace(address, std::move(bridge)).second)
    throw std::invalid_argument("duplicate or invalid guest import");
}
std::vector<State *> recomp::CurrentExecutionFrames(State &top) {
  if (!execution || execution->state != &top)
    throw std::invalid_argument("execution frames requested outside current target State");
  std::vector<State *> frames;
  for (ExecutionContext *frame = execution; frame; frame = frame->previous) frames.push_back(frame->state);
  return frames;
}
State *recomp::CurrentExecutionState() noexcept { return execution ? execution->state : nullptr; }
Memory *recomp::Runtime::Run(State &state, addr_t address, Memory *memory) {
  return RunInDomain(state,address,memory,nullptr);
}
Memory *recomp::Runtime::RunWithThreadExit(State &state,addr_t address,ThreadExitDomain &domain,Memory *memory) {
  return RunInDomain(state,address,memory,&domain);
}
bool recomp::Runtime::RequestThreadExit(State &state,ThreadExitDomain &domain,uint64_t value) {
  if (active!=this || !execution || execution->state!=&state || execution->terminal!=&domain)
    return false;
  if (domain.owner_!=std::this_thread::get_id() || domain.runtime_!=this || domain.requested_)
    throw std::logic_error("terminal thread exit has a foreign or pending domain");
  size_t count=0;
  for (ExecutionContext *frame=execution;frame;frame=frame->previous) {
    if (frame->terminal!=&domain || !frame->call_protocol_used || frame->transfer.active ||
        !frame->calls || frame->calls->root || frame->calls->expected_pc!=frame->state->gpr.x30.qword ||
        frame->calls->caller_sp!=frame->state->gpr.sp.qword)
      return false; // Never grant a capability across an ordinary/opaque Run.
    ++count;
  }
  if (!count || count!=domain.active_frames_)
    throw std::logic_error("terminal thread exit domain frame ownership differs");
  domain.value_=value;domain.requested_=true;return true;
}
uint64_t recomp::Runtime::ConsumeThreadExit(ThreadExitDomain &domain) {
  if (execution || domain.active_frames_ || !domain.requested_ || domain.owner_!=std::this_thread::get_id() || domain.runtime_!=this)
    throw std::logic_error("terminal thread exit consumed before actual Runtime retirement");
  const auto value=domain.value_;domain.value_=0;domain.memory_=nullptr;domain.requested_=false;return value;
}
Memory *recomp::Runtime::RunInDomain(State &state,addr_t address,Memory *memory,ThreadExitDomain *terminal) {
  for (ExecutionContext *frame=execution;frame;frame=frame->previous)
    if (frame->terminal && frame->terminal->Pending())
      throw std::logic_error("new Runtime Run entered during terminal thread exit propagation");
  clear_current_reservation();
  if (terminal) {
    if (terminal->requested_ || (terminal->owner_!=std::thread::id{} && terminal->owner_!=std::this_thread::get_id()) ||
        (terminal->runtime_ && terminal->runtime_!=this) ||
        (terminal->active_frames_ && terminal->memory_!=memory))
      throw std::logic_error("terminal worker Run has a pending/foreign domain or different Memory token");
    if (!terminal->active_frames_) {terminal->owner_=std::this_thread::get_id();terminal->runtime_=this;terminal->memory_=memory;}
    ++terminal->active_frames_;
  }
  ExecutionContext context{};
  context.state=&state;
  context.previous=execution;
  context.terminal=terminal;
  if (terminal) context.terminal_pending=&terminal->requested_;
  GuestCallFrame root_call{nullptr,state.gpr.x30.qword,state.gpr.sp.qword,true};
  context.calls=&root_call;
  struct Scope {
    Runtime *previous;
    ExecutionContext *previous_execution;
    int denormal;
    recomp::fpenv::Saved environment;
    ThreadExitDomain *terminal;
    explicit Scope(ThreadExitDomain *t) : previous(active), previous_execution(execution), denormal(explicit_denormal), environment(recomp::fpenv::Save()), terminal(t) {}
    ~Scope() {
      clear_current_reservation();
      active = previous;
      execution = previous_execution;
      explicit_denormal = denormal;
      recomp::fpenv::Restore(environment);
      if (terminal) --terminal->active_frames_;
    }
  } scope(terminal);
  active = this;
  execution = &context;
  recomp::fpenv::Clear(FE_ALL_EXCEPT);
  explicit_denormal = 0;
  recomp::fpenv::SetRound(host_rounding(state.fpcr.rmode));
  auto *result=Dispatch(state,address,memory);
  if (context.transfer.active || context.call_protocol_used || (terminal && terminal->Pending()))
    result=finish_guest_call(context,root_call,state,result);
  return result;
}
bool recomp::Runtime::HasGuestTrace(addr_t address) const noexcept {
  return traces_.find(address)!=traces_.end();
}
namespace {
// Direct-mapped, lock-free caches in front of the trace and import maps, one
// per dispatching Runtime (the main trace set and the bridge Runtimes all
// dispatch; with one shared cache only the first claimant hit it, and the
// ~600k-node trace map was walked on most indirect calls).
// A slot holds a pointer to a std::map node, tagged in bit 0 for an import
// node. Nodes are never erased or moved while their Runtime lives, and a hit
// is validated by the node's own key, so a racing overwrite only misses.
// Slot arrays are sized by the trace count and are reused, never freed.
using TraceNode = const std::pair<const addr_t, recomp::Entry>;
using ImportNode = const std::pair<const addr_t, std::function<void(State &)>>;
struct DispatchCache {
  std::atomic<const recomp::Runtime *> owner{nullptr};
  std::atomic<std::atomic<uintptr_t> *> slots{nullptr};
  unsigned bits = 0;  // written before slots is published
};
constexpr size_t kDispatchCaches = 16;
DispatchCache dispatch_caches[kDispatchCaches];
std::mutex dispatch_cache_claims;
inline size_t dispatch_slot(addr_t address, unsigned bits) {
  return size_t((address >> 2) * UINT64_C(0x9E3779B97F4A7C15) >> (64 - bits));
}
// The calling Runtime's cache, claimed on first use; null when all are taken.
DispatchCache *dispatch_cache_for(const recomp::Runtime *runtime, size_t traces) {
  for (auto &cache : dispatch_caches)
    if (cache.owner.load(std::memory_order_acquire) == runtime) return &cache;
  std::lock_guard lock(dispatch_cache_claims);
  for (auto &cache : dispatch_caches)
    if (cache.owner.load(std::memory_order_relaxed) == runtime) return &cache;
  for (auto &cache : dispatch_caches) {
    if (cache.owner.load(std::memory_order_relaxed)) continue;
    // 2^20 slots (8 MB) for the main trace set: a 3D frame dispatches to tens
    // of thousands of distinct targets. Small bridge sets get 2^12.
    const unsigned bits = traces >= 65536 ? 20 : 12;
    if (!cache.slots.load(std::memory_order_relaxed) || cache.bits < bits) {
      auto *slots = new std::atomic<uintptr_t>[size_t(1) << bits];
      for (size_t i = 0; i < (size_t(1) << bits); ++i) slots[i].store(0, std::memory_order_relaxed);
      cache.bits = bits;  // an older, smaller array is leaked, never freed
      cache.slots.store(slots, std::memory_order_release);
    }
    cache.owner.store(runtime, std::memory_order_release);
    return &cache;
  }
  return nullptr;
}
}  // namespace
void release_dispatch_cache(const recomp::Runtime *runtime) noexcept {
  std::lock_guard lock(dispatch_cache_claims);
  for (auto &cache : dispatch_caches) {
    if (cache.owner.load(std::memory_order_relaxed) != runtime) continue;
    auto *slots = cache.slots.load(std::memory_order_relaxed);
    for (size_t i = 0; slots && i < (size_t(1) << cache.bits); ++i) slots[i].store(0, std::memory_order_relaxed);
    cache.owner.store(nullptr, std::memory_order_release);
  }
}
recomp::Runtime::Runtime(const Runtime &other) : traces_(other.traces_), imports_(other.imports_) {}
recomp::Runtime::Runtime(Runtime &&other) noexcept : traces_(std::move(other.traces_)), imports_(std::move(other.imports_)) {
  release_dispatch_cache(&other);
}
recomp::Runtime &recomp::Runtime::operator=(const Runtime &other) {
  if (this != &other) { release_dispatch_cache(this); traces_ = other.traces_; imports_ = other.imports_; }
  return *this;
}
recomp::Runtime &recomp::Runtime::operator=(Runtime &&other) noexcept {
  if (this != &other) {
    release_dispatch_cache(this); release_dispatch_cache(&other);
    traces_ = std::move(other.traces_); imports_ = std::move(other.imports_);
  }
  return *this;
}
recomp::Runtime::~Runtime() { release_dispatch_cache(this); }
namespace {
// Host replacements for guest function entries (see recomp::AddGuestOverride).
// Registered before guest execution starts and never removed, so lookups need
// no lock once the flag is published.
std::atomic<bool> guest_overrides_active{false};
// A probe observes the entry and then runs the original trace; an override
// replaces it; a redirect edits State and continues at the guest address it
// returns (a guest tail call, the caller's LR is kept).
// A direct entry substitutes another lifted body for the trace (both direct
// lifted calls and dispatch reach it, through the same guest-call protocol).
struct GuestHook { std::function<void(State &)> bridge; bool probe; std::function<addr_t(State &)> redirect; recomp::Entry direct = nullptr; };
std::unordered_map<addr_t, GuestHook> &guest_overrides() {
  static std::unordered_map<addr_t, GuestHook> map;
  return map;
}
std::mutex guest_override_registration;
// Every lifted call checks for an override, so a 64Ki-bit (8 KiB) filter keyed
// by the instruction index keeps the common miss to two loads; it is sized so
// a few hundred probes/overrides rarely alias a hot function into the map lookup.
constexpr size_t kOverrideFilterBits = 65536;
}  // namespace
// Read inline by lifted direct-call sites (poll_instrument --direct-calls).
extern "C" uint64_t __bunker_guest_override_filter[kOverrideFilterBits / 64];
uint64_t __bunker_guest_override_filter[kOverrideFilterBits / 64];
namespace {
uint64_t (&guest_override_filter)[kOverrideFilterBits / 64] = __bunker_guest_override_filter;
inline size_t override_filter_bit(addr_t address) { return size_t(address >> 2) & (kOverrideFilterBits - 1); }
// The filter test is inline at every call site; the map lookup is not.
__attribute__((noinline)) const GuestHook *find_guest_override_slow(addr_t address) {
  auto &map = guest_overrides();
  auto found = map.find(address);
  return found == map.end() ? nullptr : &found->second;
}
inline const GuestHook *find_guest_override(addr_t address) {
  if (!guest_overrides_active.load(std::memory_order_acquire)) return nullptr;
  const auto bit = override_filter_bit(address);
  if (!(guest_override_filter[bit / 64] >> (bit % 64) & 1)) return nullptr;
  return find_guest_override_slow(address);
}
Memory *invoke_host_bridge(State &state, const std::function<void(State &)> &bridge, Memory *memory) {
  clear_current_reservation();
  struct ImportReservationScope {
    ~ImportReservationScope() { clear_current_reservation(); }
  } import_reservation_scope;
  // Guest instructions have already accumulated their flags in State. A
  // guest MSR FPSR may have cleared those flags since the last host FP op.
  // Attribute only exceptions from this import to the guest call.
  recomp::fpenv::Clear(FE_ALL_EXCEPT);
  explicit_denormal = 0;
  bridge(state);
  const int mask = guest_exceptions(recomp::fpenv::Test(FE_ALL_EXCEPT)) | explicit_denormal;
  state.sr.ioc |= (mask & kFPUExceptionInvalid) != 0;
  state.sr.dzc |= (mask & kFPUExceptionDivByZero) != 0;
  state.sr.ofc |= (mask & kFPUExceptionOverflow) != 0;
  state.sr.ufc |= (mask & kFPUExceptionUnderflow) != 0;
  state.sr.ixc |= (mask & kFPUExceptionPrecision) != 0;
  state.sr.idc |= (mask & kFPUExceptionDenormal) != 0;
  state.gpr.pc.qword = state.gpr.x30.qword;
  return memory;
}
}  // namespace
static void add_guest_hook(addr_t address, std::function<void(State &)> bridge, bool probe,
                           std::function<addr_t(State &)> redirect = {}, recomp::Entry direct = nullptr) {
  if (!bridge && !redirect && !direct) fail("guest override requires a bridge", address);
  std::lock_guard lock(guest_override_registration);
  if (guest_overrides_active.load(std::memory_order_relaxed))
    fail("guest overrides must be registered before guest execution", address);
  if (!guest_overrides().emplace(address, GuestHook{std::move(bridge), probe, std::move(redirect), direct}).second) fail("duplicate guest override", address);
  const auto bit = override_filter_bit(address);
  guest_override_filter[bit / 64] |= uint64_t(1) << (bit % 64);
}
void recomp::AddGuestOverride(addr_t address, std::function<void(State &)> bridge) {
  add_guest_hook(address, std::move(bridge), false);
}
void recomp::AddGuestProbe(addr_t address, std::function<void(State &)> observer) {
  add_guest_hook(address, std::move(observer), true);
}
void recomp::AddGuestRedirect(addr_t address, std::function<addr_t(State &)> redirect) {
  if (!redirect) fail("guest redirect requires a callback", address);
  add_guest_hook(address, {}, false, std::move(redirect));
}
void recomp::AddGuestDirectEntry(addr_t address, Entry entry) {
  if (!entry) fail("guest direct entry requires an entry", address);
  add_guest_hook(address, {}, false, {}, entry);
}
void recomp::PublishGuestOverrides() {
  std::lock_guard lock(guest_override_registration);
  if (!guest_overrides().empty()) guest_overrides_active.store(true, std::memory_order_release);
}
Memory *recomp::Runtime::Dispatch(State &state, addr_t address, Memory *memory) {
  if (auto *hook = find_guest_override(address)) {
    if (hook->direct) return hook->direct(state, address, memory);
    if (hook->redirect) {
      const addr_t target = hook->redirect(state);
      if (target == address) fail("guest redirect to itself", address);
      state.gpr.pc.qword = target;
      return Dispatch(state, target, memory);
    }
    if (!hook->probe) return invoke_host_bridge(state, hook->bridge, memory);
    hook->bridge(state);
    if (auto found = traces_.find(address); found != traces_.end()) return found->second(state, address, memory);
  }
  auto *cache = dispatch_cache_for(this, traces_.size());
  std::atomic<uintptr_t> *slot = nullptr;
  if (cache) {
    auto *slots = cache->slots.load(std::memory_order_acquire);
    slot = &slots[dispatch_slot(address, cache->bits)];
    if (const uintptr_t hit = slot->load(std::memory_order_acquire)) {
      if (hit & 1) {
        auto *node = reinterpret_cast<ImportNode *>(hit & ~uintptr_t(1));
        if (node->first == address) return invoke_host_bridge(state, node->second, memory);
      } else if (auto *node = reinterpret_cast<TraceNode *>(hit); node->first == address) {
        return node->second(state, address, memory);
      }
    }
  }
  if (auto found = traces_.find(address); found != traces_.end()) {
    if (slot) slot->store(reinterpret_cast<uintptr_t>(&*found), std::memory_order_release);
    return found->second(state, address, memory);
  }
  if (auto found = imports_.find(address); found != imports_.end()) {
    if (slot) slot->store(reinterpret_cast<uintptr_t>(&*found) | 1, std::memory_order_release);
    return invoke_host_bridge(state, found->second, memory);
  }
  std::fprintf(stderr, "recomp: unresolved indirect context target=%#llx StatePC=%#llx LR=%#llx SP=%#llx\n", static_cast<unsigned long long>(address), static_cast<unsigned long long>(state.gpr.pc.qword), static_cast<unsigned long long>(state.gpr.x30.qword), static_cast<unsigned long long>(state.gpr.sp.qword));
  fail("unresolved indirect target", address);
}

extern "C" Memory *__bunker_guest_call(State &state,addr_t target,Memory *memory,
                                      addr_t expected_pc,addr_t caller_sp,
                                      recomp::Entry direct_entry) noexcept {
  // Each ThreadLocalRef access is a guard check plus a TEB load; read once.
  auto *const runtime=active.get();
  auto *const current=execution.get();
  if (!runtime || !current || current->state!=&state)
    fail("guest call outside its active Run/State",target);
  auto &context=*current;
  if (context.transfer.active || state.gpr.sp.qword!=caller_sp)
    fail("guest call entered with pending transfer or wrong captured SP",target);
  context.call_protocol_used=true;
  GuestCallFrame frame{context.calls,expected_pc,caller_sp,false};
  context.calls=&frame;
  struct Unlink {
    ExecutionContext &context;GuestCallFrame *previous;
    ~Unlink(){context.calls=previous;}
  } unlink{context,frame.previous};
  try {
    // A direct call to an overridden entry must reach the host replacement;
    // a direct-entry substitution replaces the lifted body directly.
    if(direct_entry)if(auto *hook=find_guest_override(target))direct_entry=hook->direct;
    auto *result=direct_entry?direct_entry(state,target,memory):runtime->Dispatch(state,target,memory);
    // Ordinary return to this frame: exactly finish_guest_call's first exit.
    if (!context.terminal && !context.transfer.active && returns_to(state,frame)) return result;
    return finish_guest_call(context,frame,state,result);
  } catch (const std::exception &error) {
    std::fprintf(stderr,"recomp: native exception at explicit guest-call boundary: %s\n",error.what());
    fail("native exception cannot cross lifted nounwind call",target);
  } catch (...) {
    fail("unknown native exception cannot cross lifted nounwind call",target);
  }
}
// Slow half of an inline direct call (poll_instrument --direct-calls): the
// site linked `frame` around a direct call to the lifted callee, unlinked it,
// and saw a pending transfer or a return elsewhere. Relink the same frame and
// finish exactly as __bunker_guest_call would have.
extern "C" Memory *__bunker_guest_call_finish(State &state,Memory *memory,void *raw_frame) noexcept {
  auto *const current=execution.get();
  if (!active.get() || !current || current->state!=&state)
    fail("inline guest call finished outside its active Run/State",state.gpr.pc.qword);
  auto &frame=*static_cast<GuestCallFrame *>(raw_frame);
  if (current->calls!=frame.previous || frame.root)
    fail("inline guest call frame is not the innermost call",frame.expected_pc);
  auto &context=*current;
  context.call_protocol_used=true;
  context.calls=&frame;
  struct Unlink {
    ExecutionContext &context;GuestCallFrame *previous;
    ~Unlink(){context.calls=previous;}
  } unlink{context,frame.previous};
  try {
    return finish_guest_call(context,frame,state,memory);
  } catch (...) {
    fail("native exception cannot cross lifted nounwind call",frame.expected_pc);
  }
}
#ifdef _WIN32
// TEB byte offset of this thread's ExecutionContext pointer for inline call
// sites; 0 keeps every site on __bunker_guest_call.
extern "C" uint64_t __bunker_exec_tls_offset;
uint64_t __bunker_exec_tls_offset = [] {
  const auto index = execution.slot().index();
  return index < 64 ? uint64_t(0x1480 + index * 8) : uint64_t(0);
}();
#endif
extern "C" uint32_t __bunker_guest_transfer_pending(State &state) noexcept {
  auto *const current=execution.get();
  if (!current || current->state!=&state)
    fail("guest transfer query outside its active Run/State",state.gpr.pc.qword);
  return current->transfer.active || (current->terminal && current->terminal->Pending())?1U:0U;
}

extern "C" {
uint64_t __remill_aarch64_load_exclusive(State &state, addr_t address, uint64_t size, uint64_t) {
  auto &context = exclusive_context(state, address, size);
  std::lock_guard<TrackingLock> lock(memory_tracking().lock);
  clear_reservation_unlocked(context.reservation);
  const auto epoch = publish_granule_unlocked(address);
  uint64_t value = 0;
  switch (size) {
    case 1: value = exclusive_load_unlocked<uint8_t>(address); break;
    case 2: value = exclusive_load_unlocked<uint16_t>(address); break;
    case 4: value = exclusive_load_unlocked<uint32_t>(address); break;
    case 8: value = exclusive_load_unlocked<uint64_t>(address); break;
  }
  reserve_unlocked(context, epoch, address, size, value, false);
  return value;
}
uint32_t __remill_aarch64_store_exclusive(State &state, addr_t address, uint64_t value, uint64_t size, uint64_t) {
  auto &context = exclusive_context(state, address, size);
  std::lock_guard<TrackingLock> lock(memory_tracking().lock);
  const auto &reservation = context.reservation;
  bool success = !reservation.pair && reservation.address == address && reservation.size == size &&
                 reservation_current_unlocked(reservation);
  if (success) {
    switch (size) {
      case 1: success = exclusive_store_unlocked<uint8_t>(address, static_cast<uint64_t>(reservation.value), value); break;
      case 2: success = exclusive_store_unlocked<uint16_t>(address, static_cast<uint64_t>(reservation.value), value); break;
      case 4: success = exclusive_store_unlocked<uint32_t>(address, static_cast<uint64_t>(reservation.value), value); break;
      case 8: success = exclusive_store_unlocked<uint64_t>(address, static_cast<uint64_t>(reservation.value), value); break;
    }
    if (success) invalidate_unlocked(address, size);
  }
  clear_reservation_unlocked(context.reservation);
  return success ? 0 : 1;
}
void __remill_aarch64_clear_exclusive(State &state) {
  if (!execution || execution->state != &state) fail("clear exclusive outside active execution", state.gpr.pc.qword);
  clear_current_reservation();
}
void __remill_aarch64_load_exclusive_pair(State &state, addr_t address, uint64_t element_size, uint64_t,
                                         uint64_t *first, uint64_t *second) {
  if (element_size != 4 && element_size != 8) fail("unsupported exclusive pair element width", address);
  if (!first || !second) fail("invalid exclusive pair output", address);
  auto &context = exclusive_context(state, address, element_size * 2, true);
  std::lock_guard<TrackingLock> lock(memory_tracking().lock);
  clear_reservation_unlocked(context.reservation);
  const auto epoch = publish_granule_unlocked(address);
  uint128_t value = 0;
  if (element_size == 4) value = exclusive_load_unlocked<uint64_t>(address);
  else __atomic_load(reinterpret_cast<const uint128_t *>(address), &value, __ATOMIC_SEQ_CST);
  reserve_unlocked(context, epoch, address, element_size * 2, value, true);
  const unsigned bits = element_size * 8;
  const uint64_t mask = element_size == 4 ? UINT64_C(0xffffffff) : ~UINT64_C(0);
  *first = static_cast<uint64_t>(value) & mask;
  *second = static_cast<uint64_t>(value >> bits) & mask;
}
uint32_t __remill_aarch64_store_exclusive_pair(State &state, addr_t address, uint64_t first, uint64_t second,
                                              uint64_t element_size, uint64_t) {
  if (element_size != 4 && element_size != 8) fail("unsupported exclusive pair element width", address);
  auto &context = exclusive_context(state, address, element_size * 2, true);
  std::lock_guard<TrackingLock> lock(memory_tracking().lock);
  const auto &reservation = context.reservation;
  bool success = reservation.pair && reservation.address == address &&
                 reservation.size == element_size * 2 && reservation_current_unlocked(reservation);
  if (success) {
    if (element_size == 4) {
      const uint64_t desired = uint64_t(static_cast<uint32_t>(first)) | (uint64_t(static_cast<uint32_t>(second)) << 32);
      success = exclusive_store_unlocked<uint64_t>(address, static_cast<uint64_t>(reservation.value), desired);
    } else {
      uint128_t expected = reservation.value;
      uint128_t desired = uint128_t(first) | (uint128_t(second) << 64);
      success = __atomic_compare_exchange(reinterpret_cast<uint128_t *>(address), &expected, &desired,
                                          false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    if (success) invalidate_unlocked(address, element_size * 2);
  }
  clear_reservation_unlocked(context.reservation);
  return success ? 0 : 1;
}
// Opt-in for programs whose guest code never reads or writes FPSR (no
// MRS/MSR FPSR anywhere in translated text). Remill only ORs per-operation
// exception results into the sticky State.sr bits and never derives a value
// from them, so the per-operation MXCSR clear is skipped: the test then
// reports the accumulated host flags, a superset that nothing observes.
// Import bridges still attribute exceptions exactly (they clear around the
// host call). Set once before any guest code runs.
extern "C" void __bunker_set_fpsr_unobserved(bool value) noexcept { fpsr_unobserved = value; }
int32_t __remill_fpu_exception_test(int32_t mask) {
  return (guest_exceptions(recomp::fpenv::Test(FE_ALL_EXCEPT)) | explicit_denormal) & mask;
}
void __remill_fpu_exception_clear(int32_t mask) {
  // Called around nearly every lifted FP instruction. LDMXCSR is costly and
  // the result only feeds sticky FPSR bits, so skip it when FPSR is unobserved.
  if (fpsr_unobserved) return;
  // Write the thread-local only when the explicit denormal flag changes.
  recomp::fpenv::Clear(host_exceptions(mask));
  const int denormal = explicit_denormal;
  if (denormal & mask) explicit_denormal = denormal & ~mask;
}
void __remill_fpu_exception_raise(int32_t mask) {
  recomp::fpenv::Raise(host_exceptions(mask));
  explicit_denormal |= mask & kFPUExceptionDenormal;
}
void __remill_fpu_set_rounding(int32_t mode) { recomp::fpenv::SetRound(host_rounding(mode)); }
int32_t __remill_fpu_get_rounding() {
  switch (recomp::fpenv::GetRound()) {
    case FE_TONEAREST: return kFPURoundToNearestEven;
    case FE_UPWARD: return kFPURoundUpInf;
    case FE_DOWNWARD: return kFPURoundDownNegInf;
    case FE_TOWARDZERO: return kFPURoundToZero;
    default: fail("invalid host floating-point rounding mode", 0);
  }
}
#define MEMORY(bits, type) \
type __remill_read_memory_##bits(Memory *, addr_t a) { return load<type>(a); } \
Memory *__remill_write_memory_##bits(Memory *m, addr_t a, type v) { return store(m, a, v); }
MEMORY(8, uint8_t)
MEMORY(16, uint16_t)
MEMORY(32, uint32_t)
MEMORY(64, uint64_t)
MEMORY(f32, float32_t)
MEMORY(f64, float64_t)
#undef MEMORY
Memory *__remill_read_memory_f80(Memory *m, addr_t a, float80_t &value) { value = load<float80_t>(a); return m; }
float128_t __remill_read_memory_f128(Memory *, addr_t a) { return load<float128_t>(a); }
Memory *__remill_write_memory_f80(Memory *m, addr_t a, const float80_t &v) { return store(m, a, v); }
Memory *__remill_write_memory_f128(Memory *m, addr_t a, float128_t v) { return store(m, a, v); }

uint8_t __remill_undefined_8() { return 0; }
uint16_t __remill_undefined_16() { return 0; }
uint32_t __remill_undefined_32() { return 0; }
uint64_t __remill_undefined_64() { return 0; }
float32_t __remill_undefined_f32() { return 0; }
float64_t __remill_undefined_f64() { return 0; }
bool __remill_flag_computation_zero(bool r, ...) { return r; }
bool __remill_flag_computation_sign(bool r, ...) { return r; }
bool __remill_flag_computation_overflow(bool r, ...) { return r; }
bool __remill_flag_computation_carry(bool r, ...) { return r; }
#define COMPARE(name) bool __remill_compare_##name(bool r) { return r; }
COMPARE(eq) COMPARE(neq) COMPARE(sle) COMPARE(slt) COMPARE(sge) COMPARE(sgt)
COMPARE(ule) COMPARE(ult) COMPARE(uge) COMPARE(ugt)
#undef COMPARE

// Lifted error blocks report the instruction after the unsupported one. Known
// encodings are emulated on State and execution resumes at the next
// instruction, which is lifted as its own trace head.
Memory *__remill_error(State &s, addr_t pc, Memory *m) {
  uint32_t word = 0;
  std::memcpy(&word, reinterpret_cast<const void *>(pc - 4), sizeof(word));
  if (recomp::aarch64_fallback::Emulate(s, word)) {
    s.gpr.pc.qword = pc;
    return dispatch(s, pc, m);
  }
  std::fprintf(stderr, "recomp: unsupported instruction %08x at %#llx\n", word, static_cast<unsigned long long>(pc - 4));
  fail("unliftable instruction", pc);
}
Memory *__remill_missing_block(State &, addr_t pc, Memory *) { fail("missing block", pc); }
Memory *__remill_function_return(State &, addr_t, Memory *m) { return m; }
Memory *__remill_function_call(State &s, addr_t pc, Memory *m) { return dispatch(s, pc, m); }
Memory *__remill_jump(State &s, addr_t pc, Memory *m) { return dispatch(s, pc, m); }
Memory *__remill_async_hyper_call(State &, addr_t pc, Memory *) { fail("async hyper call", pc); }
Memory *__remill_sync_hyper_call(State &s, Memory *, SyncHyperCall::Name) { fail("sync hyper call", s.gpr.pc.qword); }

Memory *__remill_barrier_load_load(Memory *m) { std::atomic_thread_fence(std::memory_order_seq_cst); return m; }
Memory *__remill_barrier_load_store(Memory *m) { std::atomic_thread_fence(std::memory_order_seq_cst); return m; }
Memory *__remill_barrier_store_load(Memory *m) { std::atomic_thread_fence(std::memory_order_seq_cst); return m; }
Memory *__remill_barrier_store_store(Memory *m) { std::atomic_thread_fence(std::memory_order_seq_cst); return m; }
Memory *__remill_atomic_begin(Memory *m) { return m; }
Memory *__remill_atomic_end(Memory *m) { return m; }
Memory *__remill_delay_slot_begin(Memory *m) { return m; }
Memory *__remill_delay_slot_end(Memory *m) { return m; }

#define CMPXCHG(bits) \
Memory *__remill_compare_exchange_memory_##bits(Memory *m, addr_t a, uint##bits##_t &expected, uint##bits##_t desired) { \
  std::lock_guard<TrackingLock> lock(memory_tracking().lock); \
  if (__atomic_compare_exchange_n(reinterpret_cast<uint##bits##_t *>(a), &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) \
    invalidate_unlocked(a, sizeof(desired)); \
  return m; }
CMPXCHG(8) CMPXCHG(16) CMPXCHG(32) CMPXCHG(64)
#undef CMPXCHG
Memory *__remill_compare_exchange_memory_128(Memory *m, addr_t a, uint128_t &expected, uint128_t &desired) {
  std::lock_guard<TrackingLock> lock(memory_tracking().lock);
  if (__atomic_compare_exchange(reinterpret_cast<uint128_t *>(a), &expected, &desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    invalidate_unlocked(a, sizeof(desired));
  return m;
}
#define FETCH(op, bits) \
Memory *__remill_fetch_and_##op##_##bits(Memory *m, addr_t a, uint##bits##_t &value) { \
  std::lock_guard<TrackingLock> lock(memory_tracking().lock); \
  value = __atomic_fetch_##op(reinterpret_cast<uint##bits##_t *>(a), value, __ATOMIC_SEQ_CST); \
  invalidate_unlocked(a, sizeof(value)); return m; }
#define FETCH_ALL(op) FETCH(op, 8) FETCH(op, 16) FETCH(op, 32) FETCH(op, 64)
FETCH_ALL(add) FETCH_ALL(sub) FETCH_ALL(and) FETCH_ALL(or) FETCH_ALL(xor) FETCH_ALL(nand)
#undef FETCH_ALL
#undef FETCH
}
