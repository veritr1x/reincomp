#pragma once
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>

namespace bunker::windows {
inline bool IsOctoPayload(const std::filesystem::path &path) {
  auto version = path.parent_path().parent_path().parent_path().parent_path();
  return _wcsicmp(version.filename().c_str(), L"v1") == 0 &&
         _wcsicmp(version.parent_path().filename().c_str(), L"octo") == 0 &&
         _wcsicmp(version.parent_path().parent_path().filename().c_str(), L"files") == 0;
}
// Octo's versioned payloads may share an NTFS file with the imported dump.
// A writable guest descriptor must never refer to the library's file.
inline int OpenGuestFile(const std::filesystem::path &path, int flags, int mode) {
  if ((flags & (_O_WRONLY | _O_RDWR | _O_TRUNC)) &&
      (flags & (_O_CREAT | _O_EXCL)) != (_O_CREAT | _O_EXCL) &&
      IsOctoPayload(path)) {
    std::error_code error;
    const auto links = std::filesystem::hard_link_count(path, error);
    if (error && error != std::errc::no_such_file_or_directory) {
      errno = EACCES;
      return -1;
    }
    if (!error && links > 1) {
      static std::atomic<unsigned long> serial{0};
      auto local = path;
      local += L".private-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++serial);
      if (flags & _O_TRUNC) {
        const int empty = _wopen(local.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
        if (empty < 0) return -1;
        _close(empty);
      } else if (!CopyFileW(path.c_str(), local.c_str(), TRUE)) {
        errno = EACCES;
        return -1;
      }
      if (!MoveFileExW(local.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(local.c_str());
        errno = EACCES;
        return -1;
      }
    }
  }
  return _wopen(path.c_str(), flags, mode);
}
}
