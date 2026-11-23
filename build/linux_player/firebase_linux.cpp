#include "firebase_app.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <openssl/evp.h>
#include <stdexcept>

namespace {
void verify(const std::string &path, const std::string &expected) {
    std::ifstream file(path, std::ios::binary);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!file || !hash || !EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr))
        throw std::runtime_error("could not read Firebase SDK for verification");
    std::array<char, 65536> block;
    while (file.read(block.data(), block.size()) || file.gcount()) {
        if (!EVP_DigestUpdate(hash.get(), block.data(), file.gcount()))
            throw std::runtime_error("could not hash Firebase SDK");
    }
    std::array<unsigned char, 32> bytes{};
    unsigned size = 0;
    if (!file.eof() || !EVP_DigestFinal_ex(hash.get(), bytes.data(), &size) || size != bytes.size())
        throw std::runtime_error("could not verify complete Firebase SDK file");
    std::string hex;
    for (auto byte : bytes) {
        constexpr char digits[] = "0123456789abcdef";
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    if (hex != expected) throw std::runtime_error("Firebase SDK differs from the pinned Linux 9.6.0 plugin");
}
}

std::unique_ptr<bunker::windows::FirebaseAppService> linuxFirebase(
        const std::filesystem::path &library, bunker::windows::FirebaseAppService::Runner runner,
        std::function<bool()> admitted, std::function<bool(addr_t)> translated) {
    bunker::windows::FirebaseAppService::Config config;
    config.dll_path = library.string();
    config.verify_library = verify;
    config.runner = std::move(runner);
    config.admitted_thread = std::move(admitted);
    config.translated_entry = std::move(translated);
    return std::make_unique<bunker::windows::FirebaseAppService>(std::move(config));
}
