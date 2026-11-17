#pragma once
// Keyboard controls for the touch-only game. The character moves by dragging
// anywhere on screen (a floating stick), so held direction keys become one
// held drag from the screen centre toward their combined direction.
// Keys are Win32 virtual-key codes; a Linux window backend maps to the same.
// Portable C++, no platform headers.
#include <cmath>
#include <cstdint>
#include <utility>

namespace bunker::windows::keyboard_touch {

enum Bits : uint8_t { kUp = 1, kDown = 2, kLeft = 4, kRight = 8 };

// W/A/S/D and the arrow keys; 0 for anything else.
inline uint8_t Direction(uint32_t vk) {
  switch (vk) {
    case 0x57: case 0x26: return kUp;     // W, Up
    case 0x53: case 0x28: return kDown;   // S, Down
    case 0x41: case 0x25: return kLeft;   // A, Left
    case 0x44: case 0x27: return kRight;  // D, Right
    default: return 0;
  }
}

// One frame of finger motion from (x, y) toward (tx, ty), at most `max_step` px.
inline std::pair<float, float> Step(float x, float y, float tx, float ty, float max_step) {
  const float dx = tx - x, dy = ty - y, length = std::sqrt(dx * dx + dy * dy);
  if (length <= max_step || length == 0) return {tx, ty};
  return {x + dx * max_step / length, y + dy * max_step / length};
}

struct Target {
  bool active = false;
  float origin_x = 0, origin_y = 0, x = 0, y = 0;
};

class Stick {
 public:
  // Tracks each key separately so W and Up held together release cleanly.
  void Set(uint32_t vk, bool down) {
    const int slot = Slot(vk);
    if (slot >= 0) held_[slot] = down;
  }
  bool Any() const {
    for (bool held : held_)
      if (held) return true;
    return false;
  }
  // Drag for a `width` x `height` surface (pixels, y down). Opposite keys
  // cancel; diagonals keep the same radius.
  Target Target(float width, float height) const {
    struct Target t;
    t.origin_x = width * 0.5f;
    t.origin_y = height * 0.5f;
    uint8_t bits = 0;
    for (int i = 0; i < 8; ++i)
      if (held_[i]) bits |= Direction(kKeys[i]);
    float dx = float((bits & kRight) != 0) - float((bits & kLeft) != 0);
    float dy = float((bits & kDown) != 0) - float((bits & kUp) != 0);
    t.active = bits != 0;  // held, even if opposite keys cancel (stand still)
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length > 0) { dx /= length; dy /= length; }
    const float radius = height * 0.15f;
    t.x = t.origin_x + dx * radius;
    t.y = t.origin_y + dy * radius;
    return t;
  }

 private:
  static constexpr uint32_t kKeys[8] = {0x57, 0x26, 0x53, 0x28, 0x41, 0x25, 0x44, 0x27};
  static int Slot(uint32_t vk) {
    for (int i = 0; i < 8; ++i)
      if (kKeys[i] == vk) return i;
    return -1;
  }
  bool held_[8] = {};
};

}  // namespace bunker::windows::keyboard_touch
