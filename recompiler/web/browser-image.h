#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

// Each shard normally reads a small contiguous range from a large ELF image.
// Keeping the file on WORKERFS avoids a full image read per shard.
class BrowserImage {
    static constexpr size_t pageSize = 65536;
    std::ifstream file;
    size_t fileSize = 0;
    size_t page = std::numeric_limits<size_t>::max();
    std::array<uint8_t,pageSize> window;
    std::vector<uint8_t> literal;
public:
    void open(const std::string &name) {
        file.open(name, std::ios::binary | std::ios::ate);
        const auto end = file.tellg();
        if (end <= 0) throw std::runtime_error("Empty or unreadable game image");
        fileSize = static_cast<size_t>(end);
    }
    void reserve(size_t n) { literal.reserve(n); }
    void push_back(uint8_t b) { literal.push_back(b); }
    size_t size() const { return fileSize ? fileSize : literal.size(); }
    uint8_t operator[](size_t offset) {
        if (!fileSize) return literal.at(offset);
        const size_t base = offset - offset % pageSize;
        if (base != page) {
            file.seekg(base);
            if (!file.read(reinterpret_cast<char *>(window.data()), std::min(pageSize, fileSize-base)))
                throw std::runtime_error("Could not read the game image");
            page = base;
        }
        return window[offset-base];
    }
};
