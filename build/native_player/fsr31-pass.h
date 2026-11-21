#pragma once
#include <array>
#include <cstdint>

namespace fsr31 {
using Matrix = std::array<float,16>;
struct SceneFrame {
    uint32_t index, width, height, renderWidth, renderHeight;
    uintptr_t camera;
    uint32_t sceneColor,sceneDepth,destination;
    int scene;
    float nearClip, farClip, verticalFov, jitterX, jitterY, sharpness;
    Matrix clipFromWorld, worldFromClip;
    bool reversed;
};
enum RenderEvent { Composite };
void __stdcall renderEvent(int event, void *data);
void disable(const char *reason);
void status(const char *format, ...);
}
