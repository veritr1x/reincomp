#pragma once
// Grpc.Core 2.26.0 native extension as a typed host service.
//
// The game's managed Grpc.Core P/Invokes into grpc_csharp_ext. Instead of
// translating the Android ARM64 library, this loads the official host build
// from the same NuGet package (runtimes/win/native/grpc_csharp_ext.x64.dll or
// runtimes/linux/native/libgrpc_csharp_ext.x64.so) and forwards each guest
// call. Guest memory is identity-mapped, so pointers pass through unchanged.
//
// ABI: AArch64 passes and returns aggregates of 16 bytes or less in register
// pairs, and larger returns through X8. Every export is called through its
// typed C prototype, so the host compiler applies the Win64 or SysV rules for
// gpr_timespec / grpc_event / grpc_auth_property_iterator. All other
// parameters are integers or pointers and are forwarded as 64-bit values;
// both host ABIs leave unused high bits of narrower integers unspecified.
//
// Native gRPC threads never call guest code: the log redirect and callback
// dispatcher registrations are kept host-side, and exports that would make
// native code invoke the dispatcher (credential plugins, peer verification
// callbacks) fail explicitly.
//
// Blocking: grpcsharp_completion_queue_next waits on a host helper per queue
// and returns GRPC_QUEUE_TIMEOUT after a short slice. Grpc.Core's handler
// loop ignores timeouts, so guest threads keep returning to translated code.
// Optional wait hooks run each slice inside a collector wait scope with
// Copied/BeforeNativeWait/AfterNativeWait/Returned observations, so a stop
// request can be delivered to a thread parked in the slice.
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <cerrno>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include "../runtime/fast_fenv.h"
#include "../runtime/guest_memory.h"
#include "../runtime/runtime.h"
#include "module_registry.h"

namespace bunker::windows::grpc_native {

struct Timespec {
  int64_t tv_sec;
  int32_t tv_nsec;
  int32_t clock_type;
};
struct Event {
  int32_t type;  // 0 shutdown, 1 timeout, 2 op complete
  int32_t success;
  void *tag;
};
struct AuthPropertyIterator {
  const void *ctx;
  size_t index;
  const char *name;
};
static_assert(sizeof(Timespec) == 16 && sizeof(Event) == 16 && sizeof(AuthPropertyIterator) == 24);

// Host state around foreign calls: FP control/flags, errno, Win32 last error.
struct NativeScope {
  recomp::fpenv::Saved fp = recomp::fpenv::Save();
  int saved_errno = errno;
#ifdef _WIN32
  DWORD last_error = GetLastError();
#endif
  ~NativeScope() {
    recomp::fpenv::Restore(fp);
    errno = saved_errno;
#ifdef _WIN32
    SetLastError(last_error);
#endif
  }
};

class Service {
 public:
  struct Config {
    std::string library_path;  // absolute host path to the native extension
    // Channel targets are rewritten by exact match, e.g. "127.0.0.1:443" ->
    // "127.0.0.1:38003" when the backend's gRPC port differs from the
    // client's serialized 443.
    std::map<std::string, std::string> target_rewrites;
    bool trace = false;
  };
  static constexpr addr_t kMarkerBase = 0x7fed00000000ULL;

  explicit Service(Config config) : config_(std::move(config)) {
#ifdef _WIN32
    const auto wide = std::filesystem::u8path(config_.library_path).wstring();
    library_ = LoadLibraryExW(wide.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
#else
    library_ = dlopen(config_.library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    if (!library_) throw std::runtime_error("gRPC native extension failed to load: " + config_.library_path);
    for (size_t i = 0; i < Names().size(); ++i) {
      void *symbol = Resolve(Names()[i]);
      if (!symbol) throw std::runtime_error(std::string("gRPC native extension lacks export ") + Names()[i]);
      functions_.push_back(symbol);
    }
    NativeScope scope;
    auto version = reinterpret_cast<const char *(*)()>(Function("grpcsharp_version_string"))();
    std::fprintf(stderr, "gRPC native: loaded %s (core %s); %zu exports\n", config_.library_path.c_str(), version ? version : "?",
                 functions_.size());
  }
  Service(const Service &) = delete;
  Service &operator=(const Service &) = delete;
  // The library stays loaded for process lifetime: native threads may outlive us.
  ~Service() {
    std::lock_guard lock(pumps_mutex_);
    for (auto &[cq, pump] : pumps_)
      if (pump->thread.joinable()) pump->thread.detach();
  }

  static const std::vector<const char *> &Names() {
    static const std::vector<const char *> names = {
#define GRPC_NATIVE_EXPORT(n) #n,
#include "grpc_native_exports.inc"
#undef GRPC_NATIVE_EXPORT
    };
    return names;
  }
  addr_t Marker(size_t index) const { return kMarkerBase + index * 8; }

  Module MakeModule() const {
    Module module;
    module.name = "libgrpc_csharp_ext.so";
    module.aliases = {"grpc_csharp_ext", "grpc_csharp_ext.so", "libgrpc_csharp_ext", "libgrpc_csharp_ext.x64.so",
                      "grpc_csharp_ext.x64.dll", "grpc_csharp_ext.dll", "/data/app/bunker/lib/arm64/libgrpc_csharp_ext.so"};
    module.kind = Module::Kind::TypedService;
    for (size_t i = 0; i < Names().size(); ++i) module.exports.emplace(Names()[i], Marker(i));
    module.reset_image = [] {};
    return module;
  }

  // Collector integration for the blocking completion-queue import.
  enum class WaitPhase { Copied, BeforeNativeWait, AfterNativeWait, Returned };
  struct WaitObservation {
    State *state;
    WaitPhase phase;
    const void *identity;
    std::shared_ptr<const void> retained;
  };
  using WaitScope = std::function<void(const char *name, State &, const std::function<void()> &)>;
  using WaitObserver = std::function<void(const WaitObservation &)>;
  static constexpr const char *kQueueNext = "grpcsharp_completion_queue_next";
  void SetWaitHooks(WaitScope scope, WaitObserver observer) {
    wait_scope_ = std::move(scope);
    wait_observer_ = std::move(observer);
  }

  void Install(recomp::Runtime &runtime) {
    for (size_t i = 0; i < Names().size(); ++i)
      runtime.AddImport(Marker(i), [this, i](State &s) {
        try {
          Invoke(i, s);
        } catch (const std::exception &error) {
          std::fprintf(stderr, "gRPC native: %s failed: %s\n", Names()[i], error.what());
          std::fflush(stderr);
          std::abort();
        }
      });
  }

 private:
  struct Pump {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Event> events;
  };

  void *Resolve(const char *name) {
#ifdef _WIN32
    return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(library_), name));
#else
    return dlsym(library_, name);
#endif
  }
  void *Function(const char *name) {
    for (size_t i = 0; i < Names().size(); ++i)
      if (!std::strcmp(Names()[i], name)) return functions_[i];
    throw std::logic_error(std::string("unknown gRPC export ") + name);
  }
  static uint64_t X(const State &s, unsigned n) { return XImpl(s, n); }
  static Timespec TimespecArg(const State &s, unsigned first) {
    const uint64_t low = X(s, first), high = X(s, first + 1);
    Timespec t;
    t.tv_sec = int64_t(low);
    t.tv_nsec = int32_t(uint32_t(high));
    t.clock_type = int32_t(uint32_t(high >> 32));
    return t;
  }
  static void ReturnTimespec(State &s, const Timespec &t) {
    s.gpr.x0.qword = uint64_t(t.tv_sec);
    s.gpr.x1.qword = uint64_t(uint32_t(t.tv_nsec)) | uint64_t(uint32_t(t.clock_type)) << 32;
  }
  static void ReturnEvent(State &s, const Event &e) {
    s.gpr.x0.qword = uint64_t(uint32_t(e.type)) | uint64_t(uint32_t(e.success)) << 32;
    s.gpr.x1.qword = reinterpret_cast<uint64_t>(e.tag);
  }
  static uint64_t XImpl(const State &s, unsigned n) {
    switch (n) {
      case 0: return s.gpr.x0.qword; case 1: return s.gpr.x1.qword; case 2: return s.gpr.x2.qword;
      case 3: return s.gpr.x3.qword; case 4: return s.gpr.x4.qword; case 5: return s.gpr.x5.qword;
      case 6: return s.gpr.x6.qword; case 7: return s.gpr.x7.qword;
    }
    throw std::logic_error("AArch64 integer argument register out of range");
  }

  Event Next(State &s, void *cq) {
    Event result{};
    const auto slice = [&] {
      const auto request = std::make_shared<int>(0);
      const auto observe = [&](WaitPhase phase) {
        if (wait_observer_) wait_observer_(WaitObservation{&s, phase, request.get(), request});
      };
      observe(WaitPhase::Copied);
      observe(WaitPhase::BeforeNativeWait);
      result = NextSlice(cq);
      observe(WaitPhase::AfterNativeWait);
      observe(WaitPhase::Returned);
    };
    if (wait_scope_)
      wait_scope_(kQueueNext, s, slice);
    else
      slice();
    return result;
  }

  Event NextSlice(void *cq) {
    std::shared_ptr<Pump> pump;
    {
      std::lock_guard lock(pumps_mutex_);
      auto &slot = pumps_[cq];
      if (!slot) {
        slot = std::make_shared<Pump>();
        auto next = reinterpret_cast<Event (*)(void *)>(Function("grpcsharp_completion_queue_next"));
        slot->thread = std::thread([pump = slot, next, cq] {
          for (;;) {
            Event e;
            {
              NativeScope scope;
              e = next(cq);
            }
            {
              std::lock_guard lock(pump->mutex);
              pump->events.push_back(e);
            }
            pump->ready.notify_all();
            if (e.type == 0) return;  // shutdown: the queue delivers nothing further
          }
        });
      }
      pump = slot;
    }
    std::unique_lock lock(pump->mutex);
    if (!pump->ready.wait_for(lock, std::chrono::milliseconds(20), [&] { return !pump->events.empty(); }))
      return Event{1, 0, nullptr};
    Event e = pump->events.front();
    pump->events.pop_front();
    lock.unlock();
    if (e.type == 0) {
      std::lock_guard registry(pumps_mutex_);
      if (pump->thread.joinable()) pump->thread.join();
      pumps_.erase(cq);
    }
    return e;
  }

  void Invoke(size_t index, State &s) {
    const std::string name = Names()[index];
    void *f = functions_[index];
    if (config_.trace) std::fprintf(stderr, "gRPC native: %s\n", name.c_str());
    if (name == "grpcsharp_completion_queue_next") {
      ReturnEvent(s, Next(s, reinterpret_cast<void *>(X(s, 0))));
      return;
    }
    NativeScope scope;
    if (name == "grpcsharp_completion_queue_pluck") {
      ReturnEvent(s, reinterpret_cast<Event (*)(void *, void *)>(f)(reinterpret_cast<void *>(X(s, 0)), reinterpret_cast<void *>(X(s, 1))));
    } else if (name == "grpcsharp_request_call_context_deadline") {
      ReturnTimespec(s, reinterpret_cast<Timespec (*)(void *)>(f)(reinterpret_cast<void *>(X(s, 0))));
    } else if (name == "gprsharp_now" || name == "gprsharp_inf_future" || name == "gprsharp_inf_past") {
      ReturnTimespec(s, reinterpret_cast<Timespec (*)(int32_t)>(f)(int32_t(X(s, 0))));
    } else if (name == "gprsharp_convert_clock_type") {
      ReturnTimespec(s, reinterpret_cast<Timespec (*)(Timespec, int32_t)>(f)(TimespecArg(s, 0), int32_t(X(s, 2))));
    } else if (name == "grpcsharp_channel_create_call") {
      using F = void *(*)(void *, void *, uint32_t, void *, const char *, const char *, Timespec);
      s.gpr.x0.qword = reinterpret_cast<uint64_t>(reinterpret_cast<F>(f)(
          reinterpret_cast<void *>(X(s, 0)), reinterpret_cast<void *>(X(s, 1)), uint32_t(X(s, 2)), reinterpret_cast<void *>(X(s, 3)),
          reinterpret_cast<const char *>(X(s, 4)), reinterpret_cast<const char *>(X(s, 5)), TimespecArg(s, 6)));
    } else if (name == "grpcsharp_channel_watch_connectivity_state") {
      using F = void (*)(void *, int32_t, Timespec, void *, void *);
      reinterpret_cast<F>(f)(reinterpret_cast<void *>(X(s, 0)), int32_t(X(s, 1)), TimespecArg(s, 2), reinterpret_cast<void *>(X(s, 4)),
                             reinterpret_cast<void *>(X(s, 5)));
    } else if (name == "grpcsharp_auth_context_property_iterator") {
      const auto result = reinterpret_cast<AuthPropertyIterator (*)(const void *)>(f)(reinterpret_cast<const void *>(X(s, 0)));
      recomp::WriteGuestMemory(s.gpr.x8.qword, &result, sizeof(result));
    } else if (name == "grpcsharp_insecure_channel_create" || name == "grpcsharp_secure_channel_create") {
      const bool secure = name == "grpcsharp_secure_channel_create";
      const unsigned target_arg = secure ? 1 : 0;
      std::string target = recomp::ReadGuestString(X(s, target_arg));
      if (auto found = config_.target_rewrites.find(target); found != config_.target_rewrites.end()) {
        std::fprintf(stderr, "gRPC native: channel target %s -> %s\n", target.c_str(), found->second.c_str());
        target = found->second;
      } else {
        std::fprintf(stderr, "gRPC native: channel target %s\n", target.c_str());
      }
      void *channel = secure
                          ? reinterpret_cast<void *(*)(void *, const char *, const void *)>(f)(reinterpret_cast<void *>(X(s, 0)), target.c_str(),
                                                                                            reinterpret_cast<const void *>(X(s, 2)))
                          : reinterpret_cast<void *(*)(const char *, const void *)>(f)(target.c_str(), reinterpret_cast<const void *>(X(s, 1)));
      s.gpr.x0.qword = reinterpret_cast<uint64_t>(channel);
    } else if (name == "grpcsharp_native_callback_dispatcher_init") {
      // Kept host-side: native code must not call a guest function pointer.
      dispatcher_ = X(s, 0);
    } else if (name == "grpcsharp_redirect_log") {
      // Native gRPC keeps its default stderr logging.
    } else if (name == "grpcsharp_metadata_credentials_create_from_plugin" || name == "grpcsharp_test_callback" ||
               (name == "grpcsharp_ssl_credentials_create" && X(s, 3))) {
      throw std::runtime_error("export would make native gRPC call guest code; unsupported");
    } else {
      using G = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
      // At most 8 integer/pointer parameters (send_status_from_server has 9:
      // its ninth is on the guest stack).
      if (name == "grpcsharp_call_send_status_from_server") {
        uint32_t write_flags = 0;
        recomp::ReadGuestMemory(&write_flags, s.gpr.sp.qword, sizeof(write_flags));
        using H = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
        s.gpr.x0.qword = reinterpret_cast<H>(f)(X(s, 0), X(s, 1), X(s, 2), X(s, 3), X(s, 4), X(s, 5), X(s, 6), X(s, 7), write_flags);
      } else {
        s.gpr.x0.qword = reinterpret_cast<G>(f)(X(s, 0), X(s, 1), X(s, 2), X(s, 3), X(s, 4), X(s, 5), X(s, 6), X(s, 7));
      }
    }
  }

  Config config_;
  void *library_ = nullptr;
  std::vector<void *> functions_;
  WaitScope wait_scope_;
  WaitObserver wait_observer_;
  std::mutex pumps_mutex_;
  std::map<void *, std::shared_ptr<Pump>> pumps_;
  uint64_t dispatcher_ = 0;
};

}  // namespace bunker::windows::grpc_native
