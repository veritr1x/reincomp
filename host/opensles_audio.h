#pragma once
// Host implementation of the part of OpenSL ES that Unity's audio output uses
// (libunity.so loads libOpenSLES.so with dlopen and resolves these by name):
// an engine, an output mix, and PCM audio players fed through an Android
// simple buffer queue. Each player owns one output stream and one thread; the
// thread plays what the game enqueues and calls the game's buffer-queue
// callback for every buffer that finished, as Android does.
//
// Output is waveOut on Windows. Elsewhere there is no device yet: buffers are
// consumed in real time so the game's audio clock still runs.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "../runtime/runtime.h"
#include "module_registry.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmreg.h>
#include <mmsystem.h>
#endif

namespace bunker::windows::opensles {

enum : uint32_t { kSuccess = 0, kParameterInvalid = 2, kMemoryFailure = 3, kContentUnsupported = 10, kFeatureUnsupported = 12, kBufferInsufficient = 7 };
enum : uint32_t { kStopped = 1, kPaused = 2, kPlaying = 3 };

struct InterfaceId { uint32_t a; uint16_t b, c, d; uint8_t node[6]; };
// What the game holds: a pointer to this, whose first word points at the method table.
struct Interface { const addr_t *methods; void *owner; };
static_assert(offsetof(Interface, methods) == 0);

// One PCM output stream. Submit takes ownership of a block; Finished returns how
// many blocks completed since the last call; Wait blocks until something changed.
class Output {
 public:
  virtual ~Output() = default;
  virtual void Submit(std::vector<uint8_t> block) = 0;
  virtual size_t Finished() = 0;
  virtual void Wait() = 0;
  virtual void Wake() = 0;
  virtual void SetPlaying(bool) = 0;
  virtual void Clear() = 0;
};

#ifdef _WIN32
class WaveOutput final : public Output {
 public:
  WaveOutput(uint32_t channels, uint32_t rate, uint32_t bits, bool floating) {
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    WAVEFORMATEX format{};
    format.wFormatTag = floating ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    format.nChannels = WORD(channels);
    format.nSamplesPerSec = rate;
    format.wBitsPerSample = WORD(bits);
    format.nBlockAlign = WORD(channels * bits / 8);
    format.nAvgBytesPerSec = rate * format.nBlockAlign;
    error_ = waveOutOpen(&device_, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(event_), 0, CALLBACK_EVENT);
    if (error_ != MMSYSERR_NOERROR) device_ = nullptr;
    if (device_) waveOutPause(device_);
  }
  ~WaveOutput() override {
    if (device_) { waveOutReset(device_); Reap(); waveOutClose(device_); }
    CloseHandle(event_);
  }
  bool Open() const { return device_ != nullptr; }
  unsigned Error() const { return error_; }  // the waveOutOpen result when no device opened
  void Submit(std::vector<uint8_t> block) override {
    auto item = std::make_unique<Block>();
    item->data = std::move(block);
    item->header.lpData = reinterpret_cast<LPSTR>(item->data.data());
    item->header.dwBufferLength = DWORD(item->data.size());
    waveOutPrepareHeader(device_, &item->header, sizeof(WAVEHDR));
    waveOutWrite(device_, &item->header, sizeof(WAVEHDR));
    blocks_.push_back(std::move(item));
  }
  size_t Finished() override { return Reap(); }
  void Wait() override { WaitForSingleObject(event_, 200); }
  void Wake() override { SetEvent(event_); }
  void SetPlaying(bool playing) override { if (playing) waveOutRestart(device_); else waveOutPause(device_); }
  void Clear() override { waveOutReset(device_); Reap(); }
 private:
  struct Block { WAVEHDR header{}; std::vector<uint8_t> data; };
  size_t Reap() {
    size_t done = 0;
    while (!blocks_.empty() && (blocks_.front()->header.dwFlags & WHDR_DONE)) {
      waveOutUnprepareHeader(device_, &blocks_.front()->header, sizeof(WAVEHDR));
      blocks_.pop_front();
      ++done;
    }
    return done;
  }
  HWAVEOUT device_ = nullptr;
  unsigned error_ = 0;
  HANDLE event_ = nullptr;
  std::deque<std::unique_ptr<Block>> blocks_;
};
#endif

// No device: each block takes its own duration to "play".
class TimedOutput final : public Output {
 public:
  explicit TimedOutput(uint32_t bytes_per_second) : rate_(bytes_per_second ? bytes_per_second : 1) {}
  void Submit(std::vector<uint8_t> block) override {
    const auto now = std::chrono::steady_clock::now();
    if (ends_.empty() || next_ < now) next_ = now;
    next_ += std::chrono::microseconds(uint64_t(block.size()) * 1000000 / rate_);
    ends_.push_back(next_);
  }
  size_t Finished() override {
    size_t done = 0;
    if (!playing_) return 0;
    const auto now = std::chrono::steady_clock::now();
    while (!ends_.empty() && ends_.front() <= now) { ends_.pop_front(); ++done; }
    return done;
  }
  void Wait() override { std::unique_lock lock(mutex_); wake_.wait_for(lock, std::chrono::milliseconds(5)); }
  void Wake() override { wake_.notify_all(); }
  void SetPlaying(bool playing) override { playing_ = playing; }
  void Clear() override { ends_.clear(); }
 private:
  uint32_t rate_;
  bool playing_ = false;
  std::chrono::steady_clock::time_point next_{};
  std::deque<std::chrono::steady_clock::time_point> ends_;
  std::mutex mutex_;
  std::condition_variable wake_;
};

class Service {
 public:
  using Runner = std::function<uint64_t(addr_t, const std::vector<addr_t> &)>;
  static constexpr addr_t kMarkerBase = 0x7fea00000000ULL;

  // `runner` calls a game function on the calling thread; `release` is called on
  // a player thread before it ends, to retire what the runner attached to it.
  Service(Runner runner, std::function<void()> release) : runner_(std::move(runner)), release_(std::move(release)) {
    for (size_t i = 0; i < kObjectMethods; ++i) object_methods_[i] = Marker(kObject, i);
    for (size_t i = 0; i < kEngineMethods; ++i) engine_methods_[i] = Marker(kEngine, i);
    for (size_t i = 0; i < kPlayMethods; ++i) play_methods_[i] = Marker(kPlay, i);
    for (size_t i = 0; i < kQueueMethods; ++i) queue_methods_[i] = Marker(kQueue, i);
    for (size_t i = 0; i < kConfigurationMethods; ++i) configuration_methods_[i] = Marker(kConfiguration, i);
  }
  ~Service() { players_.clear(); }

  Module MakeModule() const {
    Module module;
    module.name = "libOpenSLES.so";
    module.aliases = {"OpenSLES", "/system/lib64/libOpenSLES.so"};
    module.kind = Module::Kind::TypedService;
    module.exports.emplace("slCreateEngine", kMarkerBase);
    module.exports.emplace("SL_IID_ENGINE", reinterpret_cast<addr_t>(&engine_id_));
    module.exports.emplace("SL_IID_PLAY", reinterpret_cast<addr_t>(&play_id_));
    module.exports.emplace("SL_IID_ANDROIDSIMPLEBUFFERQUEUE", reinterpret_cast<addr_t>(&queue_id_));
    module.exports.emplace("SL_IID_BUFFERQUEUE", reinterpret_cast<addr_t>(&queue_id_));
    module.exports.emplace("SL_IID_ANDROIDCONFIGURATION", reinterpret_cast<addr_t>(&configuration_id_));
    module.exports.emplace("SL_IID_RECORD", reinterpret_cast<addr_t>(&record_id_));
    module.reset_image = [] {};
    return module;
  }

  void Install(recomp::Runtime &runtime) {
    runtime.AddImport(kMarkerBase, [this](State &s) { s.gpr.x0.qword = CreateEngine(s.gpr.x0.qword); });
    for (size_t i = 0; i < kObjectMethods; ++i) runtime.AddImport(Marker(kObject, i), [this, i](State &s) { ObjectCall(i, s); });
    for (size_t i = 0; i < kEngineMethods; ++i) runtime.AddImport(Marker(kEngine, i), [this, i](State &s) { EngineCall(i, s); });
    for (size_t i = 0; i < kPlayMethods; ++i) runtime.AddImport(Marker(kPlay, i), [this, i](State &s) { PlayCall(i, s); });
    for (size_t i = 0; i < kQueueMethods; ++i) runtime.AddImport(Marker(kQueue, i), [this, i](State &s) { QueueCall(i, s); });
    for (size_t i = 0; i < kConfigurationMethods; ++i) runtime.AddImport(Marker(kConfiguration, i), [](State &s) { s.gpr.x0.qword = kSuccess; });
  }

 private:
  enum Table : addr_t { kObject = 1, kEngine, kPlay, kQueue, kConfiguration };
  static constexpr size_t kObjectMethods = 10, kEngineMethods = 15, kPlayMethods = 12, kQueueMethods = 4, kConfigurationMethods = 4;
  static constexpr addr_t Marker(Table table, size_t index) { return kMarkerBase + table * 0x100 + index * 8; }

  struct Object {
    enum Kind { kEngineObject, kMixObject, kPlayerObject } kind;
    Interface object{};
    explicit Object(Kind k) : kind(k) {}
    virtual ~Object() = default;
  };
  struct Engine : Object { Interface engine{}; Engine() : Object(kEngineObject) {} };
  struct Mix : Object { Mix() : Object(kMixObject) {} };
  struct Player : Object {
    Interface play{}, queue{}, configuration{};
    Service *service = nullptr;
    std::unique_ptr<Output> output;
    std::mutex mutex;
    std::deque<std::vector<uint8_t>> pending;  // enqueued, not yet handed to the output
    uint32_t queued = 0, played = 0, state = kStopped;
    addr_t callback = 0, context = 0;
    bool stopping = false, clear = false, state_changed = false;
    std::thread thread;
    Player() : Object(kPlayerObject) {}
    ~Player() override {
      { std::lock_guard lock(mutex); stopping = true; }
      if (output) output->Wake();
      if (thread.joinable()) thread.join();
    }
    void Work() {
      for (;;) {
        std::deque<std::vector<uint8_t>> take;
        bool wipe = false, change = false; uint32_t now = kStopped; addr_t call = 0, argument = 0;
        {
          std::lock_guard lock(mutex);
          if (stopping) break;
          take.swap(pending); wipe = clear; clear = false; change = state_changed; state_changed = false; now = state;
          call = callback; argument = context;
        }
        if (wipe) output->Clear();
        for (auto &block : take) output->Submit(std::move(block));
        if (change) output->SetPlaying(now == kPlaying);
        size_t finished = output->Finished();
        if (finished) {
          { std::lock_guard lock(mutex); queued -= uint32_t(std::min<size_t>(finished, queued)); played += uint32_t(finished); }
          // One callback per finished buffer, as Android's queue does.
          for (; finished && call; --finished) service->runner_(call, {reinterpret_cast<addr_t>(&queue), argument});
          continue;
        }
        output->Wait();
      }
      if (service->release_) service->release_();
    }
  };

  template <class T> static T *Owner(addr_t self) { return static_cast<T *>(static_cast<Object *>(reinterpret_cast<Interface *>(self)->owner)); }
  static void Put(addr_t address, uint64_t value, size_t size) { if (address) recomp::WriteGuestMemory(address, &value, size); }
  template <class T> static T Get(addr_t address) { T value{}; recomp::ReadGuestMemory(&value, address, sizeof(T)); return value; }
  void Note(const char *text) { if (notes_++ < 40) { std::fprintf(stderr, "Audio: %s\n", text); std::fflush(stderr); } }

  uint32_t CreateEngine(addr_t out) {
    if (!out) return kParameterInvalid;
    auto engine = std::make_unique<Engine>();
    engine->object = {object_methods_, engine.get()};
    engine->engine = {engine_methods_, engine.get()};
    Put(out, reinterpret_cast<addr_t>(&engine->object), 8);
    std::lock_guard lock(mutex_);
    objects_.push_back(std::move(engine));
    Note("OpenSL ES engine created");
    return kSuccess;
  }

  // SLObjectItf: Realize, Resume, GetState, GetInterface, RegisterCallback,
  // AbortAsyncOperation, Destroy, SetPriority, GetPriority, SetLossOfControlInterfaces.
  void ObjectCall(size_t method, State &s) {
    const addr_t self = s.gpr.x0.qword;
    auto *object = static_cast<Object *>(reinterpret_cast<Interface *>(self)->owner);
    uint32_t result = kSuccess;
    switch (method) {
      case 2: Put(s.gpr.x1.qword, 2 /* realized */, 4); break;
      case 3: result = GetInterface(object, s.gpr.x1.qword, s.gpr.x2.qword); break;
      case 6: Destroy(object); break;
      case 8: Put(s.gpr.x1.qword, 0, 4); Put(s.gpr.x2.qword, 0, 4); break;
      default: break;
    }
    s.gpr.x0.qword = result;
  }
  uint32_t GetInterface(Object *object, addr_t id, addr_t out) {
    if (!id || !out) return kParameterInvalid;
    const auto wanted = Get<InterfaceId>(id);
    auto is = [&](const InterfaceId *known) { return !std::memcmp(&wanted, known, sizeof(InterfaceId)); };
    Interface *found = nullptr;
    if (object->kind == Object::kEngineObject && is(engine_id_)) found = &static_cast<Engine *>(object)->engine;
    if (object->kind == Object::kPlayerObject) {
      auto *player = static_cast<Player *>(object);
      if (is(play_id_)) found = &player->play;
      else if (is(queue_id_)) found = &player->queue;
      else if (is(configuration_id_)) found = &player->configuration;
    }
    if (!found) { Put(out, 0, 8); return kFeatureUnsupported; }
    Put(out, reinterpret_cast<addr_t>(found), 8);
    return kSuccess;
  }
  void Destroy(Object *object) {
    std::unique_ptr<Object> owned;
    {
      std::lock_guard lock(mutex_);
      for (auto *list : {&objects_, &players_})
        for (auto it = list->begin(); it != list->end(); ++it)
          if (it->get() == object) { owned = std::move(*it); list->erase(it); break; }
    }
    // The player's thread is joined here, outside the registry lock.
  }

  // SLEngineItf: only CreateAudioPlayer (2) and CreateOutputMix (7) exist here.
  void EngineCall(size_t method, State &s) {
    uint32_t result = kFeatureUnsupported;
    if (method == 2) result = CreatePlayer(s.gpr.x1.qword, s.gpr.x2.qword);
    else if (method == 7) {
      auto mix = std::make_unique<Mix>();
      mix->object = {object_methods_, mix.get()};
      Put(s.gpr.x1.qword, reinterpret_cast<addr_t>(&mix->object), 8);
      std::lock_guard lock(mutex_);
      objects_.push_back(std::move(mix));
      result = kSuccess;
    } else if (s.gpr.x1.qword && method <= 9) Put(s.gpr.x1.qword, 0, 8);
    s.gpr.x0.qword = result;
  }
  uint32_t CreatePlayer(addr_t out, addr_t source) {
    if (!out || !source) return kParameterInvalid;
    // SLDataSource { void *locator; void *format }; the format is SLDataFormat_PCM
    // { type, channels, milliHertz, bits, container bits, channel mask, endianness },
    // or Android's extended form (type 4) with a representation word after it.
    const addr_t format = Get<addr_t>(source + 8);
    if (!format) return kParameterInvalid;
    struct Pcm { uint32_t type, channels, millihertz, bits, container, mask, endianness, representation; };
    Pcm pcm{};
    recomp::ReadGuestMemory(&pcm, format, 28);
    if (pcm.type == 4) pcm.representation = Get<uint32_t>(format + 28);
    else if (pcm.type != 2) return kContentUnsupported;
    const bool floating = pcm.type == 4 && pcm.representation == 3;
    const uint32_t rate = pcm.millihertz / 1000, bits = pcm.container ? pcm.container : pcm.bits;
    if (!pcm.channels || pcm.channels > 8 || !rate || (bits != 8 && bits != 16 && bits != 32)) return kContentUnsupported;
    auto player = std::make_unique<Player>();
    player->service = this;
    player->object = {object_methods_, player.get()};
    player->play = {play_methods_, player.get()};
    player->queue = {queue_methods_, player.get()};
    player->configuration = {configuration_methods_, player.get()};
#ifdef _WIN32
    auto wave = std::make_unique<WaveOutput>(pcm.channels, rate, bits, floating);
    if (wave->Open()) player->output = std::move(wave);
#endif
    const bool device = player->output != nullptr;
    if (!device) player->output = std::make_unique<TimedOutput>(rate * pcm.channels * bits / 8);
    char text[160];
    std::snprintf(text, sizeof text, "player created: %u Hz, %u channels, %u-bit %s, %s", rate, pcm.channels, bits, floating ? "float" : "integer", device ? "sound device open" : "no sound device, silent");
    Note(text);
    auto *raw = player.get();
    raw->thread = std::thread([raw] { raw->Work(); });
    Put(out, reinterpret_cast<addr_t>(&raw->object), 8);
    std::lock_guard lock(mutex_);
    players_.push_back(std::move(player));
    return kSuccess;
  }

  // SLPlayItf: SetPlayState (0) and GetPlayState (1); position queries answer zero.
  void PlayCall(size_t method, State &s) {
    auto *player = Owner<Player>(s.gpr.x0.qword);
    if (method == 0) {
      { std::lock_guard lock(player->mutex); player->state = s.gpr.x1.dword; player->state_changed = true; }
      player->output->Wake();
    } else if (method == 1) {
      std::lock_guard lock(player->mutex);
      Put(s.gpr.x1.qword, player->state, 4);
    } else if (method == 2 || method == 3 || method == 6 || method == 9 || method == 11) Put(s.gpr.x1.qword, 0, 4);
    s.gpr.x0.qword = kSuccess;
  }

  // SLAndroidSimpleBufferQueueItf: Enqueue, Clear, GetState, RegisterCallback.
  void QueueCall(size_t method, State &s) {
    auto *player = Owner<Player>(s.gpr.x0.qword);
    uint32_t result = kSuccess;
    if (method == 0) {
      const addr_t data = s.gpr.x1.qword; const uint32_t size = s.gpr.x2.dword;
      if (!data || !size) result = kParameterInvalid;
      else {
        std::vector<uint8_t> block(size);
        recomp::ReadGuestMemory(block.data(), data, size);
        { std::lock_guard lock(player->mutex); player->pending.push_back(std::move(block)); ++player->queued; }
        player->output->Wake();
      }
    } else if (method == 1) {
      { std::lock_guard lock(player->mutex); player->pending.clear(); player->queued = 0; player->clear = true; }
      player->output->Wake();
    } else if (method == 2) {
      std::lock_guard lock(player->mutex);
      Put(s.gpr.x1.qword, player->queued, 4);
      Put(s.gpr.x1.qword + 4, player->played, 4);
    } else if (method == 3) {
      std::lock_guard lock(player->mutex);
      player->callback = s.gpr.x1.qword; player->context = s.gpr.x2.qword;
    }
    s.gpr.x0.qword = result;
  }

  Runner runner_;
  std::function<void()> release_;
  addr_t object_methods_[kObjectMethods], engine_methods_[kEngineMethods], play_methods_[kPlayMethods], queue_methods_[kQueueMethods], configuration_methods_[kConfigurationMethods];
  // The interface IDs OpenSL ES defines. The game reads the exported pointer and
  // passes it back; they are compared by value.
  const InterfaceId engine_ids_[1] = {{0x8d97c260, 0xddd4, 0x11db, 0x958f, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}};
  const InterfaceId play_ids_[1] = {{0xef0bd9c0, 0xddd7, 0x11db, 0xbf49, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}};
  const InterfaceId queue_ids_[1] = {{0x198e4940, 0xc5d7, 0x11df, 0xa2a6, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}};
  const InterfaceId configuration_ids_[1] = {{0x89f6a7e0, 0xbeac, 0x11df, 0x8b5c, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}};
  const InterfaceId record_ids_[1] = {{0xc5657aa0, 0xdddb, 0x11db, 0x82f7, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}}};
  const InterfaceId *engine_id_ = engine_ids_, *play_id_ = play_ids_, *queue_id_ = queue_ids_, *configuration_id_ = configuration_ids_, *record_id_ = record_ids_;
  std::mutex mutex_;
  std::vector<std::unique_ptr<Object>> objects_;
  std::vector<std::unique_ptr<Object>> players_;
  std::atomic<unsigned> notes_{0};
};

}  // namespace bunker::windows::opensles
