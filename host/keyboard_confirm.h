#pragma once
// Enter confirms the game's dialogs. Its UI buttons are touch-only (no
// selected EventSystem object, so KEYCODE_ENTER does nothing), so on Enter the
// host samples a few pixels of the presented frame at the dialog button rows
// and taps the confirm button: a bright cream plate (OK / Yes) on the right of
// a two-button dialog or centred on a one-button dialog, under the dialog's
// dark body. Positions are fractions of the 16:9 surface (from the 1280x720
// layout). Portable C++, no platform headers.
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace bunker::windows::keyboard_confirm {

struct Point { float x, y; };  // surface fractions, y down
using Rgba = std::array<uint8_t, 4>;

// Candidate confirm buttons, preferred first: right of two, then centred.
inline constexpr Point kButtons[] = {{779.f / 1280.f, 555.f / 720.f}, {640.f / 1280.f, 555.f / 720.f}};
inline constexpr float kSpread = 40.f / 1280.f;  // plate samples left/right of centre
inline constexpr float kBodyY = 500.f / 720.f;   // dialog body just above the buttons
inline constexpr int kSamplesPerButton = 4;      // left, centre, right, body

// Every pixel to sample, kSamplesPerButton per candidate in kButtons order.
inline std::vector<Point> SamplePoints() {
  std::vector<Point> points;
  for (const auto &b : kButtons) {
    points.push_back({b.x - kSpread, b.y});
    points.push_back({b.x, b.y});
    points.push_back({b.x + kSpread, b.y});
    points.push_back({b.x, kBodyY});
  }
  return points;
}

inline bool Cream(const Rgba &p) {
  const int lo = std::min({p[0], p[1], p[2]}), hi = std::max({p[0], p[1], p[2]});
  return lo >= 180 && hi - lo <= 45;
}
inline bool Dark(const Rgba &p) { return std::max({p[0], p[1], p[2]}) <= 100; }

// The confirm button to tap given SamplePoints() colours, or nothing (then
// Enter is forwarded to the game as a key).
inline std::optional<Point> Choose(const std::vector<Rgba> &samples) {
  if (samples.size() != std::size(kButtons) * kSamplesPerButton) return std::nullopt;
  for (size_t i = 0; i < std::size(kButtons); ++i) {
    const Rgba *s = &samples[i * kSamplesPerButton];
    if (Cream(s[0]) && Cream(s[1]) && Cream(s[2]) && Dark(s[3])) return kButtons[i];
  }
  return std::nullopt;
}

}  // namespace bunker::windows::keyboard_confirm
