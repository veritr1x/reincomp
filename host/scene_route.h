#pragma once
#include "../runtime/runtime.h"
#include "game_frame_rate.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace bunker::windows {

// Scripted routes use the same touch injector as a click in the game window.
// These callbacks identify real game states in the pinned global 3.7.1 build.
class SceneRoute {
  using Clock = std::chrono::steady_clock;
  std::string target;
  std::filesystem::path evidence;
  std::atomic<bool> title_wait{false}, title_leaving{false};
  std::atomic<bool> actor_seen{false};
  std::atomic<bool> loading{false};
  std::atomic<bool> apply_preset{false}, preset_confirmed{false};
  int preset = 0;
  Clock::time_point started = Clock::now(), title_seen{}, arrived{};
  bool down = false, tapped = false;
  std::atomic<bool> captured{false};
 public:
  SceneRoute() {
    if (const char *value = std::getenv("REINCOMP_SCENE_ROUTE")) target = value;
    if (target.empty()) return;
    if (target != "title" && target != "field" && target != "storybook")
      throw std::invalid_argument("unknown scene route");
    if (target != "title") {
      if (const char *quality = std::getenv("REINCOMP_BENCH_QUALITY")) {
        if (std::strcmp(quality, "low") == 0) preset = 4;
        else if (std::strcmp(quality, "highest") == 0) preset = 1;
        else throw std::invalid_argument("unknown scene graphics preset");
      }
    }
    const auto *path = std::getenv("REINCOMP_SCENE_CAPTURE");
    if (!path || !(evidence = std::filesystem::u8path(path)).is_absolute() ||
        std::filesystem::exists(evidence)) throw std::invalid_argument("scene capture requires a new absolute PNG path");
    recomp::AddGuestProbe(0x82f102e4, [this](State &) { title_wait = true; }); // TitleScreen.WaitTapScreen
    recomp::AddGuestProbe(0x82f1038c, [this](State &) { title_leaving = true; }); // TitleScreen.PlayTitleOut
    recomp::AddGuestProbe(0x8306e628, [this](State &) { loading = true; }); // LoadingPresenter.PlayInAsync
    recomp::AddGuestProbe(0x8306e6d4, [this](State &) { loading = false; }); // LoadingPresenter.PlayOutAsync
    recomp::AddGuestProbe(0x82d62444, [this](State &state) { // Dark.Animation.Actor.ActorController.Update
      if (captured.load(std::memory_order_relaxed) || target == "title") return;
      // ActorController.IsInPictureBook in the pinned global player.
      uint8_t picturebook = 0;
      recomp::ReadGuestMemory(&picturebook, state.gpr.x0.qword + 0x213, sizeof(picturebook));
      if (bool(picturebook) != (target == "storybook")) return;
      if (apply_preset.load(std::memory_order_relaxed))
        preset_confirmed.store(frame_rate::ApplyScenePreset(preset), std::memory_order_relaxed);
      actor_seen.store(true, std::memory_order_relaxed);
    });
  }

  template<class Tap, class Capture> void AfterFrame(Tap tap, Capture capture) {
    if (target.empty() || captured) return;
    const auto now = Clock::now();
    if (title_wait && title_seen == Clock::time_point{}) title_seen = now;
    const bool title_ready = title_wait && now - title_seen >= std::chrono::seconds(2);
    if (target != "title" && title_ready && !tapped) {
      if (down || !title_leaving) tap(!down);
      tapped = down || title_leaving;
      down = !tapped;
    }
    const bool actor_updated = actor_seen.exchange(false, std::memory_order_relaxed);
    const bool ready = target == "title" ? title_ready && !title_leaving : tapped && !loading && actor_updated;
    if (ready && arrived == Clock::time_point{}) arrived = now;
    if (!ready) arrived = {};
    // The prologue actor updates beneath its opening quotation. Let that
    // sequence finish before measuring the side-scrolling scene.
    const auto settle_time = std::chrono::seconds(target == "storybook" ? 45 : 5);
    if (preset && ready && now - arrived >= settle_time)
      apply_preset.store(true, std::memory_order_relaxed);
    const bool settled = ready && now - arrived >= settle_time + std::chrono::seconds(preset ? 3 : 0) &&
                         (!preset || preset_confirmed.load(std::memory_order_relaxed));
    if (settled || now - started > std::chrono::minutes(3)) {
      capture(evidence.wstring());
      apply_preset.store(false, std::memory_order_relaxed);
      captured = true;
      std::fprintf(stderr, "Scene route: %s %s; capture requested\n", target.c_str(), settled ? "arrived" : "timed out");
    }
  }
};
}
