#include "octo_http.h"
#include <future>
#include <map>
#include <limits>

namespace {
namespace http = bunker::windows::octo_http;
namespace fs = std::filesystem;
using P = void *;
extern "C" {
P il2cpp_domain_get();
P il2cpp_domain_assembly_open(P, const char *);
P il2cpp_assembly_get_image(P);
P il2cpp_class_from_name(P, const char *, const char *);
P il2cpp_class_get_nested_types(P, P *);
const char *il2cpp_class_get_name(P);
P il2cpp_object_new(P);
uint32_t il2cpp_gchandle_new(P, bool);
void il2cpp_gchandle_free(uint32_t);
void il2cpp_gc_wbarrier_set_field(P, P *, P);
P il2cpp_resolve_icall(const char *);
const char16_t *il2cpp_string_chars(P);
int32_t il2cpp_string_length(P);
P il2cpp_string_new(const char *);
}

fs::path managedPath(P value) {
    return value ? fs::path(std::u16string(il2cpp_string_chars(value), il2cpp_string_length(value))) : fs::path{};
}

fs::path storagePath() {
    static const auto getter = reinterpret_cast<P(*)()>(il2cpp_resolve_icall("UnityEngine.Application::get_persistentDataPath"));
    return managedPath(getter());
}

struct Types {
    P task = nullptr, result = nullptr;
    Types() {
        auto image = il2cpp_assembly_get_image(il2cpp_domain_assembly_open(il2cpp_domain_get(), "Octo"));
        auto request = il2cpp_class_from_name(image, "Octo.Network", "AndroidFileDownloadRequest");
        P iterator = nullptr;
        while (auto nested = il2cpp_class_get_nested_types(request, &iterator)) {
            std::string_view name(il2cpp_class_get_name(nested));
            if (name == "NativeHttpAsyncTask") task = nested;
            if (name == "NativeHttpResult") result = nested;
        }
        if (!task || !result) throw std::runtime_error("Octo file request types are missing");
    }
};

// Octo reads the Result backing field directly from its task. Keep the real
// managed objects alive until the coroutine disposes the task on the game thread.
P *resultField(P task) { return reinterpret_cast<P *>(static_cast<char *>(task) + 0x18); }
struct Operation {
    http::Download download;
    uint32_t root;
    std::future<void> worker;
    explicit Operation(P task) : root(il2cpp_gchandle_new(task, true)) {}
};
std::mutex operationsMutex;
std::map<P,std::shared_ptr<Operation>> operations;

void start(State &s, unsigned port) {
    static const Types types;
    auto url = managedPath(reinterpret_cast<P>(s.gpr.x0.qword)).u8string();
    auto path = managedPath(reinterpret_cast<P>(s.gpr.x1.qword));
    const auto root = fs::weakly_canonical(storagePath() / "octo");
    std::error_code error;
    auto destination = fs::weakly_canonical(path, error);
    auto relative = destination.lexically_relative(root);
    auto task = il2cpp_object_new(types.task);
    auto op = std::make_shared<Operation>(task);
    auto result = il2cpp_object_new(types.result);
    il2cpp_gc_wbarrier_set_field(task, resultField(task), result);
    {
        std::lock_guard lock(operationsMutex);
        operations.emplace(result, op);
    }
    s.gpr.x0.qword = reinterpret_cast<addr_t>(task);
    auto &download = op->download;
    if (error || !path.is_absolute() || relative.empty() || *relative.begin() == ".." ||
        url.rfind("http://127.0.0.1:" + std::to_string(port) + "/", 0) != 0) {
        download.io_error = download.complete = true;
        download.error_message = "Expected a local Bunker asset URL and an Octo cache destination";
        std::fprintf(stderr,"Linux Octo: rejected asset path %s (cache %s)\n",path.c_str(),root.c_str());
        return;
    }
    try {
        op->worker = std::async(std::launch::async, [&download, url, destination] {
            try {
                http::Service::Run(download, url, destination.string(), destination);
            } catch (const std::exception &error) {
                std::lock_guard lock(download.mutex);
                download.io_error = download.complete = true;
                download.error_message = error.what();
            }
        });
    } catch (const std::system_error &error) {
        download.io_error = download.complete = true;
        download.error_message = error.what();
    }
}

std::shared_ptr<Operation> find(P result) {
    std::lock_guard lock(operationsMutex);
    auto it = operations.find(result);
    return it == operations.end() ? nullptr : it->second;
}

template<class Getter> void property(addr_t pc, Getter get) {
    recomp::AddGuestOverride(pc, [get](State &s) {
        auto op = find(reinterpret_cast<P>(s.gpr.x0.qword));
        s.gpr.x0.qword = 0;
        if (!op) return;
        std::lock_guard lock(op->download.mutex);
        s.gpr.x0.qword = get(op->download);
    });
}

addr_t text(const std::optional<std::string> &value) {
    return value ? reinterpret_cast<addr_t>(il2cpp_string_new(value->c_str())) : 0;
}
}

void installLinuxOctoDownloads(const unsigned &portOffset) {
    // Leave Storage.CheckDownloadFileSize in charge of the required byte count.
    recomp::AddGuestOverride(0x8328fffc, [](State &s){
        std::error_code error;
        const auto info = fs::space(storagePath(), error);
        s.gpr.x0.qword = error ? 0 : std::min<uintmax_t>(info.available, std::numeric_limits<int64_t>::max());
    });
    recomp::AddGuestOverride(0x84c2dcc4, [](State &) {}); // Bridge initializes AndroidJavaClass.
    recomp::AddGuestOverride(0x84c2d6d8, [&portOffset](State &s){ start(s, 8080 + portOffset); });
    property(0x84c2d138, [](auto &d){ return d.content_length; });
    property(0x84c2d194, [](auto &d){ return d.written; });
    property(0x84c2d8b0, [](auto &d){ return d.complete; });
    property(0x84c2d90c, [](auto &d){ return d.status; });
    property(0x84c2d968, [](auto &d){ return text(d.error_message); });
    property(0x84c2d9c4, [](auto &d){ return d.io_error; });
    property(0x84c2da90, [](auto &d){ return d.io_error || d.error_code.has_value(); });
    property(0x84c2daec, [](auto &d){ return text(d.error_code); });
    property(0x84c2db48, [](auto &d){ return d.md5.empty() ? 0 : text(d.md5); });
    recomp::AddGuestOverride(0x84c2cfec, [](State &s){
        if (auto op = find(*resultField(reinterpret_cast<P>(s.gpr.x0.qword)))) op->download.abort = true;
    });
    recomp::AddGuestOverride(0x84c2decc, [](State &s){
        auto result = *resultField(reinterpret_cast<P>(s.gpr.x0.qword));
        std::shared_ptr<Operation> op;
        {
            std::lock_guard lock(operationsMutex);
            auto node = operations.extract(result);
            if (node.empty()) return;
            op = std::move(node.mapped());
        }
        op->download.abort = true;
        if (op->worker.valid()) op->worker.wait();
        il2cpp_gchandle_free(op->root);
    });
}
