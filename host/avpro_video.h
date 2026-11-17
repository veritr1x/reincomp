#pragma once
// AVPro Video (RenderHeads, 1.x) Android player for the game's story movies.
//
// The APK's Java side (com.RenderHeads.AVProVideo.AVProMobileVideo and the
// AVProVideoPlayer subclasses, classes.dex) drives Android MediaPlayer or
// ExoPlayer into an OES texture through libAVProLocal.so. Neither the Java
// classes nor libAVProLocal.so exist on this host. Without them the C#
// AndroidMediaPlayer throws ClassNotFoundException, the game's movie step
// never completes, and play stops on a black screen.
//
// This service supplies both halves. When the movie resolves to app storage
// and a decoder is available (video_decoder.h: Media Foundation on Windows),
// the video track plays: a decoder thread queues RGBA frames, a playback clock
// (pause/seek aware) drives the position, and on AVPro's PlayerUpdate render
// event (Unity's render thread, GL context current) the due frame is uploaded
// into a GL_TEXTURE_2D whose name _GetTextureHandle reports, as the Java
// player's non-OES path does. The movie finishes when the clock reaches its
// duration. No audio track is played.
// Without a decoder (other platforms, unresolvable path, decode failure) a
// movie has no video track: it reports its real duration (MP4 mvhd) or 1 s,
// plays for one Update() and then finishes, so AVPro raises Started and then
// FinishedPlaying and the game continues as it does when a movie ends on a
// device. Looping movies keep playing. Method names and descriptors are the APK's (dexdump of
// classes.dex). Unimplemented array/double methods (GetTextureTransform,
// GetSeekableTimeRange, GetCurrentAbsoluteTimestamp) are not declared, so a
// call fails as a Java NoSuchMethodError instead of returning invented data.
// Portable C++ apart from the decoder backend and the GL entry lookup.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../runtime/runtime.h"
#include "jni_host.h"
#include "module_registry.h"
#include "video_decoder.h"
#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace bunker::windows::avpro_video {

// Duration in milliseconds from an MP4/MOV movie header (mvhd), if present.
inline std::optional<int64_t> Mp4DurationMs(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::nullopt;
  std::vector<char> data(size_t(64) << 20);
  file.read(data.data(), std::streamsize(data.size()));
  data.resize(size_t(file.gcount()));
  auto be = [&](size_t at, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v = (v << 8) | uint8_t(data[at + i]);
    return v;
  };
  for (size_t at = 4; at + 32 <= data.size(); ++at) {
    if (std::memcmp(&data[at], "mvhd", 4)) continue;
    const size_t body = at + 4;  // version(1) flags(3)
    const bool v1 = data[body] == 1;
    const size_t scale_at = body + 4 + (v1 ? 16 : 8);
    if (scale_at + (v1 ? 12 : 8) > data.size()) return std::nullopt;
    const uint64_t scale = be(scale_at, 4), duration = be(scale_at + 4, v1 ? 8 : 4);
    if (!scale) return std::nullopt;
    return int64_t(duration * 1000 / scale);
  }
  return std::nullopt;
}

class Service {
 public:
  using PathResolver = std::function<std::optional<std::filesystem::path>(const std::string &)>;
  static constexpr addr_t kMarkerBase = 0x7feb00000000ULL;

  Service(jni::Host &java, PathResolver resolve) : java_(java), resolve_(std::move(resolve)) { Define(); }
  Service(const Service &) = delete;
  Service &operator=(const Service &) = delete;

  Module MakeModule() const {
    Module module;
    module.name = "libAVProLocal.so";
    module.aliases = {"AVProLocal", "AVProLocal.so", "libAVProLocal", "/data/app/bunker/lib/arm64/libAVProLocal.so"};
    module.kind = Module::Kind::TypedService;
    for (size_t i = 0; i < std::size(kNatives); ++i) module.exports.emplace(kNatives[i], kMarkerBase + 8 * i);
    module.reset_image = [] {};
    return module;
  }

  void Install(recomp::Runtime &runtime) {
    for (size_t i = 0; i < std::size(kNatives); ++i)
      runtime.AddImport(kMarkerBase + 8 * i, [this, i](State &s) { Native(i, s); });
  }

 private:
  static constexpr const char *kMobile = "com/RenderHeads/AVProVideo/AVProMobileVideo";
  static constexpr const char *kPlayer = "com/RenderHeads/AVProVideo/AVProVideoPlayer";
  static constexpr const char *kMediaPlayer = "com/RenderHeads/AVProVideo/AVProVideoMediaPlayer";
  static constexpr const char *kExoPlayer = "com/RenderHeads/AVProVideo/AVProVideoExoPlayer";
  // libAVProLocal.so exports used through P/Invoke and GL.IssuePluginEvent.
  static constexpr const char *kNatives[] = {
      "_GetWidth", "_GetHeight", "_GetTextureHandle", "_GetDuration", "_GetLastErrorCode",
      "_GetFrameCount", "_GetVideoDisplayRate", "_CanPlay", "GetRenderEventFunc", "FillAudioData",
      "UnityRenderEvent", "UnitySetGraphicsDevice", "OnGraphicsDeviceEvent",
      "Java_com_RenderHeads_AVProVideo_AVProMobileVideo_nativeInit"};
  static constexpr size_t kRenderEvent = 10;  // UnityRenderEvent: the GetRenderEventFunc target

  using Clock = std::chrono::steady_clock;
  struct Player {
    int index = 0;
    bool opened = false, playing = false, paused = false, finished = false, looping = false, muted = false;
    int updates_playing = 0;
    int64_t duration_ms = 0, position_ms = 0;
    uint32_t volume = 0x3f800000, pan = 0, rate = 0x3f800000;  // binary32 bits
    std::string path;
    // Decoded video (null: no video track).
    std::shared_ptr<video::Decoder> decoder;
    Clock::time_point started{};  // wall time matching position_ms while playing
    uint32_t texture = 0;         // GL name, created on the render thread
    int32_t frames = 0;           // uploaded frames
    int64_t Now() const {
      if (!playing) return position_ms;
      return position_ms + std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    }
  };
  struct Mobile {};

  std::shared_ptr<Player> Get(addr_t self) { return java_.ReadHostState<Player>(self); }
  std::shared_ptr<Player> ByIndex(int index) {
    std::lock_guard lock(mutex_);
    auto it = players_.find(index);
    return it == players_.end() ? nullptr : it->second;
  }
  static std::string Ascii(const std::u16string &s) {
    std::string out;
    for (char16_t c : s) out += c < 0x80 ? char(c) : '?';
    return out;
  }
  void Log(const Player &p, const char *what) {
    const auto n = logs_++;
    if (n < 64) std::fprintf(stderr, "AVPro host: player %d %s path=%s duration_ms=%lld\n", p.index, what, p.path.c_str(), (long long)p.duration_ms);
  }

  void Define() {
    using Args = jni::Host::MethodArguments;
    java_.DefineClass(kMobile);
    java_.DefineClass(kPlayer);
    java_.DefineClass(kMediaPlayer, {}, kPlayer);
    java_.DefineClass(kExoPlayer, {}, kPlayer);
    for (const char *name : {kMobile, kPlayer, kMediaPlayer, kExoPlayer}) java_.MarkClassInitialized(name);

    // AVProMobileVideo: the plugin interface object.
    java_.DefineConstructor(kMobile, "()V", [this](const Args &) { return java_.NewHostInstance(kMobile, std::make_shared<Mobile>()); });
    java_.DefineObjectMethod(kMobile, "GetPluginVersion", "()Ljava/lang/String;", [this](addr_t, const Args &) { return java_.NewString(u"1.x-host"); });
    java_.DefineVoidMethod(kMobile, "SetContext", "(Landroid/content/Context;)V", [](addr_t, const Args &) {});
    java_.DefineObjectMethod(kMobile, "CreatePlayer", "(IZZIZ)Lcom/RenderHeads/AVProVideo/AVProVideoPlayer;", [this](addr_t, const Args &a) -> addr_t {
      auto player = std::make_shared<Player>();
      {
        std::lock_guard lock(mutex_);
        player->index = next_index_++;
        players_[player->index] = player;
      }
      // Android.VideoApi: 1 = MediaPlayer, 2 = ExoPlayer.
      const bool exo = int32_t(a[0]) == 2;
      std::fprintf(stderr, "AVPro host: CreatePlayer index=%d api=%d (%s)\n", player->index, int32_t(a[0]), exo ? "ExoPlayer" : "MediaPlayer");
      return java_.NewHostInstance(exo ? kExoPlayer : kMediaPlayer, player);
    });
    java_.DefineObjectMethod(kMobile, "GetAVProClassForPlayerIndex", "(I)Lcom/RenderHeads/AVProVideo/AVProVideoPlayer;", [this](addr_t, const Args &a) -> addr_t {
      (void)a;
      return 0;  // Players are held by the C# side; the Java map is not mirrored.
    });
    auto remove = [this](addr_t, const Args &a) { std::lock_guard lock(mutex_); players_.erase(int32_t(a[0])); };
    java_.DefineVoidMethod(kMobile, "DestroyPlayer", "(I)V", remove);
    java_.DefineVoidMethod(kMobile, "RemovePlayer", "(I)V", remove);
    java_.DefineVoidMethod(kMobile, "Deinitialise", "()V", [](addr_t, const Args &) {});
    for (const char *name : {"RenderPlayer", "RendererReset", "WaitForNewFramePlayer"})
      java_.DefineVoidMethod(kMobile, name, "(I)V", [](addr_t, const Args &) {});
    java_.DefineVoidMethod(kMobile, "RendererSetupPlayer", "(II)V", [](addr_t, const Args &) {});
    java_.DefineVoidMethod(kMobile, "RendererDestroyPlayers", "()V", [](addr_t, const Args &) {});

    // AVProVideoPlayer: one movie.
    auto boolean = [this](const char *name, std::function<bool(Player &)> read) {
      java_.DefineBooleanMethod(kPlayer, name, "()Z", [this, read](addr_t self, const Args &) {
        auto p = Get(self);
        if (!p) return false;
        std::lock_guard lock(mutex_);
        return read(*p);
      });
    };
    auto integer = [this](const char *name, std::function<int32_t(Player &)> read) {
      java_.DefineIntMethod(kPlayer, name, "()I", [this, read](addr_t self, const Args &) {
        auto p = Get(self);
        if (!p) return int32_t{0};
        std::lock_guard lock(mutex_);
        return read(*p);
      });
    };
    auto longer = [this](const char *name, std::function<int64_t(Player &)> read) {
      java_.DefineLongMethod(kPlayer, name, "()J", [this, read](addr_t self, const Args &) {
        auto p = Get(self);
        if (!p) return int64_t{0};
        std::lock_guard lock(mutex_);
        return read(*p);
      });
    };
    auto floating = [this](const char *name, std::function<uint32_t(Player &)> read) {
      java_.DefineFloatBitsMethod(kPlayer, name, "()F", [this, read](addr_t self, const Args &) {
        auto p = Get(self);
        if (!p) return uint32_t{0};
        std::lock_guard lock(mutex_);
        return read(*p);
      });
    };
    auto mutate = [this](const char *name, const char *signature, std::function<void(Player &, const Args &)> write) {
      java_.DefineVoidMethod(kPlayer, name, signature, [this, write](addr_t self, const Args &a) {
        auto p = Get(self);
        if (!p) return;
        std::lock_guard lock(mutex_);
        write(*p, a);
      });
    };

    java_.DefineBooleanMethod(kPlayer, "OpenVideoFromFile", "(Ljava/lang/String;JLjava/lang/String;I)Z", [this](addr_t self, const Args &a) {
      auto p = Get(self);
      auto path = java_.StringValue(a[0]);
      if (!p || !path) return false;
      const auto guest = Ascii(*path);
      std::optional<int64_t> duration;
      std::shared_ptr<video::Decoder> decoder;
      if (auto host = resolve_(guest)) {
        duration = Mp4DurationMs(*host);
        decoder = video::Open(*host);
        if (decoder && decoder->duration_ms > 0) duration = decoder->duration_ms;
      }
      std::lock_guard lock(mutex_);
      const uint32_t texture = p->texture;  // the GL name stays with the player
      *p = Player{p->index};
      p->texture = texture;
      p->path = guest;
      p->opened = true;
      p->decoder = std::move(decoder);
      p->duration_ms = duration && *duration > 0 ? *duration : 1000;
      Log(*p, p->decoder ? "opened (decoding video)" : duration ? "opened (mvhd duration, no decoder)" : "opened (file not in app storage; 1 s)");
      return true;
    });
    mutate("CloseVideo", "()V", [this](Player &p, const Args &) {
      Log(p, "closed");
      const uint32_t texture = p.texture;
      p = Player{p.index};
      p.texture = texture;
    });
    mutate("Play", "()V", [this](Player &p, const Args &) {
      if (!p.opened) return;
      if (p.finished) {
        p.position_ms = 0;
        if (p.decoder) p.decoder->Seek(0);
      }
      p.playing = true;
      p.started = Clock::now();
      p.paused = p.finished = false;
      p.updates_playing = 0;
      Log(p, "play");
    });
    mutate("Pause", "()V", [](Player &p, const Args &) {
      if (!p.playing) return;
      p.position_ms = p.Now();
      p.playing = false;
      p.paused = true;
    });
    mutate("Stop", "()V", [](Player &p, const Args &) {
      p.position_ms = p.Now();
      p.playing = p.paused = false;
    });
    auto seek = [](Player &p, const Args &a) {
      p.position_ms = std::clamp<int64_t>(int32_t(a[0]), 0, p.duration_ms);
      p.started = Clock::now();
      if (p.decoder) p.decoder->Seek(p.position_ms);
      if (p.position_ms < p.duration_ms) p.finished = false;
    };
    mutate("Seek", "(I)V", seek);
    mutate("SeekFast", "(I)V", seek);
    mutate("Update", "()V", [this](Player &p, const Args &) {
      if (p.decoder) {
        if (!p.playing || p.Now() < p.duration_ms) return;
        if (p.looping) {
          p.position_ms = 0;
          p.started = Clock::now();
          p.decoder->Seek(0);
          return;
        }
        p.playing = false;
        p.finished = true;
        p.position_ms = p.duration_ms;
        Log(p, "finished");
        return;
      }
      if (!p.playing || p.looping) return;
      if (++p.updates_playing < 2) return;
      p.playing = false;
      p.finished = true;
      p.position_ms = p.duration_ms;
      Log(p, "finished");
    });
    mutate("SetLooping", "(Z)V", [](Player &p, const Args &a) { p.looping = (a[0] & 0xff) != 0; });
    mutate("MuteAudio", "(Z)V", [](Player &p, const Args &a) { p.muted = (a[0] & 0xff) != 0; });
    mutate("SetVolume", "(F)V", [](Player &p, const Args &a) { p.volume = uint32_t(a[0]); });
    mutate("SetAudioPan", "(F)V", [](Player &p, const Args &a) { p.pan = uint32_t(a[0]); });
    mutate("SetPlaybackRate", "(F)V", [](Player &p, const Args &a) { p.rate = uint32_t(a[0]); });
    for (auto [name, signature] : std::initializer_list<std::pair<const char *, const char *>>{
             {"SetPlayerOptions", "(ZZ)V"}, {"SetHeadRotation", "(FFFF)V"}, {"SetFocusRotation", "(FFFF)V"},
             {"SetFocusProps", "(FF)V"}, {"SetFocusEnabled", "(Z)V"}, {"SetPositionTrackingEnabled", "(Z)V"},
             {"SetAudioTrack", "(I)V"}, {"Deinitialise", "()V"}, {"SetDeinitialiseFlagged", "()V"},
             {"WaitForExtract", "()V"}, {"WaitForNewFrame", "()V"}, {"UpdateAudioVolumes", "()V"}})
      mutate(name, signature, [](Player &, const Args &) {});

    // 2x3 UV transform (a, b, c, d, tx, ty) of the video texture. Frames are
    // uploaded bottom row first (GL/Unity texture order), so it is identity.
    java_.DefineObjectMethod(kPlayer, "GetTextureTransform", "()[F", [this](addr_t, const Args &) {
      return java_.NewFloatArray({1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f});
    });
    boolean("CanPlay", [](Player &p) { return p.opened; });
    boolean("IsPlaying", [](Player &p) { return p.playing; });
    boolean("IsPaused", [](Player &p) { return p.paused; });
    boolean("IsFinished", [](Player &p) { return p.finished; });
    boolean("IsLooping", [](Player &p) { return p.looping; });
    boolean("IsMuted", [](Player &p) { return p.muted; });
    boolean("HasVideo", [](Player &p) { return bool(p.decoder); });
    for (const char *name : {"IsSeeking", "IsBuffering", "HasAudio", "HasSubtitles", "HasTimedText",
                             "GetDeinitialised", "Render", "StartExtractFrame"})
      boolean(name, [](Player &) { return false; });
    integer("GetPlayerIndex", [](Player &p) { return p.index; });
    integer("GetWidth", [](Player &p) { return p.decoder ? int32_t(p.decoder->width) : 0; });
    integer("GetHeight", [](Player &p) { return p.decoder ? int32_t(p.decoder->height) : 0; });
    integer("GetTextureHandle", [](Player &p) { return p.frames ? int32_t(p.texture) : 0; });
    integer("GetFrameCount", [](Player &p) { return p.frames; });
    for (const char *name : {"GetLastErrorCode", "GetCurrentAudioTrackIndex", "GetNumberAudioTracks"})
      integer(name, [](Player &) { return int32_t{0}; });
    longer("GetCurrentTimeMs", [](Player &p) { return std::min(p.Now(), p.duration_ms); });
    longer("GetDurationMs", [](Player &p) { return p.duration_ms; });
    longer("GetTextureTimeStamp", [](Player &) { return int64_t{0}; });
    longer("GetEstimatedBandwidthUsed", [](Player &) { return int64_t{0}; });
    floating("GetVolume", [](Player &p) { return p.volume; });
    floating("GetAudioPan", [](Player &p) { return p.pan; });
    floating("GetPlaybackRate", [](Player &p) { return p.rate; });
    floating("GetBufferingProgressPercent", [](Player &) { return uint32_t{0}; });
    floating("GetDisplayRate", RateBits);
    floating("GetSourceVideoFrameRate", RateBits);
  }

  static uint32_t RateBits(Player &p) {
    const float rate = p.decoder ? p.decoder->frame_rate : 0.0f;
    uint32_t bits;
    std::memcpy(&bits, &rate, sizeof bits);
    return bits;
  }

  // libAVProLocal.so exports. Each takes the player index in w0.
  void Native(size_t which, State &s) {
    const int index = int32_t(s.gpr.x0.dword);
    auto p = which < 8 ? ByIndex(index) : nullptr;
    std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);
    if (p) lock.lock();
    const bool video = p && p->decoder;
    switch (which) {
      case 0: s.gpr.x0.qword = video ? uint64_t(uint32_t(p->decoder->width)) : 0; break;
      case 1: s.gpr.x0.qword = video ? uint64_t(uint32_t(p->decoder->height)) : 0; break;
      case 2: s.gpr.x0.qword = video && p->frames ? p->texture : 0; break;
      case 3: s.gpr.x0.qword = uint64_t(p ? p->duration_ms : 0); break;
      case 4: s.gpr.x0.qword = 0; break;  // no error
      case 5: s.gpr.x0.qword = video ? uint64_t(uint32_t(p->frames)) : 0; break;
      case 6: {  // display rate (float in s0)
        std::memset(&s.simd.v[0], 0, sizeof(s.simd.v[0]));
        const uint32_t bits = p ? RateBits(*p) : 0;
        std::memcpy(&s.simd.v[0], &bits, sizeof bits);
        break;
      }
      case 7: s.gpr.x0.qword = p && p->opened ? 1 : 0; break;
      case 8: s.gpr.x0.qword = kMarkerBase + 8 * kRenderEvent; break;
      case kRenderEvent: RenderEvent(uint32_t(s.gpr.x0.dword)); break;
      default: break;  // audio fill, device events, nativeInit
    }
  }

  // GL entry points from the process GLES library (ANGLE's libGLESv2 on Windows).
  struct Gl {
    void (*gen)(int, uint32_t *) = nullptr;
    void (*bind)(uint32_t, uint32_t) = nullptr;
    void (*image)(uint32_t, int, int, int, int, int, uint32_t, uint32_t, const void *) = nullptr;
    void (*sub)(uint32_t, int, int, int, int, int, uint32_t, uint32_t, const void *) = nullptr;
    void (*param)(uint32_t, uint32_t, int) = nullptr;
    void (*get)(uint32_t, int *) = nullptr;
    void (*store)(uint32_t, int) = nullptr;
    void (*buffer)(uint32_t, uint32_t) = nullptr;
    bool ok = false;
    template <class F>
    static void Load(F &fn, const char *name) {
#ifdef _WIN32
      HMODULE gles = GetModuleHandleA("libGLESv2.dll");
      fn = gles ? reinterpret_cast<F>(reinterpret_cast<void *>(GetProcAddress(gles, name))) : nullptr;
#else
      fn = reinterpret_cast<F>(dlsym(RTLD_DEFAULT, name));
#endif
    }
    Gl() {
      Load(gen, "glGenTextures"); Load(bind, "glBindTexture"); Load(image, "glTexImage2D"); Load(sub, "glTexSubImage2D");
      Load(param, "glTexParameteri"); Load(get, "glGetIntegerv"); Load(store, "glPixelStorei"); Load(buffer, "glBindBuffer");
      ok = gen && bind && image && sub && param && get && store && buffer;
      if (!ok) std::fprintf(stderr, "AVPro host: GLES entry points unavailable; video frames are not uploaded\n");
    }
  };

  // AVPro's GL.IssuePluginEvent id: 0x5d5ac000 | type << 8 | player index.
  // PlayerUpdate (type 2) runs on Unity's render thread with the GL context current.
  void RenderEvent(uint32_t id) {
    if (render_logs_ < 8) { ++render_logs_; std::fprintf(stderr, "AVPro host: render event %#x\n", id); }
    if ((id & 0xffffc000u) != 0x5d5ac000u || ((id >> 8) & 0x3f) != 2) return;
    auto p = ByIndex(int(id & 0xff));
    if (!p) return;
    std::shared_ptr<video::Decoder> decoder;
    int64_t clock = 0;
    uint32_t texture = 0;
    {
      std::lock_guard lock(mutex_);
      if (!p->decoder) return;
      decoder = p->decoder;
      clock = std::min(p->Now(), p->duration_ms);
      texture = p->texture;
    }
    video::Frame frame;
    const bool due = decoder->TakeDue(clock, frame) && !frame.rgba.empty();
    {  // Delivery rate, every 5 s while a player updates.
      static int updates = 0, uploads = 0;
      static Clock::time_point window = Clock::now();
      ++updates; uploads += due;
      if (Clock::now() - window >= std::chrono::seconds(5)) {
        std::fprintf(stderr, "AVPro host: player %d updates=%d uploads=%d in 5s clock=%lld\n", p->index, updates, uploads, (long long)clock);
        updates = uploads = 0; window = Clock::now();
      }
    }
    if (!due) return;
    static const Gl gl;
    if (!gl.ok) return;
    constexpr uint32_t kTexture2D = 0x0DE1, kBinding2D = 0x8069, kRgba = 0x1908, kUnsignedByte = 0x1401;
    constexpr uint32_t kUnpackAlignment = 0x0CF5, kUnpackRowLength = 0x0CF2, kUnpackSkipRows = 0x0CF3,
                       kUnpackSkipPixels = 0x0CF4, kPixelUnpackBuffer = 0x88EC, kPixelUnpackBinding = 0x88EF;
    int bound = 0, alignment = 4, row_length = 0, skip_rows = 0, skip_pixels = 0, unpack = 0;
    gl.get(kBinding2D, &bound); gl.get(kUnpackAlignment, &alignment); gl.get(kUnpackRowLength, &row_length);
    gl.get(kUnpackSkipRows, &skip_rows); gl.get(kUnpackSkipPixels, &skip_pixels); gl.get(kPixelUnpackBinding, &unpack);
    if (unpack) gl.buffer(kPixelUnpackBuffer, 0);
    gl.store(kUnpackAlignment, 4); gl.store(kUnpackRowLength, 0); gl.store(kUnpackSkipRows, 0); gl.store(kUnpackSkipPixels, 0);
    const bool create = !texture;
    if (create) gl.gen(1, &texture);
    gl.bind(kTexture2D, texture);
    const auto size = std::make_pair(decoder->width, decoder->height);
    if (create || texture_sizes_[texture] != size) {
      gl.image(kTexture2D, 0, int(kRgba), decoder->width, decoder->height, 0, kRgba, kUnsignedByte, nullptr);
      gl.param(kTexture2D, 0x2801, 0x2601); gl.param(kTexture2D, 0x2800, 0x2601);  // min/mag LINEAR
      gl.param(kTexture2D, 0x2802, 0x812F); gl.param(kTexture2D, 0x2803, 0x812F);  // wrap CLAMP_TO_EDGE
      texture_sizes_[texture] = size;
    }
    gl.sub(kTexture2D, 0, 0, 0, decoder->width, decoder->height, kRgba, kUnsignedByte, frame.rgba.data());
    gl.bind(kTexture2D, uint32_t(bound));
    gl.store(kUnpackAlignment, alignment); gl.store(kUnpackRowLength, row_length);
    gl.store(kUnpackSkipRows, skip_rows); gl.store(kUnpackSkipPixels, skip_pixels);
    if (unpack) gl.buffer(kPixelUnpackBuffer, uint32_t(unpack));
    std::lock_guard lock(mutex_);
    p->texture = texture;
    const int32_t n = ++p->frames;
    if (n == 1 || n % 600 == 0)
      std::fprintf(stderr, "AVPro host: player %d frame %d pts=%lld clock=%lld texture=%u %dx%d\n", p->index, n,
                   (long long)frame.pts_ms, (long long)clock, texture, decoder->width, decoder->height);
  }

  jni::Host &java_;
  PathResolver resolve_;
  std::mutex mutex_;
  std::map<int, std::shared_ptr<Player>> players_;
  int next_index_ = 0;
  std::atomic<uint32_t> logs_{0};
  // Render thread only.
  std::map<uint32_t, std::pair<int, int>> texture_sizes_;
  int render_logs_ = 0;
};

}  // namespace bunker::windows::avpro_video
