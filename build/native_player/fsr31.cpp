#include "fsr31.h"
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace fsr31 {
static void check(uint32_t result, const char *operation) {
    if (result != FFX_API_RETURN_OK)
        throw std::runtime_error(std::string(operation) + " returned " + std::to_string(result));
}

void Upscaler::renderSize(unsigned quality, uint32_t width, uint32_t height, uint32_t& x, uint32_t& y) {
    static constexpr float ratios[] = {1.f, 1.5f, 1.7f, 2.f, 3.f};
    if (quality >= 5 || !width || !height) throw std::invalid_argument("invalid FSR quality or output size");
    x = std::max(1u, uint32_t(width / ratios[quality]));
    y = std::max(1u, uint32_t(height / ratios[quality]));
}

Upscaler::~Upscaler() { close(); }
void Upscaler::close() {
    if (context_) reinterpret_cast<PfnFfxDestroyContext>(destroy_)(&context_, nullptr);
    if (library_) FreeLibrary(library_);
    context_ = nullptr;
    library_ = nullptr;
}

void Upscaler::open(const wchar_t *path, VkPhysicalDevice gpu, VkDevice device, PFN_vkGetDeviceProcAddr getProc,
                    uint32_t w, uint32_t h, uint32_t outW, uint32_t outH, bool reversedDepth) {
    if (library_) throw std::logic_error("FSR context already opened");
    library_ = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library_) throw std::runtime_error("could not load AMD FidelityFX Vulkan runtime");
    const auto create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(library_, "ffxCreateContext"));
    dispatch_ = reinterpret_cast<void*>(GetProcAddress(library_, "ffxDispatch"));
    destroy_ = reinterpret_cast<void*>(GetProcAddress(library_, "ffxDestroyContext"));
    query_ = reinterpret_cast<void*>(GetProcAddress(library_, "ffxQuery"));
    if (!create || !dispatch_ || !destroy_ || !query_) throw std::runtime_error("incomplete FidelityFX API");
    ffxCreateBackendVKDesc backend{{FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK, nullptr}, device, gpu, getProc};
    ffxCreateContextDescUpscale description{};
    description.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, &backend.header};
    description.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    if (reversedDepth) description.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    description.maxRenderSize = {w, h};
    description.maxUpscaleSize = {outW, outH};
    check(create(&context_, &description.header, nullptr), "ffxCreateContext");
    ffxQueryDescUpscaleGetJitterPhaseCount phase{};
    phase.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    phase.renderWidth = w;
    phase.displayWidth = outW;
    phase.pOutPhaseCount = &phases_;
    check(reinterpret_cast<PfnFfxQuery>(query_)(&context_, &phase.header), "FSR jitter phase count");
    if (phases_ <= 0) throw std::runtime_error("invalid FSR jitter sequence");
}

void Upscaler::jitter(uint32_t index, float& x, float& y) const {
    ffxQueryDescUpscaleGetJitterOffset offset{};
    offset.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
    offset.index = int(index % phases_);
    offset.phaseCount = phases_;
    offset.pOutX = &x;
    offset.pOutY = &y;
    check(reinterpret_cast<PfnFfxQuery>(query_)(const_cast<void**>(&context_), &offset.header), "FSR jitter offset");
}

void Upscaler::record(VkCommandBuffer commands, const Frame& frame) {
    auto resource = [](const Image& image, uint32_t state) {
        return ffxApiGetResourceVK(image.handle, ffxApiGetImageResourceDescriptionVK(image.handle, image.description, 0), state);
    };
    ffxDispatchDescUpscale pass{};
    pass.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    pass.commandList = commands;
    pass.color = resource(frame.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    pass.depth = resource(frame.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    pass.motionVectors = resource(frame.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    pass.output = resource(frame.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    const auto input = frame.color.description.extent;
    const auto output = frame.output.description.extent;
    pass.renderSize = {input.width, input.height};
    pass.upscaleSize = {output.width, output.height};
    pass.motionVectorScale = {float(input.width), float(input.height)};
    pass.jitterOffset = {frame.jitterX, frame.jitterY};
    pass.sharpness = std::clamp(frame.sharpness, 0.f, 1.f);
    pass.enableSharpening = pass.sharpness > 0;
    pass.frameTimeDelta = frame.elapsedMs;
    pass.preExposure = 1.f;
    pass.reset = frame.reset;
    pass.cameraNear = frame.nearClip;
    pass.cameraFar = frame.farClip;
    pass.cameraFovAngleVertical = frame.verticalFov;
    pass.viewSpaceToMetersFactor = 1.f;
    check(reinterpret_cast<PfnFfxDispatch>(dispatch_)(&context_, &pass.header), "ffxDispatch");
}
}
