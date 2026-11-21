#pragma once

#include "fsr31.h"
#include <array>
#include <GL/gl.h>
#include <GL/glext.h>

namespace fsr31 {
// Called on Unity's OpenGL render thread. Vulkan and OpenGL share device-local
// textures and semaphores on the same physical GPU. No frame data visits the CPU.
class Interop {
public:
    Interop() = default;
    Interop(const Interop&) = delete;
    Interop& operator=(const Interop&) = delete;
    ~Interop();
    void open(const wchar_t *runtime, uint32_t w, uint32_t h, uint32_t outW, uint32_t outH, bool reversed);
    void dispatch(float jitterX, float jitterY, float elapsedMs, float nearClip, float farClip,
                  float fov, float sharpness, bool reset);
    enum Slot { Color, Depth, Motion, Output };
    GLuint texture(Slot slot) const { return textures_[slot]; }
    Upscaler& upscaler() { return upscaler_; }
private:
    VkInstance instance_{};
    VkDevice device_{};
    VkPhysicalDevice gpu_{};
    VkQueue queue_{};
    uint32_t family_{};
    VkCommandPool pool_{};
    VkCommandBuffer cmd_{};
    std::array<VkCommandBuffer,3> commands_{};
    std::array<VkFence,3> submitted_{};
    unsigned slot_{};
    std::array<VkSemaphore,2> signals_{};
    std::array<GLuint,2> glSignals_{};
    std::array<Image,4> images_{};
    std::array<VkDeviceMemory,4> memory_{};
    std::array<GLuint,4> textures_{}, glMemory_{};
    Upscaler upscaler_;
    bool initialized_{};
    void selectDevice();
    void share(unsigned slot, uint32_t w, uint32_t h, VkFormat format, GLenum glFormat);
    void submit(bool first);
    void barriers(bool acquire, bool first);
};
}
