#pragma once
// Guest-visible floating-point state on x86-64 hosts, kept in MXCSR only.
//
// Lifted AArch64 code performs all FP arithmetic with SSE, so its rounding
// mode and sticky exception flags live in MXCSR. MinGW's <cfenv> functions
// also save/load the whole x87 environment (fnstenv/fldenv, fnclex), which is
// far slower and dominated the import/guest-call paths. These helpers touch
// MXCSR directly; save/restore additionally keeps the x87 control word (cheap
// fnstcw/fldcw) so host x87 rounding/precision is preserved exactly. x87 status
// flags are host-only state that no guest-visible query reads.
//
// FE_* values differ between C libraries (UCRT uses FE_INVALID=0x10 and RC
// values 0x100..0x300), so they are mapped to MXCSR bits explicitly.
#include <cfenv>
#include <cstdint>
#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#define RECOMP_FAST_FENV 1
#endif

namespace recomp::fpenv {
#ifdef RECOMP_FAST_FENV
constexpr unsigned kFlags = 0x3d, kRounding = 0x6000;  // IE ZE OE UE PE (not DE), RC
constexpr unsigned ToMxcsr(int fe) {
  return (fe & FE_INVALID ? 0x01u : 0u) | (fe & FE_DIVBYZERO ? 0x04u : 0u) | (fe & FE_OVERFLOW ? 0x08u : 0u) |
         (fe & FE_UNDERFLOW ? 0x10u : 0u) | (fe & FE_INEXACT ? 0x20u : 0u);
}
constexpr int FromMxcsr(unsigned csr) {
  return (csr & 0x01u ? FE_INVALID : 0) | (csr & 0x04u ? FE_DIVBYZERO : 0) | (csr & 0x08u ? FE_OVERFLOW : 0) |
         (csr & 0x10u ? FE_UNDERFLOW : 0) | (csr & 0x20u ? FE_INEXACT : 0);
}
constexpr unsigned RoundToMxcsr(int round) {
  return round == FE_DOWNWARD ? 0x2000u : round == FE_UPWARD ? 0x4000u : round == FE_TOWARDZERO ? 0x6000u : 0u;
}
constexpr int RoundFromMxcsr(unsigned csr) {
  switch (csr & kRounding) {
    case 0x2000u: return FE_DOWNWARD;
    case 0x4000u: return FE_UPWARD;
    case 0x6000u: return FE_TOWARDZERO;
    default: return FE_TONEAREST;
  }
}
static_assert(FromMxcsr(ToMxcsr(FE_ALL_EXCEPT)) == FE_ALL_EXCEPT && ToMxcsr(FE_ALL_EXCEPT) == kFlags,
              "FE_ALL_EXCEPT must be the five IEEE exceptions");
static_assert(RoundFromMxcsr(RoundToMxcsr(FE_UPWARD)) == FE_UPWARD && RoundFromMxcsr(RoundToMxcsr(FE_DOWNWARD)) == FE_DOWNWARD &&
              RoundFromMxcsr(RoundToMxcsr(FE_TOWARDZERO)) == FE_TOWARDZERO && RoundFromMxcsr(RoundToMxcsr(FE_TONEAREST)) == FE_TONEAREST,
              "rounding modes must round-trip through MXCSR RC");
struct Saved {
  unsigned mxcsr;
  uint16_t x87_control;
};
inline Saved Save() noexcept {
  Saved saved;
  saved.mxcsr = _mm_getcsr();
  __asm__ volatile("fnstcw %0" : "=m"(saved.x87_control));
  return saved;
}
inline void Restore(const Saved &saved) noexcept {
  _mm_setcsr(saved.mxcsr);
  uint16_t control;
  __asm__ volatile("fnstcw %0" : "=m"(control));
  if (control != saved.x87_control) __asm__ volatile("fldcw %0" : : "m"(saved.x87_control));
}
inline int Test(int excepts) noexcept { return FromMxcsr(_mm_getcsr()) & excepts; }
inline void Clear(int excepts) noexcept {
  const unsigned csr = _mm_getcsr(), next = csr & ~ToMxcsr(excepts);
  if (next != csr) _mm_setcsr(next);
}
// Host exceptions are masked, so setting the sticky flags is the observable
// effect of raising them.
inline void Raise(int excepts) noexcept { _mm_setcsr(_mm_getcsr() | ToMxcsr(excepts)); }
inline void SetRound(int round) noexcept {
  const unsigned csr = _mm_getcsr(), next = (csr & ~kRounding) | RoundToMxcsr(round);
  if (next != csr) _mm_setcsr(next);
}
inline int GetRound() noexcept { return RoundFromMxcsr(_mm_getcsr()); }
// feholdexcept: save, clear the sticky flags and mask every exception.
inline Saved Hold() noexcept {
  const Saved saved = Save();
  const unsigned next = (saved.mxcsr | 0x1f80u) & ~0x3fu;
  if (next != saved.mxcsr) _mm_setcsr(next);
  return saved;
}
#else
struct Saved {
  std::fenv_t environment;
};
inline Saved Save() noexcept { Saved saved; std::fegetenv(&saved.environment); return saved; }
inline void Restore(const Saved &saved) noexcept { std::fesetenv(&saved.environment); }
inline int Test(int excepts) noexcept { return std::fetestexcept(excepts); }
inline void Clear(int excepts) noexcept { std::feclearexcept(excepts); }
inline void Raise(int excepts) noexcept { std::feraiseexcept(excepts); }
inline void SetRound(int round) noexcept { std::fesetround(round); }
inline int GetRound() noexcept { return std::fegetround(); }
inline Saved Hold() noexcept { Saved saved; std::feholdexcept(&saved.environment); return saved; }
#endif
}  // namespace recomp::fpenv
