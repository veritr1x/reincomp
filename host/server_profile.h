#pragma once
// Private-server client profile for the Windows bootstrap (--server-profile).
//
// These are host equivalents of the Android build's client changes in
// upstream lunar-scripts android/patch_apk.py, applied without modifying the
// verified APK or ELF images:
//  - global-metadata.dat string literals point the game at the local backend
//    (patched in the copy Unity extracts to files/il2cpp, just before il2cpp
//    opens it; the APK and its SHA-256 stay unchanged);
//  - libil2cpp entry-point patches become guest function overrides
//    (recomp::AddGuestOverride), since lifted code cannot take byte patches.
// Mid-function patches from that script (in-app purchase flow, gRPC port
// immediates) are not representable as entry overrides; the gRPC port is
// handled where the channel target is created instead.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "../runtime/runtime.h"

namespace bunker::windows::server_profile {

struct Endpoints {
  std::string grpc_host = "127.0.0.1";       // gRPC game API host (port is separate)
  std::string web_url = "http://127.0.0.1:38080";  // assets/web HTTP base URL
};

using Replacements = std::vector<std::pair<std::string, std::string>>;

inline Replacements MetadataReplacements(const Endpoints &e) {
  return {
      {"api.app.nierreincarnation.com", e.grpc_host},
      {"https://web.app.nierreincarnation.com/assets/release/{0}/database.bin",
       e.web_url + "/assets/release/{0}/database.bin"},
      {"https://web.app.nierreincarnation.com", e.web_url},
      {"https://resources-api.app.nierreincarnation.com/", e.web_url + "/"},
  };
}

// IL2CPP metadata v24 string literals: a table of (length, dataIndex) pairs at
// header offset 8 and the UTF-8 data blob at header offset 16. Each
// replacement must be no longer than the original; the entry's length is
// shortened and the remaining bytes are zeroed. Returns how many literals were
// patched now. A literal that is already patched no longer exists and is
// skipped, so the operation is idempotent.
inline int PatchMetadataStrings(const std::filesystem::path &path, const Replacements &replacements) {
  std::vector<uint8_t> data;
  {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("metadata is not readable: " + path.u8string());
    data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  auto u32 = [&](size_t at) {
    if (at > data.size() || data.size() - at < 4) throw std::runtime_error("metadata header truncated");
    uint32_t value;
    std::memcpy(&value, data.data() + at, 4);
    return value;
  };
  if (u32(0) != 0xfab11bafu || u32(4) != 24) throw std::runtime_error("metadata is not IL2CPP v24");
  const uint64_t table = u32(8), table_size = u32(12), blob = u32(16), blob_size = u32(20);
  if (table + table_size > data.size() || blob + blob_size > data.size() || table_size % 8)
    throw std::runtime_error("metadata string literal sections out of range");
  int patched = 0;
  for (const auto &[old_text, new_text] : replacements) {
    if (new_text.size() > old_text.size()) throw std::invalid_argument("metadata replacement is longer than original: " + old_text);
    const auto begin = data.begin() + blob, end = data.begin() + blob + blob_size;
    // A literal can also occur as a prefix of a longer one; only an exact
    // table entry (same index and length) identifies the literal itself.
    for (auto at = std::search(begin, end, old_text.begin(), old_text.end()); at != end;
         at = std::search(at + 1, end, old_text.begin(), old_text.end())) {
      const uint32_t index = uint32_t(at - begin);
      bool found = false;
      for (uint64_t entry = table; entry < table + table_size; entry += 8) {
        if (u32(entry) == old_text.size() && u32(entry + 4) == index) {
          const uint32_t length = uint32_t(new_text.size());
          std::memcpy(data.data() + entry, &length, 4);
          found = true;
          break;
        }
      }
      if (!found) continue;
      std::copy(new_text.begin(), new_text.end(), at);
      std::fill(at + new_text.size(), at + old_text.size(), uint8_t(0));
      ++patched;
      break;
    }
  }
  if (patched) {
    auto temporary = path;
    temporary += ".bunker-tmp";
    {
      std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
      if (!out) throw std::runtime_error("metadata write failed: " + temporary.u8string());
    }
    std::filesystem::rename(temporary, path);
  }
  return patched;
}

struct Override {
  addr_t entry;
  const char *what;
  void (*apply)(State &);
};

// libil2cpp addresses are the upstream RVAs plus the 0x80000000 load base;
// the bootstrap verifies the libil2cpp SHA-256 these RVAs belong to.
inline const std::vector<Override> &EntryOverrides() {
  static const std::vector<Override> overrides = {
      {0x835c8670, "ToNativeCredentials returns null: insecure gRPC channel",
       [](State &s) { s.gpr.x0.qword = 0; }},
      {0x8279410c, "HandleNet.Encrypt returns the payload unchanged",
       [](State &s) { s.gpr.x0.qword = s.gpr.x1.qword; }},
      {0x8279420c, "HandleNet.Decrypt returns the received message unchanged",
       [](State &s) { s.gpr.x0.qword = s.gpr.x1.qword; }},
      {0x84c27038, "OctoManager.Internal.GetListAes returns false: plain list.bin",
       [](State &s) { s.gpr.x0.qword = 0; }},
      {0x8282ce78, "Purchaser.IsExistProduct returns true",
       [](State &s) { s.gpr.x0.qword = 1; }},
      // Unity IAP's Google Play module needs com.android.billingclient, which
      // has no host here: initialization never reaches OnInitialized or
      // OnInitializeFailed, so the awaited task never completes. Return a
      // default (already completed) UniTask {source=null, token=0} in x0/x1.
      {0x82830e68, "Purchaser.InitializeAndroid returns a completed UniTask: no store",
       [](State &s) { s.gpr.x0.qword = 0; s.gpr.x1.qword = 0; }},
      {0x82f11900, "TitleScreen.InitializeMenuButton returns: no end-of-service menu hiding",
       [](State &) {}},
  };
  return overrides;
}

inline void InstallEntryOverrides() {
  for (const auto &o : EntryOverrides()) {
    recomp::AddGuestOverride(o.entry, o.apply);
    std::fprintf(stderr, "Server profile: override %#llx %s\n", (unsigned long long)o.entry, o.what);
  }
  // The caller publishes once all overrides are registered.
}

}  // namespace bunker::windows::server_profile
