#pragma once
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace bunker::windows {
// Keep imported emulator preferences and its device ID together. Normal play
// uses the package's existing registry key; a benchmark can select a copy.
inline std::wstring WindowsAppRegistryPath(const std::u16string &package) {
  std::wstring path = L"Software\\Bunker\\Apps\\";
  path.append(package.begin(), package.end());
  const char *profile = std::getenv("REINCOMP_BENCH_PROFILE");
  if (!profile || !*profile) return path;
  const std::string name(profile);
  if (name.size() > 64 || name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos)
    throw std::invalid_argument("invalid benchmark profile name");
  path += L"\\BenchmarkProfiles\\";
  path.append(name.begin(), name.end());
  return path;
}
}
