#pragma once
#include <array>
#include <cstdint>

namespace fsr31 {
using Matrix = std::array<float,16>;
struct SceneFrame {
    uint32_t index, width, height, renderWidth, renderHeight;
    uintptr_t camera;
    uint32_t sceneColor,sceneDepth,destination;
    uint32_t generation{}, directOutput{};
    bool copyOutput=true;
    int scene;
    float nearClip, farClip, verticalFov, jitterX, jitterY, sharpness;
    Matrix clipFromWorld, worldFromClip;
    bool reversed;
};
enum RenderEvent { Composite };
uint32_t sharedOutput(uint32_t generation);
void __stdcall renderEvent(int event, void *data);
void __stdcall releasePass(int event);
void disable(const char *reason);
void status(const char *format, ...);
}
