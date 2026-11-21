#pragma once
// Frame rates above the game's 30, ported from the Bunker Android app's
// display_patch.c (3.7.1 global; same RVAs, il2cpp loaded at 0x80000000).
// Lifted code cannot take byte patches, so each instruction patch there
// becomes guest data written each frame or a guest entry hook here:
//
// - Application.targetFrameRate: the game sets 30 (Generator.OnlyWhenStarting
//   and SetupRenderer); the host writes the chosen rate each frame instead
//   (game_bootstrap.cpp ApplyGameTargetFps).
// - Config.UnSavePowerTargetRenderFrameRate (30, Config..cctor 2C7AC80): the
//   performance presets draw every target/that frames through
//   OnDemandRendering.renderFrameInterval, so at 60 only every 2nd update was
//   drawn. The static is rewritten to the chosen rate once Config exists.
// - ActorController.FixedProcess (2D5F7A0, from ActorController.FixedUpdate)
//   moves characters on Unity's 1/30 s physics tick, so at higher rates they
//   moved on only some frames. display_patch.c sets the tick to each frame's
//   duration; here it is quantized (1/30 to 1/240 s) with hysteresis instead
//   (see FollowFrameTime). FixedUpdate tail-branches to FixedProcess, so the
//   probe is on FixedUpdate.
// - BattleUpdate.OnStateUpdate (2A6527C) tail-calls KernelBattle.Process
//   (37DC3D0) with (1.0, 0, null) for one 30 Hz battle step per frame; it now
//   runs only once 1/30 s of frame time has built up (display_patch.c
//   battle_step).
// The Windows player applies Bunker's summon-list snapping and campaign badge
// timing in its Unity adapters. Flying Mama uses a separate game loop.
// Portable: no platform headers; guest calls go through the supplied caller.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

#include "../runtime/runtime.h"

extern "C" Memory *sub_82a6527c(State &, addr_t, Memory *);  // BattleUpdate.OnStateUpdate (lifted)

namespace bunker::windows::frame_rate {

// Guest call with an optional float argument/result in s0.
using FloatCall = std::function<uint64_t(addr_t, const std::vector<addr_t> &, const float *, float *)>;
inline FloatCall &Call() { static FloatCall call; return call; }
inline int32_t &Rate() { static int32_t fps = 0; return fps; }

constexpr addr_t kTimeUnscaledDelta = 0x84d7ed90, kTimeFixedDelta = 0x84d7edc4,
                 kTimeSetFixedDelta = 0x84d7edf8, kTimeFrameCount = 0x84d7ef50;
// FixedUpdate tail-branches to FixedProcess (2D5F7A0); lifted tail branches skip
// probes, so the probe sits on FixedUpdate, which Unity reaches through dispatch.
// OnStateUpdate likewise tail-branches to KernelBattle.Process (37DC3D0), so
// the battle gate is a direct entry on OnStateUpdate (a virtual, dispatched).
constexpr addr_t kFixedUpdate = 0x82d62510, kBattleStateUpdate = 0x82a6527c;
// OnDemandRendering.set_renderFrameInterval stores its clamped argument at
// offset zero in the class statics (il2cpp 4A0A110..4A0A140).
constexpr addr_t kOnDemandClassSlot = 0x874062c0;
constexpr addr_t kConfigClassSlot = 0x87383410;  // Config's Il2CppClass* (Config..cctor's GOT load)

// Once per host frame: Config's statics, once the class is initialised.
inline addr_t Statics(addr_t class_slot) {
  addr_t slot = 0, klass = 0, statics = 0;
  recomp::ReadGuestMemory(&slot, class_slot, 8);
  if (!slot) return 0;
  recomp::ReadGuestMemory(&klass, slot, 8);
  if (!klass) return 0;
  recomp::ReadGuestMemory(&statics, klass + 0xb8, 8);
  return statics;
}

inline void ApplyData(int32_t fps, bool repair_interval = true) {
  if (fps <= 30) return;
  // The preset uses integer division and may run before the host updates
  // Config. Repair the stored interval as well as the input to the preset.
  if (const auto demand = Statics(kOnDemandClassSlot)) {
    int32_t interval = 0;
    recomp::ReadGuestMemory(&interval, demand, sizeof(interval));
    if (repair_interval && interval > 1 && interval == fps / 30) {
      const int32_t every_frame = 1;
      recomp::WriteGuestMemory(demand, &every_frame, sizeof(every_frame));
      std::fprintf(stderr, "Frame rate: render interval %d -> 1\n", interval);
    }
  }
  const auto statics = Statics(kConfigClassSlot);
  if (!statics) return;
  int32_t render_rate = 0;
  recomp::ReadGuestMemory(&render_rate, statics + 4, 4);
  if (render_rate == 30) {
    recomp::WriteGuestMemory(statics + 4, &fps, 4);
    std::fprintf(stderr, "Frame rate: Config.UnSavePowerTargetRenderFrameRate 30 -> %d (draw every update)\n", fps);
  }
}

inline float FloatResult(addr_t entry) {
  float out = 0;
  Call()(entry, {0}, nullptr, &out);
  return out;
}

inline bool ApplyScenePreset(int preset, bool change = true) {
  // Use the same Config instance as SetPerformanceLevel (3357B4C..3357B70).
  if (!Call()) return false;
  addr_t instance = Statics(0x873a0db0);
  for (auto offset : {0, 16, 16}) {
    if (!instance) return false;
    recomp::ReadGuestMemory(&instance, instance + offset, sizeof(instance));
  }
  if (!instance) return false;
  int level = 0;
  recomp::ReadGuestMemory(&level, instance + 0x18, sizeof(level));
  if (level != preset) {
    if (!change) return false;
    Call()(0x82c7ab70, {instance, addr_t(preset), 0}, nullptr, nullptr);
    std::fprintf(stderr, "Scene route: game graphics level %d -> %d\n", level, preset);
    return false;  // confirm the result on the next actor update
  }
  return true;
}

inline void FollowFrameTime() {
  if (!Call()) return;
  static int32_t last_frame = -1;
  const int32_t frame = int32_t(Call()(kTimeFrameCount, {0}, nullptr, nullptr));
  if (frame == last_frame) return;
  last_frame = frame;
  // FixedProcess runs from ActorController.FixedUpdate, so this step is Unity's
  // whole physics tick: near the frame time characters move every frame, and
  // each extra tick per frame costs main-thread time. Instead of
  // display_patch.c's exact per-frame duration, the step is 1/rate for the
  // smoothed frame rate rounded to 5 fps (30..target, up to 240), about one tick per frame.
  // It changes only once the rate has moved 10 fps or more for 30 consecutive
  // frames, so physics runs on a steady tick. BUNKER_PHYSICS_STEP fixes it.
  static const float forced = [] {
    const char *v = std::getenv("BUNKER_PHYSICS_STEP");
    return v ? float(std::atof(v)) : 0.f;
  }();
  static float smooth = 1 / 30.f, step = 1 / 30.f, pending = 0;
  static int held = 0;
  if (forced > 0) {
    step = forced;
  } else {
    float frame_time = FloatResult(kTimeUnscaledDelta);
    static const bool trace = std::getenv("BUNKER_PHYSICS_TRACE") != nullptr;
    static int calls = 0;
    if (trace && calls++ % 60 == 0)
      std::fprintf(stderr, "Frame rate: frame %d unscaledDelta %.5f fixedDelta %.5f smooth %.5f step %.5f\n", frame,
                   frame_time, FloatResult(kTimeFixedDelta), smooth, step);
    if (!(frame_time > 0)) frame_time = smooth;
    if (frame_time > 0.25f) frame_time = 0.25f;  // a load hitch must not swing the average
    smooth += (frame_time - smooth) * 0.05f;
    int rate = int(std::lround(1 / smooth / 5)) * 5;
    const int ceiling = std::min(Rate(), 240);
    rate = std::clamp(rate, 30, ceiling);
    const int current = int(std::lround(1 / step));
    const float choice = std::abs(rate - current) >= 10 ? 1.f / rate : step;
    if (choice == step) {
      held = 0;
    } else if (held == 0 || std::abs(1 / choice - 1 / pending) >= 5) {
      pending = choice;
      held = 1;
    } else if (pending = choice, ++held >= 30) {
      static int logs = 0;
      if (logs++ < 20) std::fprintf(stderr, "Frame rate: physics step 1/%.0f -> 1/%.0f s\n", 1 / step, 1 / choice);
      step = choice;
      held = 0;
    }
  }
  if (FloatResult(kTimeFixedDelta) != step) {
    float value = step;
    Call()(kTimeSetFixedDelta, {0}, &value, nullptr);
  }
}

// One 30 Hz battle step once 1/30 s of frame time has built up.
inline bool BattleStep() {
  if (!Call()) return true;
  static double built = 0;
  built += 30.0 * FloatResult(kTimeUnscaledDelta);
  if (built < 1.0) return false;
  built = built >= 2.0 ? 0.0 : built - 1.0;  // at most one step per frame, no backlog
  return true;
}

// BattleUpdate.OnStateUpdate only reads the battle (class init, getters) and
// then tail-calls KernelBattle.Process(1.0, 0, null), so a frame without a
// 30 Hz step skips the whole update.
inline Memory *BattleStateUpdate(State &s, addr_t pc, Memory *memory) {
  if (!BattleStep()) {
    s.gpr.pc.qword = s.gpr.x30.qword;  // return to the caller
    return memory;
  }
  return sub_82a6527c(s, pc, memory);
}

// Before PublishGuestOverrides. Only above 30 (the game's own rate needs none).
inline void AddOverrides(int32_t fps) {
  Rate() = fps;
  if (fps <= 30) return;
  // Graphics presets pass their own positive render target here. Unity
  // divides Application.targetFrameRate by it to select a frame interval.
  recomp::AddGuestProbe(0x829dc4a4, [fps](State &s) {
    if (int32_t(s.gpr.x0.dword) > 0) s.gpr.x0.qword = uint32_t(fps);
  });
  // Android computes a second sleep interval here. Its only caller checks
  // d0 <= 0 before waiting (libunity 22BF7C..22BF84). Host frame starts
  // already have deadlines. Keep the real target visible to game scripts.
  if (fps > 120) {
    recomp::AddGuestDirectEntry(0x7022beb4, [](State &s, addr_t, Memory *memory) {
      std::memset(&s.simd.v[0], 0, sizeof(s.simd.v[0]));
      s.gpr.pc.qword = s.gpr.x30.qword;
      return memory;
    });
  }
  recomp::AddGuestProbe(kFixedUpdate, [](State &) { FollowFrameTime(); });
  recomp::AddGuestDirectEntry(kBattleStateUpdate, BattleStateUpdate);
  std::fprintf(stderr, "Frame rate: %d fps hooks (physics step follows frame time, 30 Hz battle steps)\n", fps);
}

}  // namespace bunker::windows::frame_rate
