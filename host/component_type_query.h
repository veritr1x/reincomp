#pragma once
#include "../runtime/runtime.h"

namespace bunker::windows {

// Only UnityMain publishes this pointer, for the duration of one nativeRender.
// An IL2CPP class pair describes a type relationship, independent of instances.
struct ComponentTypeQueryFrame {
  struct Entry {
    addr_t component_class, requested_class, result;
  };
  Entry entries[256]{};
  static inline thread_local ComponentTypeQueryFrame *current = nullptr;
  ComponentTypeQueryFrame() : previous(current) { current = this; }
  ~ComponentTypeQueryFrame() { current = previous; }
  ComponentTypeQueryFrame(const ComponentTypeQueryFrame &) = delete;
  ComponentTypeQueryFrame &operator=(const ComponentTypeQueryFrame &) = delete;

 private:
  ComponentTypeQueryFrame *previous;
};

}
