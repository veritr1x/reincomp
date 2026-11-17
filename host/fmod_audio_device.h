#pragma once
// org.fmod.FMODAudioDevice, the Java class Unity's FMOD uses for sound output on
// Android. The real class runs a Java thread that asks the game for mixed
// audio (fmodProcess, into a direct ByteBuffer) and writes it to an AudioTrack.
// This does the same on a host thread, writing to the host's sound device.
//
// fmodGetInfo(n): 0 sample rate, 1 block length in frames, 2 number of blocks,
// 3 whether the game wants audio running. Output is 16-bit stereo.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "jni_host.h"
#include "opensles_audio.h"

namespace bunker::windows::fmod_audio {

class Service {
 public:
  using Runner = std::function<uint64_t(addr_t, const std::vector<addr_t> &)>;
  static constexpr const char *kDevice = "org/fmod/FMODAudioDevice";
  static constexpr const char *kBuffer = "java/nio/ByteBuffer";

  Service(jni::Host &java, Runner runner, std::function<void()> release)
      : java_(java), runner_(std::move(runner)), release_(std::move(release)) {
    using Args = jni::Host::MethodArguments;
    java_.DefineClass(kBuffer);
    // The game registers these natives itself (RegisterNatives).
    java_.DefineClass(kDevice, {{"fmodGetInfo", "(I)I", false, true},
                                {"fmodProcess", "(Ljava/nio/ByteBuffer;)I", false, true},
                                {"fmodProcessMicData", "(Ljava/nio/ByteBuffer;I)I", false, true}});
    java_.DefineConstructor(kDevice, "()V", [this](const Args &) { return java_.NewHostInstance(kDevice, std::make_shared<Device>()); });
    java_.DefineVoidMethod(kDevice, "start", "()V", [this](addr_t self, const Args &) { Start(self); });
    java_.DefineVoidMethod(kDevice, "stop", "()V", [this](addr_t self, const Args &) { Stop(self); });
    java_.DefineVoidMethod(kDevice, "close", "()V", [this](addr_t self, const Args &) { Stop(self); });
    java_.DefineBooleanMethod(kDevice, "isRunning", "()Z", [this](addr_t self, const Args &) {
      auto device = java_.ReadHostState<Device>(self);
      return device && device->running.load();
    });
    // No microphone.
    java_.DefineIntMethod(kDevice, "startAudioRecord", "(III)I", [](addr_t, const Args &) { return 0; });
    java_.DefineVoidMethod(kDevice, "stopAudioRecord", "()V", [](addr_t, const Args &) {});
  }
  ~Service() {
    std::vector<std::shared_ptr<Device>> devices;
    { std::lock_guard lock(mutex_); devices.swap(devices_); }
    for (auto &device : devices) Halt(*device);
  }

 private:
  struct Device {
    std::atomic<bool> running{false};
    std::thread thread;
    addr_t self = 0;  // global reference the thread passes to the game
  };

  void Start(addr_t self) {
    auto device = java_.ReadHostState<Device>(self);
    if (!device) return;
    Halt(*device);
    if (!device->self) device->self = java_.NewGlobalReference(self);
    device->running = true;
    { std::lock_guard lock(mutex_); devices_.push_back(device); }
    device->thread = std::thread([this, device] { Work(*device); });
    std::fputs("Audio: FMOD audio device started\n", stderr);
  }
  void Stop(addr_t self) {
    if (auto device = java_.ReadHostState<Device>(self)) Halt(*device);
  }
  static void Halt(Device &device) {
    device.running = false;
    if (device.thread.joinable()) device.thread.join();
  }

  void Work(Device &device) {
    try {
      const addr_t info = java_.NativeEntry(kDevice, "fmodGetInfo", "(I)I");
      const addr_t process = java_.NativeEntry(kDevice, "fmodProcess", "(Ljava/nio/ByteBuffer;)I");
      // This thread attaches to the Java VM as the real one would.
      addr_t env = 0;
      { ::State vm{}; vm.gpr.x0.qword = java_.VmAddress(); vm.gpr.x1.qword = reinterpret_cast<addr_t>(&env); java_.InvokeVm(4, vm); }
      if (!env) throw std::runtime_error("the audio thread could not attach to the Java VM");
      auto ask = [&](uint32_t what) { return uint32_t(runner_(info, {env, device.self, what})); };
      const uint32_t rate = ask(0), frames = ask(1), blocks = ask(2);
      if (!rate || !frames || frames > (1u << 20)) throw std::runtime_error("the game reported no usable audio format");
      const size_t bytes = size_t(frames) * 2 * 2;
      std::vector<uint8_t> storage(bytes);
      auto direct = std::make_shared<jni::DirectBuffer>();
      direct->address = reinterpret_cast<addr_t>(storage.data());
      direct->capacity = bytes;
      const addr_t local = java_.NewHostInstance(kBuffer, direct);
      const addr_t buffer = java_.NewGlobalReference(local);
      java_.DeleteLocalReference(local);
      std::unique_ptr<opensles::Output> output;
      bool sound = false;
#ifdef _WIN32
      auto wave = std::make_unique<opensles::WaveOutput>(2, rate, 16, false);
      if (wave->Open()) { output = std::move(wave); sound = true; }
      else std::fprintf(stderr, "Audio: the sound device did not open (waveOutOpen error %u, %u devices)\n", wave->Error(), waveOutGetNumDevs());
#endif
      if (!output) output = std::make_unique<opensles::TimedOutput>(rate * 4);
      std::fprintf(stderr, "Audio: %u Hz stereo, blocks of %u frames, %s\n", rate, frames, sound ? "sound device open" : "no sound device, silent");
      std::fflush(stderr);
      output->SetPlaying(true);
      // Keep a few blocks ahead of the device, as AudioTrack's buffer does.
      const size_t ahead = blocks > 2 ? blocks : 3;
      size_t waiting = 0;
      size_t mixed = 0; int peak = 0, reports = 0;
      while (device.running) {
        if (ask(3) != 1) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
        runner_(process, {env, device.self, buffer});
        // Until the game is first audible (or a minute passes), report the loudest sample.
        if (reports < 12) {
          for (size_t i = 0; i + 1 < bytes; i += 2) {
            const int sample = int16_t(storage[i] | storage[i + 1] << 8);
            peak = std::max(peak, sample < 0 ? -sample : sample);
          }
          if (++mixed % (size_t(rate) * 5 / frames) == 0) {
            std::fprintf(stderr, "Audio: loudest sample in the last 5 s: %d of 32767\n", peak);
            std::fflush(stderr);
            reports = peak ? 12 : reports + 1;  // once sound is heard, stop reporting
            peak = 0;
          }
        }
        output->Submit(std::vector<uint8_t>(storage.begin(), storage.end()));
        ++waiting;
        while (device.running && waiting >= ahead) {
          const size_t done = output->Finished();
          if (done) waiting -= done < waiting ? done : waiting;
          else output->Wait();
        }
      }
      output.reset();
      java_.DeleteGlobalReference(buffer);
      { ::State vm{}; vm.gpr.x0.qword = java_.VmAddress(); java_.InvokeVm(5, vm); }
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Audio: FMOD audio device stopped: %s\n", error.what());
      std::fflush(stderr);
    }
    device.running = false;
    if (release_) release_();
  }

  jni::Host &java_;
  Runner runner_;
  std::function<void()> release_;
  std::mutex mutex_;
  std::vector<std::shared_ptr<Device>> devices_;
};

}  // namespace bunker::windows::fmod_audio
