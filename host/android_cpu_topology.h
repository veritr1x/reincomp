#pragma once
// Android CPU topology files for the guest (portable: std::filesystem only).
// Unity sizes its job-worker pool and chooses the threaded GfxDevice from
// /sys/devices/system/cpu (possible, cpuN/cpufreq/cpuinfo_max_freq,
// cpuN/cpu_capacity) and /proc/cpuinfo. Without them it counted at most one
// core: one job worker and rendering on UnityMain. The files describe the
// host's logical processors as equal (big) cores.
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace bunker::windows::cpu_topology {

inline unsigned Count() {
  const unsigned n = std::thread::hardware_concurrency();
  const unsigned available = std::clamp(n ? n : 8u, 2u, 16u);
  if (const char *setting = std::getenv("BUNKER_GUEST_CPU_COUNT")) {
    char *end = nullptr;
    const auto count = std::strtoul(setting, &end, 10);
    if (end != setting && !*end && count >= 2 && count <= available)
      return static_cast<unsigned>(count);
  }
  return available;  // guest affinity masks are 16 wide
}

inline void Write(const std::filesystem::path &file, const std::string &text) {
  std::filesystem::create_directories(file.parent_path());
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out << text;
}

// Writes the files under `root`; returns {sysfs cpu dir, cpuinfo file}.
inline std::pair<std::filesystem::path, std::filesystem::path> Build(const std::filesystem::path &root) {
  const unsigned n = Count();
  const auto cpu = root / ("sys-devices-system-cpu-" + std::to_string(n));
  const std::string range = "0-" + std::to_string(n - 1) + "\n";
  for (const char *name : {"possible", "present", "online"}) Write(cpu / name, range);
  std::string cpuinfo;
  for (unsigned i = 0; i < n; ++i) {
    const auto dir = cpu / ("cpu" + std::to_string(i));
    Write(dir / "cpufreq" / "cpuinfo_max_freq", "3000000\n");
    Write(dir / "cpufreq" / "cpuinfo_min_freq", "300000\n");
    Write(dir / "cpu_capacity", "1024\n");
    Write(dir / "online", "1\n");
    cpuinfo += "processor\t: " + std::to_string(i) + "\nBogoMIPS\t: 38.40\n"
               "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp\n"
               "CPU implementer\t: 0x41\nCPU architecture: 8\nCPU variant\t: 0x1\nCPU part\t: 0xd41\nCPU revision\t: 0\n\n";
  }
  cpuinfo += "Hardware\t: Bunker\n";
  const auto info = root / "proc-cpuinfo";
  Write(info, cpuinfo);
  return {cpu, info};
}

}  // namespace bunker::windows::cpu_topology
