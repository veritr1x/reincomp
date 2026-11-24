// The APK's AVPro factory has no Linux backend. Keep its managed interface
// and event loop, and supply a Unity VideoPlayer on the same GameObject.
#include "bridge.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
namespace {
namespace fs = std::filesystem;
using P = void *;
extern "C" {
P il2cpp_resolve_icall(const char *);
P il2cpp_domain_get();
P il2cpp_domain_assembly_open(P, const char *);
P il2cpp_assembly_get_image(P);
P il2cpp_object_get_class(P);
P il2cpp_class_get_image(P);
P il2cpp_class_from_name(P, const char *, const char *);
P il2cpp_class_get_type(P);
P il2cpp_type_get_object(P);
P il2cpp_object_new(P);
void il2cpp_runtime_object_init(P);
uint32_t il2cpp_gchandle_new(P, bool);
P il2cpp_gchandle_get_target(uint32_t);
void il2cpp_gchandle_free(uint32_t);
P il2cpp_string_new(const char *);
const char16_t *il2cpp_string_chars(P);
int il2cpp_string_length(P);
}

template <class F> F engine(const char *name) {
    static std::map<std::string_view, F> functions;
    auto &function = functions[name];
    if (!function)
        function = reinterpret_cast<F>(il2cpp_resolve_icall(name));
    if (!function)
        throw std::runtime_error(std::string("Missing Unity movie binding: ") + name);
    return function;
}
#define VIDEO(name, signature) engine<signature>("UnityEngine.Video.VideoPlayer::" #name)

fs::path pathFromString(P value) {
    return value ? fs::path(std::u16string(il2cpp_string_chars(value), il2cpp_string_length(value)))
                 : fs::path{};
}
const fs::path *streaming;
struct Movie {
    uint32_t component = 0;
    fs::path cache, temporary;
    pid_t encoder = -1;
    bool played = false, playing = false, paused = false, finished = false, failed = false;
    bool muted = false;
    float volume = 1;
    P video() const { return il2cpp_gchandle_get_target(component); }
    void stopEncoder() {
        if (encoder > 0) {
            kill(encoder, SIGTERM);
            while (waitpid(encoder, nullptr, 0) < 0 && errno == EINTR) {
            }
            encoder = -1;
        }
        if (!temporary.empty()) {
            std::error_code ec;
            fs::remove(temporary, ec);
            temporary.clear();
        }
    }
    void close() {
        stopEncoder();
        VIDEO(Stop, void (*)(P))(video());
        played = playing = paused = finished = failed = false;
        cache.clear();
    }
    void prepare() {
        auto v = video();
        VIDEO(set_url, void (*)(P, P))(v, il2cpp_string_new(cache.c_str()));
        VIDEO(Prepare, void (*)(P))(v);
    }
    void update() {
        if (encoder > 0) {
            int status = 0;
            auto result = waitpid(encoder, &status, WNOHANG);
            if (!result || (result < 0 && errno == EINTR))
                return;
            encoder = -1;
            std::error_code ec;
            if (result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0)
                fs::rename(temporary, cache, ec);
            else
                failed = true;
            if (ec)
                failed = true;
            stopEncoder();
            if (failed) {
                std::fprintf(stderr, "Could not convert the story movie for Unity: %s\n", cache.c_str());
                return;
            }
            prepare();
        }
        if (played && !paused) {
            const bool active = VIDEO(get_isPlaying, uint8_t (*)(P))(video());
            if (playing && !active)
                finished = true;
            playing = active;
        }
    }
    bool open(P filename) {
        close();
        try {
            static const auto storage = fs::canonical(
                pathFromString(engine<P (*)()>("UnityEngine.Application::get_persistentDataPath")()));
            auto name = pathFromString(filename).string();
            const auto jar = name.find("!/assets/");
            fs::path path = jar != name.npos && name.rfind("jar:file:", 0) == 0
                                ? *streaming / name.substr(jar + 9)
                                : fs::path(name.rfind("file://", 0) == 0 ? name.substr(7) : name);
            path = fs::canonical(path);
            auto rel = path.lexically_relative(storage);
            fs::path kind = "storage";
            if (rel.empty() || *rel.begin() == "..") {
                kind = "packaged";
                rel = path.lexically_relative(fs::canonical(*streaming));
            }
            if (rel.empty() || *rel.begin() == ".." || !fs::is_regular_file(path))
                throw std::runtime_error("not a local game movie");
            cache = storage / "reincomp-movies-v1" / kind / rel;
            cache += ".webm";
            if (fs::is_regular_file(cache) && fs::file_size(cache) &&
                fs::last_write_time(cache) >= fs::last_write_time(path)) {
                prepare();
                return true;
            }
            fs::create_directories(cache.parent_path());
            std::string tmp = cache.string() + ".XXXXXX";
            int fd = mkstemp(tmp.data());
            if (fd < 0)
                throw std::runtime_error(std::strerror(errno));
            ::close(fd);
            temporary = tmp;
            // Unity 2019's native Linux decoder accepts VP8/Vorbis WebM.
            const char *args[] = {
                "ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", path.c_str(),
                "-map", "0:v:0", "-map", "0:a:0?",
                "-c:v", "libvpx", "-deadline", "realtime", "-cpu-used", "8", "-threads", "4",
                "-b:v", "4M", "-pix_fmt", "yuv420p", "-c:a", "libvorbis", "-q:a", "4",
                "-f", "webm", temporary.c_str(), nullptr
            };
            pid_t child;
            int err = posix_spawnp(&child, args[0], nullptr, nullptr, const_cast<char **>(args), environ);
            if (err == ENOENT)
                throw std::runtime_error("install FFmpeg with the VP8 and Vorbis encoders to play story movies");
            if (err)
                throw std::runtime_error(std::string("starting ffmpeg: ") + std::strerror(err));
            encoder = child;
            return true;
        } catch (const std::exception &e) {
            std::fprintf(stderr, "Could not open the Linux story movie %s: %s\n",
                         pathFromString(filename).c_str(), e.what());
            stopEncoder();
            failed = true;
            return false;
        }
    }
};
std::map<P, Movie> players;
Movie &movie(P self) { return players.at(self); }
P create(P owner) {
    static auto backend = il2cpp_class_from_name(il2cpp_class_get_image(il2cpp_object_get_class(owner)),
                                                 "RenderHeads.Media.AVProVideo", "NullMediaPlayer");
    static auto type = [] {
        auto module = il2cpp_domain_assembly_open(il2cpp_domain_get(), "UnityEngine.VideoModule");
        auto image = il2cpp_assembly_get_image(module);
        auto player = il2cpp_class_from_name(image, "UnityEngine.Video", "VideoPlayer");
        return il2cpp_type_get_object(il2cpp_class_get_type(player));
    }();
    auto game = engine<P (*)(P)>("UnityEngine.Component::get_gameObject")(owner);
    auto video = engine<P (*)(P, P)>("UnityEngine.GameObject::Internal_AddComponentWithType")(game, type);
    VIDEO(set_playOnAwake, void (*)(P, uint8_t))(video, false);
    VIDEO(set_renderMode, void (*)(P, int))(video, 4);      // APIOnly: AVPro's DisplayUGUI draws the texture.
    VIDEO(set_audioOutputMode, void (*)(P, int))(video, 2); // Direct audio
    auto self = il2cpp_object_new(backend);
    il2cpp_runtime_object_init(self);
    players[self].component = il2cpp_gchandle_new(video, false);
    return self;
}
template <class Sig, class F> void bind(addr_t pc, F fn) {
    recomp::AddGuestOverride(pc, recomp::MakeBridge<Sig>(fn));
}
} // namespace

void installLinuxMoviePlayer(const std::filesystem::path &streamingAssets) {
    streaming = &streamingAssets;
    // Global 3.7.1 AVPro MediaPlayer.CreatePlatformMediaPlayer and the
    // NullMediaPlayer implementation of IMediaControl, IMediaInfo and ITextureProducer.
    bind<P(P)>(0x8391af44, create);
    bind<uint8_t(P, P)>(0x840b3fe0, [](P s, P path) { return movie(s).open(path); });
    bind<void(P)>(0x840b4100, [](P s) { movie(s).close(); });
    bind<void(P)>(0x840b427c, [](P s) { movie(s).update(); });
    bind<void(P)>(0x840b43e4, [](P s) {
        auto it = players.find(s);
        if (it == players.end())
            return;
        it->second.close();
        engine<void (*)(P, float)>("UnityEngine.Object::Destroy")(it->second.video(), 0);
        il2cpp_gchandle_free(it->second.component);
        players.erase(it);
    });
    auto ready = [](P s) { return VIDEO(get_isPrepared, uint8_t (*)(P))(movie(s).video()); };
    bind<uint8_t(P)>(0x840b4154, ready);
    bind<uint8_t(P)>(0x840b415c, ready);
    bind<uint8_t(P)>(0x840b4164, ready);
    bind<void(P)>(0x840b416c, [](P s) {
        auto &m = movie(s);
        m.played = true;
        m.paused = m.finished = false;
        VIDEO(Play, void (*)(P))(m.video());
    });
    bind<void(P)>(0x840b417c, [](P s) {
        auto &m = movie(s);
        m.paused = true;
        VIDEO(Pause, void (*)(P))(m.video());
    });
    bind<void(P)>(0x840b4188, [](P s) {
        auto &m = movie(s);
        m.played = m.playing = false;
        VIDEO(Stop, void (*)(P))(m.video());
    });
    bind<uint8_t(P)>(0x840b4198, [](P s) { return VIDEO(get_isPlaying, uint8_t (*)(P))(movie(s).video()); });
    bind<uint8_t(P)>(0x840b41a0, [](P s) { return movie(s).paused; });
    bind<uint8_t(P)>(0x840b41a8, [](P s) { return movie(s).finished || movie(s).failed; });
    bind<float(P)>(0x840b41f4,
                   [](P s) { return 1000.0 * VIDEO(get_length, double (*)(P))(movie(s).video()); });
    bind<float(P)>(0x840b4240, [](P s) { return 1000.0 * VIDEO(get_time, double (*)(P))(movie(s).video()); });
    bind<int32_t(P)>(0x840b4200, [](P s) { return VIDEO(get_width, uint32_t (*)(P))(movie(s).video()); });
    bind<int32_t(P)>(0x840b4208, [](P s) { return VIDEO(get_height, uint32_t (*)(P))(movie(s).video()); });
    auto rate = [](P s) { return VIDEO(get_frameRate, float (*)(P))(movie(s).video()); };
    bind<float(P)>(0x840b4210, rate);
    bind<float(P)>(0x840b4274, rate);
    bind<P(P, int32_t)>(0x840b4218, [](P s, int32_t index) -> P {
        return index ? nullptr : VIDEO(get_texture, P (*)(P))(movie(s).video());
    });
    bind<int32_t(P)>(0x840b4220, [](P s) {
        return std::max<int64_t>(0, 1 + VIDEO(get_frame, int64_t (*)(P))(movie(s).video()));
    });
    auto seek = [](P s, float ms) { VIDEO(set_time, void (*)(P, double))(movie(s).video(), ms / 1000.0); };
    bind<void(P, float)>(0x840b4230, seek);
    bind<void(P, float)>(0x840b4238, seek);
    bind<void(P, uint8_t)>(
        0x840b4140, [](P s, uint8_t b) { VIDEO(set_isLooping, void (*)(P, uint8_t))(movie(s).video(), b); });
    bind<uint8_t(P)>(0x840b414c, [](P s) { return VIDEO(get_isLooping, uint8_t (*)(P))(movie(s).video()); });
    bind<void(P, float)>(
        0x840b4248, [](P s, float v) { VIDEO(set_playbackSpeed, void (*)(P, float))(movie(s).video(), v); });
    bind<float(P)>(0x840b4250, [](P s) { return VIDEO(get_playbackSpeed, float (*)(P))(movie(s).video()); });
    bind<void(P, uint8_t)>(0x840b4258, [](P s, uint8_t b) {
        auto &m = movie(s);
        m.muted = b;
        VIDEO(SetDirectAudioMute, void (*)(P, uint16_t, uint8_t))(m.video(), 0, b);
    });
    bind<uint8_t(P)>(0x840b425c, [](P s) { return movie(s).muted; });
    bind<void(P, float)>(0x840b4264, [](P s, float v) {
        auto &m = movie(s);
        m.volume = v;
        VIDEO(SetDirectAudioVolume, void (*)(P, uint16_t, float))(m.video(), 0, v);
    });
    bind<float(P)>(0x840b426c, [](P s) { return movie(s).volume; });
}
