#pragma once
// Pointer-sized thread-local slots without MinGW emulated TLS.
//
// MinGW GCC lowers `thread_local` to __emutls_get_address, which goes through
// pthread_once/pthread_getspecific/spinlocks on every access. The runtime's hot
// paths (every poll site, indirect dispatch, FP exception queries) read a few
// thread-locals, so on Windows they use a Win32 TLS slot read straight from the
// TEB (no API call, GetLastError untouched). Other hosts keep `thread_local`.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#endif

namespace recomp {
// Tag makes each declaration distinct. Declare instances as function-local
// statics so the slot exists before any (static-initialization-time) use.
template <typename T, typename Tag> class FastThreadLocal {
  static_assert(sizeof(T) <= sizeof(void *) && std::is_trivially_copyable<T>::value,
                "FastThreadLocal holds pointer-sized trivially copyable values");
 public:
#ifdef _WIN32
  FastThreadLocal() : slot_(TlsAlloc()) {
    if (slot_ == TLS_OUT_OF_INDEXES) std::abort();
  }
  T get() const noexcept {
    uintptr_t raw;
    if (slot_ < 64) {
      raw = __readgsqword(0x1480 + slot_ * 8);  // TEB.TlsSlots[slot]
    } else {
      const DWORD error = GetLastError();
      raw = reinterpret_cast<uintptr_t>(TlsGetValue(slot_));
      SetLastError(error);
    }
    T value;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
  }
  // Win32 TLS index: slots below 64 live at TEB+0x1480+8*index.
  DWORD index() const noexcept { return slot_; }
  void set(T value) noexcept {
    uintptr_t raw = 0;
    std::memcpy(&raw, &value, sizeof(value));
    if (slot_ < 64) {
      // TlsSetValue stores exactly here for the 64 inline slots.
      __writegsqword(0x1480 + slot_ * 8, raw);  // TEB.TlsSlots[slot]
      return;
    }
    const DWORD error = GetLastError();
    TlsSetValue(slot_, reinterpret_cast<void *>(raw));
    SetLastError(error);
  }
 private:
  DWORD slot_;
#else
  T get() const noexcept { return value(); }
  void set(T v) noexcept { value() = v; }
 private:
  static T &value() noexcept { static thread_local T v{}; return v; }
#endif
 public:
  operator T() const noexcept { return get(); }
  FastThreadLocal &operator=(T value) noexcept { set(value); return *this; }
  T operator->() const noexcept { return get(); }
};
}  // namespace recomp

namespace recomp {
// A zero-size namespace-scope handle with variable-like syntax. The slot is a
// function-local static, so it is created on first use from any TU or thread.
template <typename T, typename Tag> struct ThreadLocalRef {
  static FastThreadLocal<T, Tag> &slot() noexcept { static FastThreadLocal<T, Tag> s; return s; }
  operator T() const noexcept { return slot().get(); }
  T get() const noexcept { return slot().get(); }
  const ThreadLocalRef &operator=(T value) const noexcept { slot().set(value); return *this; }
  T operator->() const noexcept { return slot().get(); }
  auto &operator*() const noexcept { return *slot().get(); }
  const ThreadLocalRef &operator|=(T value) const noexcept { slot().set(T(slot().get() | value)); return *this; }
  const ThreadLocalRef &operator&=(T value) const noexcept { slot().set(T(slot().get() & value)); return *this; }
};
}  // namespace recomp
