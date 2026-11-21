#define VK_USE_PLATFORM_WIN32_KHR
#include "fsr31-interop.h"
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace fsr31 {
namespace {
void vkCheck(VkResult result, const char *where) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(where)+" returned "+std::to_string(result));
}
void glCheck(const char *where) {
    if (auto err = glGetError()) throw std::runtime_error(std::string(where)+" GL error "+std::to_string(err));
}
template<class F> F proc(const char *name) {
    auto addr = reinterpret_cast<uintptr_t>(wglGetProcAddress(name));
    if (addr <= 3 || addr == UINTPTR_MAX) throw std::runtime_error(std::string("OpenGL driver lacks ")+name);
    return reinterpret_cast<F>(addr);
}
struct GL {
    PFNGLGETUNSIGNEDBYTEI_VEXTPROC uuid = proc<decltype(uuid)>("glGetUnsignedBytei_vEXT");
    PFNGLCREATEMEMORYOBJECTSEXTPROC createMemory = proc<decltype(createMemory)>("glCreateMemoryObjectsEXT");
    PFNGLMEMORYOBJECTPARAMETERIVEXTPROC memoryParam = proc<decltype(memoryParam)>("glMemoryObjectParameterivEXT");
    PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC importMemory = proc<decltype(importMemory)>("glImportMemoryWin32HandleEXT");
    PFNGLDELETEMEMORYOBJECTSEXTPROC deleteMemory = proc<decltype(deleteMemory)>("glDeleteMemoryObjectsEXT");
    PFNGLCREATETEXTURESPROC createTextures = proc<decltype(createTextures)>("glCreateTextures");
    PFNGLTEXTURESTORAGEMEM2DEXTPROC storage = proc<decltype(storage)>("glTextureStorageMem2DEXT");
    PFNGLTEXTUREPARAMETERIPROC textureParam = proc<decltype(textureParam)>("glTextureParameteri");
    PFNGLGENSEMAPHORESEXTPROC genSignals = proc<decltype(genSignals)>("glGenSemaphoresEXT");
    PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC importSignal = proc<decltype(importSignal)>("glImportSemaphoreWin32HandleEXT");
    PFNGLDELETESEMAPHORESEXTPROC deleteSignals = proc<decltype(deleteSignals)>("glDeleteSemaphoresEXT");
    PFNGLWAITSEMAPHOREEXTPROC wait = proc<decltype(wait)>("glWaitSemaphoreEXT");
    PFNGLSIGNALSEMAPHOREEXTPROC signal = proc<decltype(signal)>("glSignalSemaphoreEXT");
};
GL& gl() { static GL api; return api; }
const GLenum inputLayouts[4] = {GL_LAYOUT_SHADER_READ_ONLY_EXT, GL_LAYOUT_SHADER_READ_ONLY_EXT,
                                 GL_LAYOUT_SHADER_READ_ONLY_EXT, GL_LAYOUT_GENERAL_EXT};
const GLenum writeLayouts[4] = {GL_LAYOUT_GENERAL_EXT, GL_LAYOUT_GENERAL_EXT, GL_LAYOUT_GENERAL_EXT, GL_LAYOUT_GENERAL_EXT};
}

void Interop::selectDevice() {
    uint8_t uuid[VK_UUID_SIZE]{};
    gl().uuid(GL_DEVICE_UUID_EXT, 0, uuid);
    glCheck("OpenGL GPU UUID");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Nier Reincarnation";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    vkCheck(vkCreateInstance(&info, nullptr, &instance_), "vkCreateInstance");
    uint32_t count = 0;
    vkCheck(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "enumerate GPUs");
    std::vector<VkPhysicalDevice> devices(count);
    vkCheck(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), "read GPUs");
    for (auto gpu : devices) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
        vkGetPhysicalDeviceProperties2(gpu, &props);
        if (std::memcmp(uuid, id.deviceUUID, sizeof(uuid)) == 0) { gpu_ = gpu; break; }
    }
    if (!gpu_) throw std::runtime_error("Vulkan could not match the game's OpenGL GPU");
    vkGetPhysicalDeviceQueueFamilyProperties(gpu_, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu_, &count, families.data());
    for (family_ = 0; family_ < count; ++family_)
        if ((families[family_].queueFlags & (VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)) break;
    if (family_ == count) throw std::runtime_error("GPU lacks a graphics/compute queue");
    const float priority = 1;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = family_; q.queueCount = 1; q.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &v12};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v11};
    vkGetPhysicalDeviceFeatures2(gpu_, &features);
    // The 3.1 provider resolves the KHR spellings even on devices where these
    // commands are already part of Vulkan 1.1.
    const char *extensions[] = {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME, VK_KHR_BIND_MEMORY_2_EXTENSION_NAME};
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device.pQueueCreateInfos = &q; device.queueCreateInfoCount = 1; device.pNext = &features;
    device.ppEnabledExtensionNames = extensions; device.enabledExtensionCount = uint32_t(std::size(extensions));
    vkCheck(vkCreateDevice(gpu_, &device, nullptr, &device_), "create Vulkan interop device");
    vkGetDeviceQueue(device_, family_, 0, &queue_);
}

void Interop::share(unsigned slot, uint32_t w, uint32_t h, VkFormat format, GLenum glFormat) {
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    auto& image = images_[slot];
    image.description = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external};
    auto& d = image.description;
    d.imageType = VK_IMAGE_TYPE_2D; d.extent = {w,h,1}; d.format = format; d.mipLevels = 1; d.arrayLayers = 1;
    d.samples = VK_SAMPLE_COUNT_1_BIT; d.tiling = VK_IMAGE_TILING_OPTIMAL;
    d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (slot == Output) d.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    vkCheck(vkCreateImage(device_, &d, nullptr, &image.handle), "create shared frame texture");
    d.pNext = nullptr;
    VkMemoryRequirements required{};
    vkGetImageMemoryRequirements(device_, image.handle, &required);
    VkPhysicalDeviceMemoryProperties types{};
    vkGetPhysicalDeviceMemoryProperties(gpu_, &types);
    uint32_t index = 0;
    for (; index < types.memoryTypeCount; ++index)
        if ((required.memoryTypeBits & (1u<<index)) && (types.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) break;
    if (index == types.memoryTypeCount) throw std::runtime_error("shared texture has no device-local memory type");
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, image.handle};
    VkExportMemoryAllocateInfo exports{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dedicated, external.handleTypes};
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exports, required.size, index};
    vkCheck(vkAllocateMemory(device_, &allocate, nullptr, &memory_[slot]), "allocate shared texture");
    vkCheck(vkBindImageMemory(device_, image.handle, memory_[slot], 0), "bind shared texture");
    const auto get = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(vkGetDeviceProcAddr(device_, "vkGetMemoryWin32HandleKHR"));
    VkMemoryGetWin32HandleInfoKHR hi{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR, nullptr, memory_[slot], VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT};
    HANDLE handle = nullptr;
    vkCheck(get(device_, &hi, &handle), "export frame texture");
    gl().createMemory(1, &glMemory_[slot]);
    GLint yes = GL_TRUE;
    gl().memoryParam(glMemory_[slot], GL_DEDICATED_MEMORY_OBJECT_EXT, &yes);
    gl().importMemory(glMemory_[slot], required.size, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
    glCheck("glImportMemoryWin32HandleEXT");
    gl().createTextures(GL_TEXTURE_2D, 1, &textures_[slot]);
    gl().storage(textures_[slot], 1, glFormat, w, h, glMemory_[slot], 0);
    CloseHandle(handle);
    glCheck("glTextureStorageMem2DEXT");
    gl().textureParam(textures_[slot], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl().textureParam(textures_[slot], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glCheck("import frame texture");
}

void Interop::open(const wchar_t *runtime, uint32_t w, uint32_t h, uint32_t outW, uint32_t outH, bool reversed) {
    if (instance_) throw std::logic_error("FSR interop already opened");
    selectDevice();
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family_};
    vkCheck(vkCreateCommandPool(device_, &pool, nullptr, &pool_), "create FSR command pool");
    VkCommandBufferAllocateInfo cmd{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool_, VK_COMMAND_BUFFER_LEVEL_PRIMARY, uint32_t(commands_.size())};
    vkCheck(vkAllocateCommandBuffers(device_, &cmd, commands_.data()), "allocate FSR command buffers");
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
    for(auto& f:submitted_)vkCheck(vkCreateFence(device_, &fence, nullptr, &f), "create FSR fence");
    cmd_=commands_[slot_];
    vkCheck(vkResetFences(device_,1,&submitted_[slot_]),"initialize FSR fence");
    VkExportSemaphoreCreateInfo external{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, nullptr, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT};
    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &external};
    const auto get = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(vkGetDeviceProcAddr(device_, "vkGetSemaphoreWin32HandleKHR"));
    gl().genSignals(2, glSignals_.data());
    for (unsigned i = 0; i < 2; ++i) {
        vkCheck(vkCreateSemaphore(device_, &sem, nullptr, &signals_[i]), "create shared semaphore");
        VkSemaphoreGetWin32HandleInfoKHR hi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR, nullptr, signals_[i], VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT};
        HANDLE handle = nullptr;
        vkCheck(get(device_, &hi, &handle), "export shared semaphore");
        gl().importSignal(glSignals_[i], GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
        CloseHandle(handle);
    }
    glCheck("import shared semaphores");
    share(Color,w,h,VK_FORMAT_R16G16B16A16_SFLOAT,GL_RGBA16F);
    share(Depth,w,h,VK_FORMAT_R32_SFLOAT,GL_R32F);
    share(Motion,w,h,VK_FORMAT_R16G16_SFLOAT,GL_RG16F);
    share(Output,outW,outH,VK_FORMAT_R16G16B16A16_SFLOAT,GL_RGBA16F);
    upscaler_.open(runtime, gpu_, device_, vkGetDeviceProcAddr, w, h, outW, outH, reversed);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkCheck(vkBeginCommandBuffer(cmd_, &begin), "begin texture initialization");
    barriers(false,true);
    submit(true);
    initialized_ = true;
}

void Interop::barriers(bool acquire, bool first) {
    std::array<VkImageMemoryBarrier,4> transitions{};
    for (unsigned i=0; i<4; ++i) {
        auto& b = transitions[i];
        b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.image = images_[i].handle;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        const auto passLayout = i == Output ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.oldLayout = first ? VK_IMAGE_LAYOUT_UNDEFINED : passLayout;
        b.newLayout = acquire ? passLayout : VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_EXTERNAL : family_;
        b.dstQueueFamilyIndex = acquire ? family_ : VK_QUEUE_FAMILY_EXTERNAL;
        if (acquire) b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        else if (!first) b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(cmd_, acquire || first ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        acquire ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
        0,nullptr,0,nullptr,4,transitions.data());
}

void Interop::submit(bool first) {
    vkCheck(vkEndCommandBuffer(cmd_), "end FSR commands");
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = first ? 0 : 1; submit.pWaitSemaphores = &signals_[0]; submit.pWaitDstStageMask = &stage;
    submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd_; submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &signals_[1];
    vkCheck(vkQueueSubmit(queue_, 1, &submit, submitted_[slot_]), "submit FSR to GPU");
    gl().wait(glSignals_[1],0,nullptr,4,textures_.data(),writeLayouts);
    glCheck("wait for FSR output");
}

void Interop::dispatch(float x, float y, float ms, float nearClip, float farClip, float fov, float sharp, bool reset) {
    if (!initialized_) throw std::logic_error("FSR is not initialized");
    slot_=(slot_+1)%commands_.size();
    cmd_=commands_[slot_];
    vkCheck(vkWaitForFences(device_,1,&submitted_[slot_],VK_TRUE,5000000000ULL), "recycle FSR command buffer");
    vkCheck(vkResetFences(device_,1,&submitted_[slot_]), "reset FSR fence");
    vkCheck(vkResetCommandBuffer(cmd_,0), "reset FSR commands");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkCheck(vkBeginCommandBuffer(cmd_, &begin), "begin FSR frame");
    barriers(true,false);
    upscaler_.record(cmd_, {images_[0], images_[1], images_[2], images_[3], x,y,ms,nearClip,farClip,fov,sharp,reset});
    barriers(false,false);
    gl().signal(glSignals_[0],0,nullptr,4,textures_.data(),inputLayouts);
    glFlush();
    glCheck("release scene inputs to FSR");
    submit(false);
}

Interop::~Interop() {
    if (device_) {
        glFlush();
        vkDeviceWaitIdle(device_);
        glFinish();
        // The provider must release its Vulkan pipelines before the device.
        upscaler_.close();
        glDeleteTextures(4,textures_.data());
        if (glMemory_[0]) gl().deleteMemory(4,glMemory_.data());
        if (glSignals_[0]) gl().deleteSignals(2,glSignals_.data());
        for (unsigned i=0; i<4; ++i) {
            if (images_[i].handle) vkDestroyImage(device_,images_[i].handle,nullptr);
            if (memory_[i]) vkFreeMemory(device_,memory_[i],nullptr);
        }
        for (auto signal: signals_) if (signal) vkDestroySemaphore(device_,signal,nullptr);
        for(auto f:submitted_)if(f)vkDestroyFence(device_,f,nullptr);
        if (pool_) vkDestroyCommandPool(device_,pool_,nullptr);
        vkDestroyDevice(device_,nullptr);
    }
    if (instance_) vkDestroyInstance(instance_,nullptr);
}
}
