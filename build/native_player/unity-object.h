#pragma once
#include <cstring>

// Global 3.7.1 UnityEngine.Object.GetCachedPtr reads the native object
// immediately after the two-pointer IL2CPP object header. Destroyed scene
// components can still have a reachable managed wrapper.
inline bool nativeUnityObjectAlive(const void *object) {
    if (!object) return false;
    void *native = nullptr;
    std::memcpy(&native, static_cast<const char *>(object) + 2 * sizeof(void *), sizeof(native));
    return native != nullptr;
}
