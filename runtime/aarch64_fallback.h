#pragma once
// Host emulation for AArch64 instructions the cohort lifter left unsupported.
//
// A lifted trace that reaches such an instruction X calls
// __remill_error(state, X + 4) with every architectural register already
// stored in State. Emulate(state, word) performs X on State (and on guest
// memory through the __remill memory intrinsics), so the caller can resume at
// X + 4, where a separately lifted trace head starts. It returns false for any
// encoding it does not implement, and then State is untouched.
//
// Covered encoding groups (everything the 3.7.1 cohort reported):
// AdvSIMD modified immediate, three same, three different, two-register misc,
// across lanes, scalar pairwise, shift by immediate, load/store multiple
// structures (post-index and no offset); FP<->integer (also fixed point),
// FP 1-source rounding, FCCMP/FCCMPE; LDURSB/LDURSH/LDURSW.
// Floating point follows the default FPCR (round to nearest even, no flush to
// zero, IEEE half precision); cumulative FPSR exception bits and QC are not
// updated. Platform-neutral C++17.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "remill/Arch/AArch64/Runtime/State.h"
#include "remill/Arch/Runtime/Intrinsics.h"

namespace recomp::aarch64_fallback {
namespace detail {

inline uint64_t Bits(uint32_t w, int hi, int lo) { return (w >> lo) & ((1ull << (hi - lo + 1)) - 1); }
inline Reg &X(State &s, unsigned n) {  // n == 31 means SP (callers handle XZR).
  return *reinterpret_cast<Reg *>(reinterpret_cast<char *>(&s.gpr) + 8 + 16 * n);
}
inline uint64_t ReadX(State &s, unsigned n) { return n == 31 ? 0 : X(s, n).qword; }
inline void WriteX(State &s, unsigned n, uint64_t v, bool sf) {
  if (n != 31) X(s, n).qword = sf ? v : (v & 0xffffffffu);
}

struct Vec { uint8_t b[16]; };
inline Vec GetV(const State &s, unsigned n) { Vec v; std::memcpy(v.b, &s.simd.v[n], 16); return v; }
inline void SetV(State &s, unsigned n, const Vec &v) { std::memcpy(&s.simd.v[n], v.b, 16); }
inline Vec Zero() { Vec v; std::memset(v.b, 0, 16); return v; }
// Element access, esize in bits (8..64), little-endian lanes.
inline uint64_t El(const Vec &v, unsigned i, unsigned esize) {
  uint64_t r = 0; std::memcpy(&r, v.b + i * (esize / 8), esize / 8); return r;
}
inline void SetEl(Vec &v, unsigned i, unsigned esize, uint64_t x) { std::memcpy(v.b + i * (esize / 8), &x, esize / 8); }
inline int64_t SExt(uint64_t x, unsigned bits) {
  return bits >= 64 ? int64_t(x) : int64_t(x << (64 - bits)) >> (64 - bits);
}
inline uint64_t Mask(unsigned bits) { return bits >= 64 ? ~0ull : (1ull << bits) - 1; }
// Writes a 64-bit (Q=0) or 128-bit result; Q=0 clears the upper half.
inline void WriteVQ(State &s, unsigned n, Vec v, bool q) { if (!q) std::memset(v.b + 8, 0, 8); SetV(s, n, v); }

// ---- floating point helpers -------------------------------------------------
inline float F32(uint64_t b) { float f; uint32_t u = uint32_t(b); std::memcpy(&f, &u, 4); return f; }
inline double F64(uint64_t b) { double f; std::memcpy(&f, &b, 8); return f; }
inline uint64_t B32(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline uint64_t B64(double f) { uint64_t u; std::memcpy(&u, &f, 8); return u; }

// Round to integral per AArch64 FPRounding: 0 nearest-even, 1 +inf, 2 -inf,
// 3 zero, 4 ties away.
template <class T> T RoundInt(T x, int mode) {
  if (!std::isfinite(x) || x == 0) return x;
  T r;
  switch (mode) {
    case 1: r = std::ceil(x); break;
    case 2: r = std::floor(x); break;
    case 3: r = std::trunc(x); break;
    case 4: r = std::round(x); break;
    default: { r = std::floor(x); const T d = x - r; if (d > T(0.5) || (d == T(0.5) && std::fmod(r, T(2)) != 0)) r += 1; }
  }
  return r == 0 ? std::copysign(T(0), x) : r;  // keeps the sign of zero
}
// FP -> integer with saturation; NaN -> 0.
template <class T> uint64_t FpToInt(T x, int mode, unsigned bits, bool is_unsigned) {
  if (std::isnan(x)) return 0;
  const long double r = RoundInt<long double>(static_cast<long double>(x), mode);
  if (is_unsigned) {
    if (r <= 0) return 0;
    const long double max = std::ldexp(1.0L, int(bits)) - 1;
    if (r >= max) return Mask(bits);
    return uint64_t(r);
  }
  const long double min = -std::ldexp(1.0L, int(bits) - 1), max = std::ldexp(1.0L, int(bits) - 1) - 1;
  if (r <= min) return uint64_t(int64_t(-1) << (bits - 1)) & Mask(bits);
  if (r >= max) return Mask(bits - 1);
  return uint64_t(int64_t(r)) & Mask(bits);
}
inline uint64_t DefaultNaN(unsigned esize) { return esize == 32 ? 0x7fc00000u : 0x7ff8000000000000ull; }
inline bool IsSNaN(uint64_t b, unsigned esize) {
  return esize == 32 ? ((b & 0x7f800000u) == 0x7f800000u && (b & 0x7fffffu) && !(b & 0x400000u))
                     : ((b & 0x7ff0000000000000ull) == 0x7ff0000000000000ull && (b & 0xfffffffffffffull) && !(b & 0x8000000000000ull));
}
inline bool IsNaNBits(uint64_t b, unsigned esize) { return esize == 32 ? std::isnan(F32(b)) : std::isnan(F64(b)); }
inline uint64_t Quiet(uint64_t b, unsigned esize) { return b | (esize == 32 ? 0x400000ull : 0x8000000000000ull); }
// FPProcessNaNs: signalling first (op1 then op2), then quiet.
inline bool ProcessNaNs(uint64_t a, uint64_t b, unsigned esize, uint64_t &out) {
  if (IsSNaN(a, esize)) { out = Quiet(a, esize); return true; }
  if (IsSNaN(b, esize)) { out = Quiet(b, esize); return true; }
  if (IsNaNBits(a, esize)) { out = a; return true; }
  if (IsNaNBits(b, esize)) { out = b; return true; }
  return false;
}
inline uint64_t FMaxMin(uint64_t a, uint64_t b, unsigned esize, bool is_min) {
  uint64_t nan;
  if (ProcessNaNs(a, b, esize, nan)) return nan;
  const double x = esize == 32 ? F32(a) : F64(a), y = esize == 32 ? F32(b) : F64(b);
  if (x == 0 && y == 0) {  // signed zeros: max(+0,-0)=+0, min(+0,-0)=-0
    const bool sx = std::signbit(x), sy = std::signbit(y);
    return (is_min ? (sx || sy) : (sx && sy)) ? (esize == 32 ? 0x80000000ull : 0x8000000000000000ull) : 0;
  }
  return (is_min ? (x < y) : (x > y)) ? a : b;
}
// IEEE half <-> single, round to nearest even (FPCR.AHP = 0).
inline uint32_t HalfToSingle(uint16_t h) {
  const uint32_t sign = uint32_t(h >> 15) << 31; uint32_t exp = (h >> 10) & 0x1f, frac = h & 0x3ff;
  if (exp == 0x1f) return sign | 0x7f800000u | (frac << 13) | (frac ? 0x400000u : 0);  // NaN quieted
  if (exp == 0) {
    if (!frac) return sign;
    int e = -1; do { frac <<= 1; ++e; } while (!(frac & 0x400));
    return sign | (uint32_t(127 - 15 - e) << 23) | ((frac & 0x3ff) << 13);
  }
  return sign | ((exp + 112) << 23) | (frac << 13);
}
inline uint16_t SingleToHalf(uint32_t f) {
  const uint16_t sign = uint16_t((f >> 16) & 0x8000); const int32_t exp = int32_t((f >> 23) & 0xff); const uint32_t frac = f & 0x7fffff;
  if (exp == 0xff) return frac ? uint16_t(sign | 0x7e00 | (frac >> 13)) : uint16_t(sign | 0x7c00);
  if (exp == 0) return sign;  // single denormals round to zero in half
  int32_t e = exp - 127 + 15; const uint32_t m = frac | 0x800000;
  if (e >= 31) return sign | 0x7c00;
  if (e <= 0) {  // half subnormal (may round up to the smallest normal, same encoding)
    const unsigned shift = unsigned(14 - e); if (shift >= 25) return sign;
    uint32_t mant = m >> shift; const uint32_t rem = m & ((1u << shift) - 1), half = 1u << (shift - 1);
    if (rem > half || (rem == half && (mant & 1))) ++mant;
    return uint16_t(sign | mant);
  }
  uint32_t mant = m >> 13; const uint32_t rem = m & 0x1fff;
  if (rem > 0x1000 || (rem == 0x1000 && (mant & 1))) ++mant;
  if (mant == 0x800) { mant = 0x400; if (++e >= 31) return sign | 0x7c00; }
  return uint16_t(sign | (uint32_t(e) << 10) | (mant & 0x3ff));
}
inline bool ConditionHolds(const State &s, unsigned cond) {
  const bool n = s.sr.n, z = s.sr.z, c = s.sr.c, v = s.sr.v; bool r;
  switch (cond >> 1) {
    case 0: r = z; break; case 1: r = c; break; case 2: r = n; break; case 3: r = v; break;
    case 4: r = c && !z; break; case 5: r = n == v; break; case 6: r = n == v && !z; break; default: r = true;
  }
  return (cond & 1) && cond != 15 ? !r : r;
}
inline void SetNZCV(State &s, unsigned nzcv) { s.sr.n = (nzcv >> 3) & 1; s.sr.z = (nzcv >> 2) & 1; s.sr.c = (nzcv >> 1) & 1; s.sr.v = nzcv & 1; }

// ---- memory -----------------------------------------------------------------
inline uint64_t Load(addr_t a, unsigned bytes) {
  switch (bytes) {
    case 1: return __remill_read_memory_8(nullptr, a);
    case 2: return __remill_read_memory_16(nullptr, a);
    case 4: return __remill_read_memory_32(nullptr, a);
    default: return __remill_read_memory_64(nullptr, a);
  }
}
inline void Store(addr_t a, unsigned bytes, uint64_t v) {
  switch (bytes) {
    case 1: __remill_write_memory_8(nullptr, a, uint8_t(v)); break;
    case 2: __remill_write_memory_16(nullptr, a, uint16_t(v)); break;
    case 4: __remill_write_memory_32(nullptr, a, uint32_t(v)); break;
    default: __remill_write_memory_64(nullptr, a, v);
  }
}

// ---- groups -----------------------------------------------------------------
// AdvSIMD modified immediate: MOVI/MVNI/ORR/BIC/FMOV (vector).
inline bool ModifiedImmediate(State &s, uint32_t w) {
  const bool q = Bits(w, 30, 30), op = Bits(w, 29, 29); const unsigned cmode = Bits(w, 15, 12), rd = Bits(w, 4, 0);
  const uint64_t imm8 = (Bits(w, 18, 16) << 5) | Bits(w, 9, 5);
  uint64_t imm = 0;
  auto rep32 = [](uint64_t x) { return x | (x << 32); };
  auto rep16 = [](uint64_t x) { return x * 0x0001000100010001ull; };
  switch (cmode >> 1) {
    case 0: imm = rep32(imm8); break; case 1: imm = rep32(imm8 << 8); break;
    case 2: imm = rep32(imm8 << 16); break; case 3: imm = rep32(imm8 << 24); break;
    case 4: imm = rep16(imm8); break; case 5: imm = rep16(imm8 << 8); break;
    case 6: imm = rep32((cmode & 1) ? ((imm8 << 16) | 0xffff) : ((imm8 << 8) | 0xff)); break;
    case 7:
      if (!(cmode & 1) && !op) imm = imm8 * 0x0101010101010101ull;
      else if (!(cmode & 1) && op) { for (int i = 0; i < 8; ++i) if (imm8 >> i & 1) imm |= 0xffull << (8 * i); }
      else if ((cmode & 1) && !op) {
        const uint64_t b6 = (imm8 >> 6) & 1;
        const uint64_t f = ((imm8 >> 7) << 31) | (((b6 ^ 1) << 7 | (b6 ? 0x7c : 0) | ((imm8 >> 4) & 3)) << 23) | ((imm8 & 0xf) << 19);
        imm = rep32(f);
      } else {
        if (!q) return false;
        const uint64_t b6 = (imm8 >> 6) & 1;
        imm = ((imm8 >> 7) << 63) | (((b6 ^ 1) << 10 | (b6 ? 0x3fc : 0) | ((imm8 >> 4) & 3)) << 52) | ((imm8 & 0xf) << 48);
      }
      break;
  }
  const bool logical = cmode < 12 && (cmode & 1);  // 0xx1 / 10x1
  Vec v = GetV(s, rd), r;
  for (unsigned i = 0; i < 2; ++i) {
    const uint64_t cur = El(v, i, 64); uint64_t x;
    if (logical) x = op ? (cur & ~imm) : (cur | imm);
    else x = (op && cmode != 14 && cmode != 15) ? ~imm : imm;
    SetEl(r, i, 64, x);
  }
  WriteVQ(s, rd, r, q);
  return true;
}

inline bool ThreeSame(State &s, uint32_t w) {
  const bool q = Bits(w, 30, 30), u = Bits(w, 29, 29); const unsigned size = Bits(w, 23, 22), opc = Bits(w, 15, 11);
  const unsigned rm = Bits(w, 20, 16), rn = Bits(w, 9, 5), rd = Bits(w, 4, 0);
  const Vec n = GetV(s, rn), m = GetV(s, rm); Vec r = Zero();
  if (opc >= 0x18) {  // floating point
    const unsigned esize = (size & 1) ? 64 : 32, lanes = (q ? 128 : 64) / esize; const bool a = size >> 1;
    if (esize == 64 && !q) return false;
    if (u && opc == 0x1e) {  // FMAXP / FMINP
      for (unsigned i = 0; i < lanes; ++i) {
        const Vec &src = i < lanes / 2 ? n : m; const unsigned j = 2 * (i % (lanes / 2));
        SetEl(r, i, esize, FMaxMin(El(src, j, esize), El(src, j + 1, esize), esize, a));
      }
      WriteVQ(s, rd, r, q); return true;
    }
    return false;
  }
  const unsigned esize = 8u << size, lanes = (q ? 128 : 64) / esize;
  if (esize == 64 && !q) return false;
  for (unsigned i = 0; i < lanes; ++i) {
    const uint64_t x = El(n, i, esize), y = El(m, i, esize); uint64_t out;
    switch (opc) {
      case 0x01: {  // SQADD / UQADD
        if (u) { const uint64_t sum = (x + y) & Mask(esize); out = (esize < 64 ? (x + y) > Mask(esize) : sum < x) ? Mask(esize) : sum; }
        else {
          const int64_t a = SExt(x, esize), b = SExt(y, esize), lo = -int64_t(Mask(esize - 1)) - 1, hi = int64_t(Mask(esize - 1));
          int64_t sum; if (__builtin_add_overflow(a, b, &sum)) sum = a < 0 ? lo : hi;
          out = uint64_t(sum < lo ? lo : sum > hi ? hi : sum) & Mask(esize);
        }
        break;
      }
      case 0x06: out = (u ? x > y : SExt(x, esize) > SExt(y, esize)) ? Mask(esize) : 0; break;   // CMGT / CMHI
      case 0x07: out = (u ? x >= y : SExt(x, esize) >= SExt(y, esize)) ? Mask(esize) : 0; break; // CMGE / CMHS
      case 0x08: {  // SSHL / USHL (non-rounding, non-saturating)
        const int sh = int8_t(y & 0xff);
        if (sh >= 0) out = sh >= int(esize) ? 0 : (x << sh) & Mask(esize);
        else if (u) out = -sh >= int(esize) ? 0 : x >> -sh;
        else out = uint64_t(SExt(x, esize) >> (-sh >= int(esize) ? esize - 1 : -sh)) & Mask(esize);
        break;
      }
      case 0x10: out = (u ? x - y : x + y) & Mask(esize); break;  // ADD / SUB
      case 0x17:  // ADDP (vector)
        if (u) return false;
        { const Vec &src = i < lanes / 2 ? n : m; const unsigned j = 2 * (i % (lanes / 2));
          out = (El(src, j, esize) + El(src, j + 1, esize)) & Mask(esize); }
        break;
      default: return false;
    }
    SetEl(r, i, esize, out);
  }
  WriteVQ(s, rd, r, q);
  return true;
}

inline bool ThreeDifferent(State &s, uint32_t w) {
  const bool q = Bits(w, 30, 30), u = Bits(w, 29, 29); const unsigned size = Bits(w, 23, 22), opc = Bits(w, 15, 12);
  const unsigned rm = Bits(w, 20, 16), rn = Bits(w, 9, 5), rd = Bits(w, 4, 0);
  if (size == 3) return false;
  const unsigned esize = 8u << size, lanes = 64 / esize, part = q ? lanes : 0;
  const Vec n = GetV(s, rn), m = GetV(s, rm), d = GetV(s, rd); Vec r = Zero();
  auto ext = [&](uint64_t x) { return u ? x : uint64_t(SExt(x, esize)); };
  switch (opc) {
    case 0x0: case 0x1: case 0x8: case 0xc:
      for (unsigned i = 0; i < lanes; ++i) {
        const uint64_t mv = ext(El(m, part + i, esize)); uint64_t out;
        if (opc == 0x0) out = ext(El(n, part + i, esize)) + mv;            // xADDL
        else if (opc == 0x1) out = El(n, i, 2 * esize) + mv;               // xADDW
        else if (opc == 0x8) out = El(d, i, 2 * esize) + ext(El(n, part + i, esize)) * mv;  // xMLAL
        else out = ext(El(n, part + i, esize)) * mv;                       // xMULL
        SetEl(r, i, 2 * esize, out & Mask(2 * esize));
      }
      SetV(s, rd, r); return true;
    case 0x4: {  // ADDHN / RADDHN
      Vec out = q ? d : Zero();
      for (unsigned i = 0; i < lanes; ++i) {
        uint64_t sum = El(n, i, 2 * esize) + El(m, i, 2 * esize);
        if (u) sum += 1ull << (esize - 1);
        SetEl(out, part + i, esize, (sum & Mask(2 * esize)) >> esize);
      }
      SetV(s, rd, out); return true;
    }
    default: return false;
  }
}

// Narrowing helpers for XTN/SQXTN/UQXTN/SQXTUN.
inline uint64_t Narrow(uint64_t x, unsigned esize, int kind) {  // 0 xtn, 1 sqxtn, 2 uqxtn, 3 sqxtun
  const unsigned src = 2 * esize;
  if (kind == 0) return x & Mask(esize);
  if (kind == 2) return x > Mask(esize) ? Mask(esize) : x;
  const int64_t v = SExt(x, src);
  if (kind == 1) { const int64_t hi = int64_t(Mask(esize - 1)), lo = -hi - 1; return uint64_t(v > hi ? hi : v < lo ? lo : v) & Mask(esize); }
  return v < 0 ? 0 : uint64_t(v) > Mask(esize) ? Mask(esize) : uint64_t(v);
}

inline bool TwoRegMisc(State &s, uint32_t w) {
  const bool q = Bits(w, 30, 30), u = Bits(w, 29, 29); const unsigned size = Bits(w, 23, 22), opc = Bits(w, 16, 12);
  const unsigned rn = Bits(w, 9, 5), rd = Bits(w, 4, 0);
  const Vec n = GetV(s, rn); Vec r = Zero();
  if (opc >= 0x0c && !(opc == 0x12 || opc == 0x13 || opc == 0x14)) {  // floating point forms
    const bool a = size >> 1, sz = size & 1;
    if (opc == 0x16 && !u && !a) {  // FCVTN{2}: 4s->4h or 2d->2s
      Vec out = q ? GetV(s, rd) : Zero();
      const unsigned lanes = sz ? 2 : 4, part = q ? lanes : 0;
      for (unsigned i = 0; i < lanes; ++i) {
        if (sz) { const double x = F64(El(n, i, 64)); float f = float(x); uint64_t b = B32(f);
          if (std::isnan(x)) b = 0x7fc00000u | (uint32_t(El(n, i, 64) >> 63) << 31) | uint32_t((El(n, i, 64) >> 29) & 0x3fffff);
          SetEl(out, part + i, 32, b); }
        else SetEl(out, part + i, 16, SingleToHalf(uint32_t(El(n, i, 32))));
      }
      SetV(s, rd, out); return true;
    }
    if (opc == 0x17 && !u && !a) {  // FCVTL{2}
      const unsigned lanes = sz ? 2 : 4, part = q ? lanes : 0;
      for (unsigned i = 0; i < lanes; ++i) {
        if (sz) { const uint64_t b = El(n, part + i, 32); uint64_t out = B64(double(F32(b)));
          if (std::isnan(F32(b))) out = 0x7ff8000000000000ull | ((b >> 31) << 63) | ((b & 0x3fffff) << 29);
          SetEl(r, i, 64, out); }
        else SetEl(r, i, 32, HalfToSingle(uint16_t(El(n, part + i, 16))));
      }
      SetV(s, rd, r); return true;
    }
    const unsigned esize = sz ? 64 : 32, lanes = (q ? 128 : 64) / esize;
    if (sz && !q) return false;
    int mode = -1; bool to_int = false;
    // FRINT*: opc 11000/11001 with (U, a, o) -> mode.
    if (opc == 0x18 || opc == 0x19) {
      const unsigned key = (u << 2) | (a << 1) | (opc & 1);
      static const int kModes[8] = {0, 2, 1, 3, 4, 0, -1, 0};  // N M P Z A X - I (X/I: FPCR = nearest)
      mode = kModes[key];
      if (mode < 0) return false;
    } else if (opc == 0x1a || opc == 0x1b || (opc == 0x1c && !a)) {
      to_int = true;
      mode = opc == 0x1c ? 4 : opc == 0x1a ? (a ? 1 : 0) : (a ? 3 : 2);
    } else return false;
    for (unsigned i = 0; i < lanes; ++i) {
      const uint64_t b = El(n, i, esize); uint64_t out;
      if (to_int) out = esize == 32 ? FpToInt(F32(b), mode, 32, u) : FpToInt(F64(b), mode, 64, u);
      else if (IsNaNBits(b, esize)) out = Quiet(b, esize);
      else out = esize == 32 ? B32(RoundInt(F32(b), mode)) : B64(RoundInt(F64(b), mode));
      SetEl(r, i, esize, out);
    }
    WriteVQ(s, rd, r, q); return true;
  }
  const unsigned esize = 8u << size;
  switch (opc) {
    case 0x05:  // CNT (U=0, size=0) / NOT, RBIT otherwise
      if (u || size) return false;
      for (unsigned i = 0; i < (q ? 16u : 8u); ++i) r.b[i] = uint8_t(__builtin_popcount(n.b[i]));
      WriteVQ(s, rd, r, q); return true;
    case 0x0b: {  // ABS / NEG
      if (esize == 64 && !q) return false;
      for (unsigned i = 0; i < (q ? 128 : 64) / esize; ++i) {
        const int64_t x = SExt(El(n, i, esize), esize);
        SetEl(r, i, esize, uint64_t(u ? -uint64_t(x) : (x < 0 ? -uint64_t(x) : uint64_t(x))) & Mask(esize));
      }
      WriteVQ(s, rd, r, q); return true;
    }
    case 0x12: case 0x14: {  // XTN/SQXTUN, SQXTN/UQXTN
      if (size == 3) return false;
      const int kind = opc == 0x12 ? (u ? 3 : 0) : (u ? 2 : 1);
      Vec out = q ? GetV(s, rd) : Zero(); const unsigned lanes = 64 / esize, part = q ? lanes : 0;
      for (unsigned i = 0; i < lanes; ++i) SetEl(out, part + i, esize, Narrow(El(n, i, 2 * esize), esize, kind));
      SetV(s, rd, out); return true;
    }
    case 0x13: {  // SHLL{2} (U=1)
      if (!u || size == 3) return false;
      const unsigned lanes = 64 / esize, part = q ? lanes : 0;
      for (unsigned i = 0; i < lanes; ++i) SetEl(r, i, 2 * esize, (El(n, part + i, esize) << esize) & Mask(2 * esize));
      SetV(s, rd, r); return true;
    }
    default: return false;
  }
}

inline bool AcrossLanes(State &s, uint32_t w) {
  const bool q = Bits(w, 30, 30), u = Bits(w, 29, 29); const unsigned size = Bits(w, 23, 22), opc = Bits(w, 16, 12);
  const unsigned rn = Bits(w, 9, 5), rd = Bits(w, 4, 0); const unsigned esize = 8u << size, lanes = (q ? 128 : 64) / esize;
  if (size == 3 || (size == 2 && !q)) return false;
  const Vec n = GetV(s, rn); Vec r = Zero();
  if (opc == 0x03) {  // SADDLV / UADDLV
    uint64_t sum = 0;
    for (unsigned i = 0; i < lanes; ++i) sum += u ? El(n, i, esize) : uint64_t(SExt(El(n, i, esize), esize));
    SetEl(r, 0, 2 * esize, sum & Mask(2 * esize));
  } else if (opc == 0x1b && !u) {  // ADDV
    uint64_t sum = 0; for (unsigned i = 0; i < lanes; ++i) sum += El(n, i, esize);
    SetEl(r, 0, esize, sum & Mask(esize));
  } else return false;
  SetV(s, rd, r); return true;
}

inline bool ShiftImmediate(State &s, uint32_t w) {
  const bool q = Bits(w, 30, 30), u = Bits(w, 29, 29); const unsigned immh = Bits(w, 22, 19), immhb = Bits(w, 22, 16);
  const unsigned opc = Bits(w, 15, 11), rn = Bits(w, 9, 5), rd = Bits(w, 4, 0);
  const unsigned esize = 8u << (31 - __builtin_clz(immh));
  const Vec n = GetV(s, rn), d = GetV(s, rd); Vec r = Zero();
  if (opc == 0x10 || opc == 0x14) {  // SHRN/RSHRN-free narrow, SSHLL/USHLL widen (esize = narrow size)
    if (esize == 64) return false;
    const unsigned lanes = 64 / esize, part = q ? lanes : 0;
    if (opc == 0x10) {
      if (u) return false;  // SQSHRUN
      const unsigned shift = 2 * esize - immhb; Vec out = q ? d : Zero();
      for (unsigned i = 0; i < lanes; ++i) SetEl(out, part + i, esize, (El(n, i, 2 * esize) >> shift) & Mask(esize));
      SetV(s, rd, out); return true;
    }
    const unsigned shift = immhb - esize;
    for (unsigned i = 0; i < lanes; ++i) {
      const uint64_t x = El(n, part + i, esize);
      SetEl(r, i, 2 * esize, ((u ? x : uint64_t(SExt(x, esize))) << shift) & Mask(2 * esize));
    }
    SetV(s, rd, r); return true;
  }
  if (esize == 64 && !q) return false;
  const unsigned lanes = (q ? 128 : 64) / esize, rshift = 2 * esize - immhb, lshift = immhb - esize;
  for (unsigned i = 0; i < lanes; ++i) {
    const uint64_t x = El(n, i, esize), acc = El(d, i, esize); uint64_t out;
    auto shr = [&](uint64_t v) {
      if (u) return rshift >= esize ? 0 : v >> rshift;
      return uint64_t(SExt(v, esize) >> (rshift >= esize ? esize - 1 : rshift)) & Mask(esize);
    };
    switch (opc) {
      case 0x00: out = shr(x); break;                               // SSHR / USHR
      case 0x02: out = (acc + shr(x)) & Mask(esize); break;         // SSRA / USRA
      case 0x08:                                                     // SRI
        if (!u) return false;
        out = rshift >= esize ? acc : ((acc & ~(Mask(esize) >> rshift)) | (x >> rshift)) & Mask(esize);
        break;
      case 0x0a:                                                     // SHL / SLI
        out = u ? ((acc & Mask(lshift)) | (x << lshift)) & Mask(esize) : (x << lshift) & Mask(esize);
        break;
      default: return false;
    }
    SetEl(r, i, esize, out);
  }
  WriteVQ(s, rd, r, q); return true;
}

// FP <-> integer, integer and fixed-point forms (only FP -> integer).
inline bool FpToIntGroup(State &s, uint32_t w) {
  const bool sf = Bits(w, 31, 31); const unsigned type = Bits(w, 23, 22), rmode = Bits(w, 20, 19), opc = Bits(w, 18, 16);
  const unsigned rn = Bits(w, 9, 5), rd = Bits(w, 4, 0); const bool fixed = !Bits(w, 21, 21);
  if (type > 1) return false;
  const unsigned bits = sf ? 64 : 32; const uint64_t b = El(GetV(s, rn), 0, type ? 64 : 32);
  int mode; bool is_unsigned;
  if (fixed) {
    if (rmode != 3 || opc > 1) return false;
    const unsigned scale = 64 - unsigned(Bits(w, 15, 10));
    if (!sf && scale > 32) return false;
    is_unsigned = opc; mode = 3;
    const long double x = type ? (long double)F64(b) : (long double)F32(b);
    uint64_t out;
    if (std::isnan(x)) out = 0;
    else out = FpToInt<long double>(std::ldexp(x, int(scale)), mode, bits, is_unsigned);
    WriteX(s, rd, out, sf); return true;
  }
  if (Bits(w, 15, 10)) return false;
  if (opc == 0 || opc == 1) { is_unsigned = opc; mode = rmode == 0 ? 0 : rmode == 1 ? 1 : rmode == 2 ? 2 : 3; }
  else if ((opc == 4 || opc == 5) && rmode == 0) { is_unsigned = opc == 5; mode = 4; }
  else return false;
  const uint64_t out = type ? FpToInt(F64(b), mode, bits, is_unsigned) : FpToInt(F32(b), mode, bits, is_unsigned);
  WriteX(s, rd, out, sf); return true;
}

inline bool FpRoundScalar(State &s, uint32_t w) {  // FRINT{N,P,M,Z,A,I} (scalar)
  const unsigned type = Bits(w, 23, 22), opc = Bits(w, 20, 15), rn = Bits(w, 9, 5), rd = Bits(w, 4, 0);
  if (type > 1 || (opc >> 3) != 1) return false;
  static const int kModes[8] = {0, 1, 2, 3, 4, -1, 0, 0};  // N P M Z A - X I (X/I: FPCR = nearest)
  const int mode = kModes[opc & 7]; if (mode < 0) return false;
  const unsigned esize = type ? 64 : 32; const uint64_t b = El(GetV(s, rn), 0, esize);
  Vec r = Zero();
  SetEl(r, 0, esize, IsNaNBits(b, esize) ? Quiet(b, esize) : esize == 32 ? B32(RoundInt(F32(b), mode)) : B64(RoundInt(F64(b), mode)));
  SetV(s, rd, r); return true;
}

// FMUL FDIV FADD FSUB FMAX FMIN FMAXNM FMINNM FNMUL (scalar, single/double).
// AArch64 NaN rules: FPProcessNaNs first; an invalid operation yields the
// positive default NaN (the x86 host would produce a negative one).
inline bool FpTwoSource(State &s, uint32_t w) {
  const unsigned type = Bits(w, 23, 22), rm = Bits(w, 20, 16), opc = Bits(w, 15, 12), rn = Bits(w, 9, 5), rd = Bits(w, 4, 0);
  if (type > 1 || opc > 8) return false;
  const unsigned esize = type ? 64 : 32;
  uint64_t a = El(GetV(s, rn), 0, esize), b = El(GetV(s, rm), 0, esize), out;
  if (opc == 6 || opc == 7) {  // FMAXNM / FMINNM: a single quiet NaN loses to a number
    const bool qa = IsNaNBits(a, esize) && !IsSNaN(a, esize), qb = IsNaNBits(b, esize) && !IsSNaN(b, esize);
    const uint64_t inf = esize == 32 ? (opc == 6 ? 0xff800000ull : 0x7f800000ull) : (opc == 6 ? 0xfff0000000000000ull : 0x7ff0000000000000ull);
    if (qa && !IsNaNBits(b, esize)) a = inf; else if (qb && !IsNaNBits(a, esize)) b = inf;
    out = FMaxMin(a, b, esize, opc == 7);
  } else if (opc == 4 || opc == 5) {
    out = FMaxMin(a, b, esize, opc == 5);
  } else if (!ProcessNaNs(a, b, esize, out)) {
    if (esize == 32) {
      const float x = F32(a), y = F32(b); volatile float r;
      switch (opc) { case 0: case 8: r = x * y; break; case 1: r = x / y; break; case 2: r = x + y; break; default: r = x - y; break; }
      out = std::isnan(r) ? DefaultNaN(32) : B32(r);
    } else {
      const double x = F64(a), y = F64(b); volatile double r;
      switch (opc) { case 0: case 8: r = x * y; break; case 1: r = x / y; break; case 2: r = x + y; break; default: r = x - y; break; }
      out = std::isnan(r) ? DefaultNaN(64) : B64(r);
    }
    if (opc == 8 && !IsNaNBits(out, esize)) out ^= esize == 32 ? 0x80000000ull : 0x8000000000000000ull;
  }
  Vec r = Zero(); SetEl(r, 0, esize, out); SetV(s, rd, r); return true;
}

inline bool FCCmp(State &s, uint32_t w) {
  const unsigned type = Bits(w, 23, 22), rm = Bits(w, 20, 16), cond = Bits(w, 15, 12), rn = Bits(w, 9, 5);
  if (type > 1) return false;
  if (!ConditionHolds(s, cond)) { SetNZCV(s, Bits(w, 3, 0)); return true; }
  const unsigned esize = type ? 64 : 32;
  const double a = esize == 32 ? F32(El(GetV(s, rn), 0, 32)) : F64(El(GetV(s, rn), 0, 64));
  const double b = esize == 32 ? F32(El(GetV(s, rm), 0, 32)) : F64(El(GetV(s, rm), 0, 64));
  SetNZCV(s, std::isnan(a) || std::isnan(b) ? 0x3 : a == b ? 0x6 : a < b ? 0x8 : 0x2);
  return true;
}

inline bool ScalarPairwise(State &s, uint32_t w) {  // ADDP Dd, Vn.2D
  if ((w & 0xfffffc00u) != 0x5ef1b800u) return false;
  const Vec n = GetV(s, Bits(w, 9, 5)); Vec r = Zero();
  SetEl(r, 0, 64, El(n, 0, 64) + El(n, 1, 64));
  SetV(s, Bits(w, 4, 0), r); return true;
}

inline bool LoadStoreMultiple(State &s, uint32_t w) {
  const bool q = Bits(w, 30, 30), load = Bits(w, 22, 22), post = Bits(w, 23, 23);
  const unsigned rm = Bits(w, 20, 16), opc = Bits(w, 15, 12), size = Bits(w, 11, 10), rn = Bits(w, 9, 5), rt = Bits(w, 4, 0);
  unsigned rpt, selem;
  switch (opc) {
    case 0x0: rpt = 1; selem = 4; break; case 0x2: rpt = 4; selem = 1; break;
    case 0x4: rpt = 1; selem = 3; break; case 0x6: rpt = 3; selem = 1; break;
    case 0x7: rpt = 1; selem = 1; break; case 0x8: rpt = 1; selem = 2; break;
    case 0xa: rpt = 2; selem = 1; break; default: return false;
  }
  if (size == 3 && !q && selem != 1) return false;
  const unsigned esize = 8u << size, ebytes = esize / 8, elements = (q ? 128 : 64) / esize;
  const addr_t base = X(s, rn).qword; addr_t offs = 0;
  Vec regs[4]; for (unsigned i = 0; i < 4; ++i) regs[i] = GetV(s, (rt + i) % 32);
  for (unsigned r = 0; r < rpt; ++r)
    for (unsigned e = 0; e < elements; ++e)
      for (unsigned k = 0; k < selem; ++k) {
        Vec &v = regs[(r + k) % 4];
        if (load) SetEl(v, e, esize, Load(base + offs, ebytes)); else Store(base + offs, ebytes, El(v, e, esize));
        offs += ebytes;
      }
  if (load) for (unsigned i = 0; i < rpt * selem; ++i) WriteVQ(s, (rt + i) % 32, regs[i], q);
  if (post) X(s, rn).qword = base + (rm == 31 ? offs : ReadX(s, rm));
  return true;
}

inline bool LoadUnscaledSigned(State &s, uint32_t w) {  // LDURSB/LDURSH/LDURSW
  const unsigned size = Bits(w, 31, 30), opc = Bits(w, 23, 22), rn = Bits(w, 9, 5), rt = Bits(w, 4, 0);
  if (opc < 2 || (size == 2 && opc == 3) || size == 3) return false;
  const int64_t imm = SExt(Bits(w, 20, 12), 9); const unsigned bytes = 1u << size;
  const uint64_t v = uint64_t(SExt(Load(X(s, rn).qword + imm, bytes), 8 * bytes));
  WriteX(s, rt, v, opc == 2);
  return true;
}

}  // namespace detail

inline bool Emulate(State &s, uint32_t w) {
  using namespace detail;
  if ((w & 0x9ff80400u) == 0x0f000400u) return ModifiedImmediate(s, w);
  if ((w & 0x9f800400u) == 0x0f000400u) return ShiftImmediate(s, w);  // immh != 0 here
  if ((w & 0x9f200400u) == 0x0e200400u) return ThreeSame(s, w);
  if ((w & 0x9f3e0c00u) == 0x0e200800u) return TwoRegMisc(s, w);
  if ((w & 0x9f3e0c00u) == 0x0e300800u) return AcrossLanes(s, w);
  if ((w & 0x9f200c00u) == 0x0e200000u) return ThreeDifferent(s, w);
  if ((w & 0xff3e0c00u) == 0x5e300800u) return ScalarPairwise(s, w);
  if ((w & 0xbf200000u) == 0x0c000000u && (Bits(w, 23, 23) || !Bits(w, 20, 16))) return LoadStoreMultiple(s, w);
  if ((w & 0x7f200000u) == 0x1e200000u && Bits(w, 15, 10) == 0) return FpToIntGroup(s, w);
  if ((w & 0x7f200000u) == 0x1e000000u) return FpToIntGroup(s, w);
  if ((w & 0xff207c00u) == 0x1e204000u || (w & 0xff207c00u) == 0x1e604000u) return FpRoundScalar(s, w);
  if ((w & 0xff200c00u) == 0x1e200400u || (w & 0xff200c00u) == 0x1e600400u) return FCCmp(s, w);
  if ((w & 0xff200c00u) == 0x1e200800u || (w & 0xff200c00u) == 0x1e600800u) return FpTwoSource(s, w);
  if ((w & 0x3f200c00u) == 0x38000000u) return LoadUnscaledSigned(s, w);
  return false;
}

}  // namespace recomp::aarch64_fallback
