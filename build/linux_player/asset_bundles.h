#pragma once
#include "runtime.h"
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <sys/types.h>

class LinuxAssetBundles {
    std::mutex mutex;
    std::filesystem::path script;
    int channel = -1;
    pid_t child = -1;
    void start();
    void disconnect();
    std::string convert(const std::vector<char> &bytes, uint32_t crc);
public:
    ~LinuxAssetBundles();
    void setWorker(std::filesystem::path path) { script = std::move(path); }
    using FindNative = std::function<void *(const char *)>;
    std::function<void(State &)> bind(const char *name, void *native, FindNative find);
};
