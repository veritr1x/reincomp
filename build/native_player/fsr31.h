#pragma once

#include <windows.h>
#include <cstdint>
#include <vulkan/vulkan.h>

namespace fsr31 {

struct Image {
    VkImage handle;
    VkImageCreateInfo description;
};

// Motion vectors are current-to-previous UV offsets, excluding camera jitter.
// All images must belong to the supplied device and command buffer's queue family.
struct Frame {
    Image color, depth, motion, output;
    float jitterX, jitterY, elapsedMs, nearClip, farClip, verticalFov;
    float sharpness;
    bool reset;
};

class Upscaler {
public:
    Upscaler() = default;
    Upscaler(const Upscaler&) = delete;
    Upscaler& operator=(const Upscaler&) = delete;
    ~Upscaler();
    void close();
    void open(const wchar_t *library, VkPhysicalDevice gpu, VkDevice device, PFN_vkGetDeviceProcAddr getProc,
              uint32_t renderWidth, uint32_t renderHeight, uint32_t outputWidth, uint32_t outputHeight,
              bool reversedDepth);
    void record(VkCommandBuffer commands, const Frame& frame);
    void jitter(uint32_t frameIndex, float& x, float& y) const;
    static void renderSize(unsigned quality, uint32_t width, uint32_t height, uint32_t& x, uint32_t& y);

private:
    HMODULE library_{};
    void *context_{};
    void *dispatch_{}, *destroy_{}, *query_{};
    int phases_{};
};
}
