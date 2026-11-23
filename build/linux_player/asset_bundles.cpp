#include "asset_bundles.h"
#include "bridge.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <spawn.h>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;
using P = void *;
extern "C" {
P il2cpp_get_corlib();
P il2cpp_class_from_name(P, const char *, const char *);
P il2cpp_object_get_class(P);
P il2cpp_class_get_method_from_name(P, const char *, int);
P il2cpp_object_get_virtual_method(P, P);
P il2cpp_runtime_invoke(P, P, P *, P *);
P il2cpp_object_unbox(P);
P il2cpp_array_new(P, size_t);
uint32_t il2cpp_array_object_header_size();
uint32_t il2cpp_gchandle_new(P, bool);
void il2cpp_gchandle_free(uint32_t);
P il2cpp_string_new(const char *);
const char16_t *il2cpp_string_chars(P);
int32_t il2cpp_string_length(P);
}

namespace {
constexpr size_t maxBundleSize = 256U << 20;
using LoadFile = P (*)(P, uint32_t, uint64_t);
using LoadStream = P (*)(P, uint32_t, uint32_t);

bool unityBundle(const char *data, size_t size) {
    return size >= 8 && !std::memcmp(data, "UnityFS\0", 8);
}
// Read the game's decrypted Stream through its real managed methods. Unity
// still owns the stream and expects its original position on every return.
std::vector<char> readStream(P stream) {
    if (!stream) return {};
    P klass = il2cpp_object_get_class(stream);
    auto call = [&](const char *name, int count = 0, P *args = nullptr) {
        P method = il2cpp_class_get_method_from_name(klass, name, count);
        if (!method) throw std::runtime_error(name);
        P exception = nullptr;
        P value = il2cpp_runtime_invoke(il2cpp_object_get_virtual_method(stream, method), stream, args, &exception);
        if (exception) throw std::runtime_error(name);
        return value;
    };
    if (!*static_cast<uint8_t *>(il2cpp_object_unbox(call("get_CanSeek")))) return {};
    int64_t position = *static_cast<int64_t *>(il2cpp_object_unbox(call("get_Position")));
    int64_t length = *static_cast<int64_t *>(il2cpp_object_unbox(call("get_Length")));
    if (position || length < 8 || length > maxBundleSize) return {};
    P byte = il2cpp_class_from_name(il2cpp_get_corlib(), "System", "Byte");
    const int chunk = static_cast<int>(std::min<int64_t>(length, 256 * 1024));
    P array = il2cpp_array_new(byte, chunk);
    if (!array) throw std::bad_alloc();
    struct Pin {
        uint32_t handle;
        ~Pin() { il2cpp_gchandle_free(handle); }
    } pin{il2cpp_gchandle_new(array, true)};
    char *data = static_cast<char *>(array) + il2cpp_array_object_header_size();
    std::vector<char> bytes(static_cast<size_t>(length));
    P seek[] = {&position};
    try {
        size_t copied = 0;
        while (copied < bytes.size()) {
            int start = 0, want = static_cast<int>(std::min<size_t>(bytes.size() - copied, chunk));
            P args[] = {array, &start, &want};
            int got = *static_cast<int32_t *>(il2cpp_object_unbox(call("Read", 3, args)));
            if (got <= 0 || got > want) throw std::runtime_error("incomplete managed bundle stream");
            std::memcpy(bytes.data() + copied, data, got);
            copied += got;
            if (copied >= 8 && !unityBundle(bytes.data(), copied)) {
                bytes.clear();
                break;
            }
        }
    } catch (...) {
        call("set_Position", 1, seek);
        throw;
    }
    call("set_Position", 1, seek);
    return bytes;
}

std::vector<char> readFile(P path) {
    if (!path) return {};
    std::u16string name(il2cpp_string_chars(path), il2cpp_string_length(path));
    std::ifstream file(std::filesystem::path(name), std::ios::binary | std::ios::ate);
    auto size = file.tellg();
    if (size < 8 || size > maxBundleSize) return {};
    file.seekg(0);
    char header[8];
    if (!file.read(header, sizeof(header)) || !unityBundle(header, sizeof(header))) return {};
    std::vector<char> bytes(static_cast<size_t>(size));
    std::memcpy(bytes.data(),header,sizeof(header));
    if (!file.read(bytes.data()+sizeof(header), bytes.size()-sizeof(header))) return {};
    return bytes;
}

void transfer(int fd, void *buffer, size_t size, bool sending) {
    char *data = static_cast<char *>(buffer);
    while (size) {
        ssize_t n = sending ? send(fd, data, size, MSG_NOSIGNAL) : recv(fd, data, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("Linux bundle converter disconnected or timed out");
        data += n;
        size -= n;
    }
}
}

void LinuxAssetBundles::start() {
    const char *xdg = std::getenv("XDG_CACHE_HOME"), *home = std::getenv("HOME");
    if ((!xdg || !*xdg) && (!home || !*home)) throw std::runtime_error("Linux bundle cache directory is unavailable");
    auto cache = (xdg && *xdg ? std::filesystem::path(xdg) : std::filesystem::path(home) / ".cache") / "reincomp/linux-bundles-v1";
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair)) throw std::runtime_error("bundle converter socketpair failed");
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (!error) {
        error = posix_spawn_file_actions_adddup2(&actions, pair[1], STDIN_FILENO);
        if (!error) error = posix_spawn_file_actions_adddup2(&actions, pair[1], STDOUT_FILENO);
        char python[] = "python3";
        char *args[] = {python, const_cast<char *>(script.c_str()), const_cast<char *>(cache.c_str()), nullptr};
        if (!error) error = posix_spawnp(&child, python, &actions, nullptr, args, environ);
        posix_spawn_file_actions_destroy(&actions);
    }
    close(pair[1]);
    if (error) { close(pair[0]); child = -1; throw std::runtime_error(std::strerror(error)); }
    channel = pair[0];
    timeval timeout{90, 0};
    if (setsockopt(channel, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        setsockopt(channel, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) {
        disconnect(); throw std::runtime_error("could not set bundle converter timeout");
    }
}
void LinuxAssetBundles::disconnect() {
    if (channel >= 0) { close(channel); channel = -1; }
    if (child <= 0) return;
    // Closing the channel lets Python leave its cache directory cleanly.
    // Bound shutdown if a conversion has stopped responding.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (true) {
        pid_t result = waitpid(child, nullptr, WNOHANG);
        if (result == child || (result < 0 && errno != EINTR)) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGKILL);
            while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    child = -1;
}
LinuxAssetBundles::~LinuxAssetBundles() { disconnect(); }

std::string LinuxAssetBundles::convert(const std::vector<char> &bytes, uint32_t crc) {
    if (bytes.empty()) return {};
    std::lock_guard guard(mutex);
    if (channel < 0) start();
    uint64_t length = bytes.size();
    uint32_t reply[2];
    std::string message;
    try {
        transfer(channel, &length, sizeof(length), true);
        transfer(channel, &crc, sizeof(crc), true);
        transfer(channel, const_cast<char *>(bytes.data()), bytes.size(), true);
        transfer(channel, reply, sizeof(reply), false);
        if (reply[1] > 65536 || !reply[1]) throw std::runtime_error("invalid bundle converter reply");
        message.resize(reply[1]);
        transfer(channel, message.data(), message.size(), false);
    } catch (...) { disconnect(); throw; }
    if (reply[0]) throw std::runtime_error(message);
    return message;
}

std::function<void(State &)> LinuxAssetBundles::bind(const char *name, void *native, FindNative find) {
    std::string_view key(name);
    bool stream = key == "UnityEngine.AssetBundle::LoadFromStreamAsyncInternal" || key == "UnityEngine.AssetBundle::LoadFromStreamInternal";
    bool file = key == "UnityEngine.AssetBundle::LoadFromFileAsync_Internal" || key == "UnityEngine.AssetBundle::LoadFromFile_Internal";
    if (!native || (!stream && !file)) return {};
    bool async = key.find("Async") != std::string_view::npos;
    return [this, native, find, stream, async](State &s) {
        auto source = reinterpret_cast<P>(s.gpr.x0.qword);
        auto crc = s.gpr.x1.dword;
        auto offset = s.gpr.x2.qword;
        try {
            auto path = convert(stream ? readStream(source) : (offset ? std::vector<char>{} : readFile(source)), crc);
            if (!path.empty()) {
                auto target = stream ? find(async ? "UnityEngine.AssetBundle::LoadFromFileAsync_Internal" : "UnityEngine.AssetBundle::LoadFromFile_Internal") : native;
                if (!target) throw std::runtime_error("Unity did not register the bundle file loader");
                s.gpr.x0.qword = uintptr_t(reinterpret_cast<LoadFile>(target)(il2cpp_string_new(path.c_str()), 0, 0));
                return;
            }
        } catch (const std::exception &e) { std::fprintf(stderr, "Linux asset bundle: %s\n", e.what()); }
        s.gpr.x0.qword = uintptr_t(stream ? reinterpret_cast<LoadStream>(native)(source, crc, uint32_t(offset)) :
                                            reinterpret_cast<LoadFile>(native)(source, crc, offset));
    };
}
