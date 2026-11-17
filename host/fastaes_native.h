#pragma once
// Host implementation of the game's libFastAES.so P/Invoke exports
// (FastAES.dll: AES_CBC_decrypt / AES_CBC_encrypt). The ARM64 library is not in
// the translated module catalog, so il2cpp's dlopen("FastAES") resolves to this
// typed service. Semantics follow the original disassembly and
// work/windows-fastaes/ref.txt:
//   decrypt(out,in,len,key,key_len,iv,padding): copy, CBC-decrypt len/16 whole
//     blocks; padding==1 (None) returns len, otherwise a valid PKCS7 tail
//     (n in 1..16, last n bytes == n) returns len-n, an invalid one len.
//   encrypt(out,unused,in,len,key,key_len,iv,padding): padding==2 (PKCS7)
//     pads to the next block; any other value encrypts len/16 whole blocks.
// key_len is in bytes (16/24/32). Portable C++; no platform crypto.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "../runtime/guest_memory.h"
#include "../runtime/runtime.h"
#include "module_registry.h"

namespace bunker::windows::fastaes_native {

namespace detail {
inline const uint8_t *Sbox() {
  static const std::array<uint8_t, 256> box = [] {
    std::array<uint8_t, 256> s{};
    uint8_t p = 1, q = 1;
    do {
      p = p ^ uint8_t(p << 1) ^ uint8_t((p & 0x80) ? 0x1b : 0);
      q ^= q << 1; q ^= q << 2; q ^= q << 4; if (q & 0x80) q ^= 0x09;
      const uint8_t x = q ^ uint8_t((q << 1) | (q >> 7)) ^ uint8_t((q << 2) | (q >> 6)) ^
                        uint8_t((q << 3) | (q >> 5)) ^ uint8_t((q << 4) | (q >> 4));
      s[p] = x ^ 0x63;
    } while (p != 1);
    s[0] = 0x63;
    return s;
  }();
  return box.data();
}
inline const uint8_t *InvSbox() {
  static const std::array<uint8_t, 256> box = [] {
    std::array<uint8_t, 256> inv{};
    for (int i = 0; i < 256; ++i) inv[Sbox()[i]] = uint8_t(i);
    return inv;
  }();
  return box.data();
}
inline uint8_t Xtime(uint8_t x) { return uint8_t((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
inline uint8_t Mul(uint8_t a, uint8_t b) {
  uint8_t r = 0;
  while (b) { if (b & 1) r ^= a; a = Xtime(a); b >>= 1; }
  return r;
}
struct MulTables {
  uint8_t m9[256], m11[256], m13[256], m14[256];
  MulTables() { for (int i = 0; i < 256; ++i) { m9[i] = Mul(uint8_t(i), 9); m11[i] = Mul(uint8_t(i), 11); m13[i] = Mul(uint8_t(i), 13); m14[i] = Mul(uint8_t(i), 14); } }
};
inline const MulTables &Tables() { static const MulTables t; return t; }

class Aes {
 public:
  // Returns false for an unsupported key length.
  bool SetKey(const uint8_t *key, uint32_t length) {
    if (length != 16 && length != 24 && length != 32) return false;
    const int nk = int(length / 4);
    rounds_ = nk + 6;
    const int words = 4 * (rounds_ + 1);
    std::memcpy(schedule_, key, length);
    uint8_t rcon = 1;
    for (int i = nk; i < words; ++i) {
      uint8_t t[4];
      std::memcpy(t, schedule_ + 4 * (i - 1), 4);
      if (i % nk == 0) {
        const uint8_t first = t[0];
        t[0] = uint8_t(Sbox()[t[1]] ^ rcon); t[1] = Sbox()[t[2]]; t[2] = Sbox()[t[3]]; t[3] = Sbox()[first];
        rcon = Xtime(rcon);
      } else if (nk > 6 && i % nk == 4) {
        for (auto &b : t) b = Sbox()[b];
      }
      for (int j = 0; j < 4; ++j) schedule_[4 * i + j] = uint8_t(schedule_[4 * (i - nk) + j] ^ t[j]);
    }
    return true;
  }
  void EncryptBlock(uint8_t *s) const {
    AddRoundKey(s, 0);
    for (int round = 1; round <= rounds_; ++round) {
      for (int i = 0; i < 16; ++i) s[i] = Sbox()[s[i]];
      ShiftRows(s);
      if (round != rounds_)
        for (int c = 0; c < 4; ++c) {
          uint8_t *col = s + 4 * c;
          const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3], all = a0 ^ a1 ^ a2 ^ a3;
          col[0] ^= all ^ Xtime(a0 ^ a1); col[1] ^= all ^ Xtime(a1 ^ a2);
          col[2] ^= all ^ Xtime(a2 ^ a3); col[3] ^= all ^ Xtime(a3 ^ a0);
        }
      AddRoundKey(s, round);
    }
  }
  void DecryptBlock(uint8_t *s) const {
    const auto &t = Tables();
    AddRoundKey(s, rounds_);
    for (int round = rounds_ - 1; round >= 0; --round) {
      InvShiftRows(s);
      for (int i = 0; i < 16; ++i) s[i] = InvSbox()[s[i]];
      AddRoundKey(s, round);
      if (round)
        for (int c = 0; c < 4; ++c) {
          uint8_t *col = s + 4 * c;
          const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
          col[0] = t.m14[a0] ^ t.m11[a1] ^ t.m13[a2] ^ t.m9[a3];
          col[1] = t.m9[a0] ^ t.m14[a1] ^ t.m11[a2] ^ t.m13[a3];
          col[2] = t.m13[a0] ^ t.m9[a1] ^ t.m14[a2] ^ t.m11[a3];
          col[3] = t.m11[a0] ^ t.m13[a1] ^ t.m9[a2] ^ t.m14[a3];
        }
    }
  }

 private:
  void AddRoundKey(uint8_t *s, int round) const { for (int i = 0; i < 16; ++i) s[i] ^= schedule_[16 * round + i]; }
  static void ShiftRows(uint8_t *s) {
    uint8_t t[16];
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) t[4 * c + r] = s[4 * ((c + r) & 3) + r];
    std::memcpy(s, t, 16);
  }
  static void InvShiftRows(uint8_t *s) {
    uint8_t t[16];
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) t[4 * ((c + r) & 3) + r] = s[4 * c + r];
    std::memcpy(s, t, 16);
  }
  uint8_t schedule_[240]{};
  int rounds_ = 0;
};

constexpr uint8_t kPaddingNone = 1, kPaddingPkcs7 = 2;

// Pure host functions (also used by tests). Return value as the original.
inline uint32_t CbcDecrypt(std::vector<uint8_t> &data, const uint8_t *key, uint32_t key_length, const uint8_t iv[16], uint8_t padding) {
  Aes aes;
  if (!aes.SetKey(key, key_length)) throw std::invalid_argument("FastAES: unsupported key length " + std::to_string(key_length));
  const uint32_t length = uint32_t(data.size());
  uint8_t chain[16], saved[16];
  std::memcpy(chain, iv, 16);
  for (uint32_t at = 0; at + 16 <= length; at += 16) {
    uint8_t *block = data.data() + at;
    std::memcpy(saved, block, 16);
    aes.DecryptBlock(block);
    for (int i = 0; i < 16; ++i) block[i] ^= chain[i];
    std::memcpy(chain, saved, 16);
  }
  if (padding == kPaddingNone || length < 16 || (length & 15)) return length;
  const uint8_t pad = data[length - 1];
  if (pad < 1 || pad > 16) return length;
  for (uint32_t i = 0; i < pad; ++i) if (data[length - 1 - i] != pad) return length;
  return length - pad;
}
inline std::vector<uint8_t> CbcEncrypt(const uint8_t *input, uint32_t length, const uint8_t *key, uint32_t key_length, const uint8_t iv[16], uint8_t padding) {
  Aes aes;
  if (!aes.SetKey(key, key_length)) throw std::invalid_argument("FastAES: unsupported key length " + std::to_string(key_length));
  std::vector<uint8_t> data;
  if (padding != kPaddingPkcs7) data.assign(input, input + (length & ~15u));
  else {
    const uint8_t pad = uint8_t(16 - (length & 15));
    data.assign(input, input + length);
    data.insert(data.end(), pad, pad);
  }
  uint8_t chain[16];
  std::memcpy(chain, iv, 16);
  for (size_t at = 0; at < data.size(); at += 16) {
    uint8_t *block = data.data() + at;
    for (int i = 0; i < 16; ++i) block[i] ^= chain[i];
    aes.EncryptBlock(block);
    std::memcpy(chain, block, 16);
  }
  return data;
}
}  // namespace detail

class Service {
 public:
  static constexpr addr_t kMarkerBase = 0x7fec00000000ULL;
  static constexpr addr_t kDecrypt = kMarkerBase, kEncrypt = kMarkerBase + 8;

  Module MakeModule() const {
    Module module;
    module.name = "libFastAES.so";
    module.aliases = {"FastAES", "FastAES.so", "libFastAES", "/data/app/bunker/lib/arm64/libFastAES.so"};
    module.kind = Module::Kind::TypedService;
    module.exports.emplace("AES_CBC_decrypt", kDecrypt);
    module.exports.emplace("AES_CBC_encrypt", kEncrypt);
    module.reset_image = [] {};
    return module;
  }

  void Install(recomp::Runtime &runtime) {
    runtime.AddImport(kDecrypt, [this](State &s) { Guard("AES_CBC_decrypt", [&] { Decrypt(s); }); });
    runtime.AddImport(kEncrypt, [this](State &s) { Guard("AES_CBC_encrypt", [&] { Encrypt(s); }); });
  }

 private:
  template <class F> void Guard(const char *name, F &&body) {
    try { body(); } catch (const std::exception &error) {
      std::fprintf(stderr, "FastAES native: %s failed: %s\n", name, error.what());
      std::fflush(stderr);
      std::abort();
    }
  }
  static std::vector<uint8_t> Read(addr_t address, size_t length) {
    std::vector<uint8_t> bytes(length);
    if (length) recomp::ReadGuestMemory(bytes.data(), address, length);
    return bytes;
  }
  void Log(const char *name, uint32_t length, uint32_t key_length, uint8_t padding, uint32_t result) {
    const auto n = calls_++;
    if (n < 8 || (n & (n - 1)) == 0)
      std::fprintf(stderr, "FastAES native: %s len=%u key_len=%u padding=%u -> %u (call %llu)\n", name, length, key_length, padding, result, (unsigned long long)n + 1);
  }
  // AES_CBC_decrypt(out, in, len, key, key_len, iv, padding)
  void Decrypt(State &s) {
    const addr_t out = s.gpr.x0.qword, in = s.gpr.x1.qword, key = s.gpr.x3.qword, iv = s.gpr.x5.qword;
    const uint32_t length = s.gpr.x2.dword, key_length = s.gpr.x4.dword;
    const uint8_t padding = uint8_t(s.gpr.x6.dword);
    if (!key || !iv || (length && (!out || !in))) throw std::invalid_argument("null buffer");
    auto data = Read(in, length);
    const auto key_bytes = Read(key, key_length);
    const auto iv_bytes = Read(iv, 16);
    const uint32_t result = detail::CbcDecrypt(data, key_bytes.data(), key_length, iv_bytes.data(), padding);
    if (length) recomp::WriteGuestMemory(out, data.data(), length);
    Log("AES_CBC_decrypt", length, key_length, padding, result);
    s.gpr.x0.qword = result;
  }
  // AES_CBC_encrypt(out, unused, in, len, key, key_len, iv, padding)
  void Encrypt(State &s) {
    const addr_t out = s.gpr.x0.qword, in = s.gpr.x2.qword, key = s.gpr.x4.qword, iv = s.gpr.x6.qword;
    const uint32_t length = s.gpr.x3.dword, key_length = s.gpr.x5.dword;
    const uint8_t padding = uint8_t(s.gpr.x7.dword);
    if (!key || !iv || !out || (length && !in)) throw std::invalid_argument("null buffer");
    const auto input = Read(in, length);
    const auto key_bytes = Read(key, key_length);
    const auto iv_bytes = Read(iv, 16);
    const auto data = detail::CbcEncrypt(input.data(), length, key_bytes.data(), key_length, iv_bytes.data(), padding);
    if (!data.empty()) recomp::WriteGuestMemory(out, data.data(), data.size());
    Log("AES_CBC_encrypt", length, key_length, padding, uint32_t(data.size()));
    s.gpr.x0.qword = data.size();
  }
  std::atomic<uint64_t> calls_{0};
};

}  // namespace bunker::windows::fastaes_native
