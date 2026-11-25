#include "../../host/octo_cache_file.h"
#include <cassert>
#include <fstream>

int main(int argc, char **argv) {
  assert(argc == 2);
  namespace fs = std::filesystem;
  const fs::path dir = fs::u8path(argv[1]);
  const auto source = dir / "original", payload = dir / "files/octo/v1/301/1/4141/md5";
  fs::create_directories(payload.parent_path());
  std::ofstream(source) << "original";
  const int fresh = bunker::windows::OpenGuestFile(payload, _O_WRONLY | _O_CREAT | _O_TRUNC, _S_IREAD | _S_IWRITE);
  assert(fresh >= 0);
  _close(fresh);
  fs::remove(payload);
  for (const auto flags : {_O_WRONLY | _O_TRUNC, _O_RDWR | _O_APPEND}) {
    fs::create_hard_link(source, payload);
    assert(fs::equivalent(source, payload));
    assert(bunker::windows::OpenGuestFile(payload, _O_WRONLY | _O_CREAT | _O_EXCL, _S_IREAD | _S_IWRITE) < 0);
    assert(fs::equivalent(source, payload));
    auto fd = bunker::windows::OpenGuestFile(payload, flags | _O_BINARY, _S_IREAD | _S_IWRITE);
    assert(fd >= 0 && _write(fd, "new", 3) == 3);
    _close(fd);
    assert(!fs::equivalent(source, payload));
    assert(fs::file_size(payload) == ((flags & _O_APPEND) ? 11 : 3));
    std::string original;
    std::ifstream(source) >> original;
    assert(original == "original");
    fs::remove(payload);
  }
}
