#include "fsr31.h"
#include "fsr31-pass.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace fsr31 {
namespace {
using Object = void*;
using Visit = void(*)(void(*)(const char*,void*));
std::map<std::string,void*> functions;
template<class Function> Function icall(const char *name) {
    static auto visit = reinterpret_cast<Visit>(GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),"VisitRegisteredCalls"));
    if (functions.empty() && visit) visit([](const char *key,void *value){functions.emplace(key,value);});
    return reinterpret_cast<Function>(functions.at(name));
}
template<class Function> Function exported(const char *name) {
    auto result = GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),name);
    if (!result) throw std::runtime_error(std::string("Missing Unity function ")+name);
    return reinterpret_cast<Function>(result);
}
std::wstring text(Object value) {
    if (!value) return {};
    static const auto length = exported<int(*)(Object)>("il2cpp_string_length");
    static const auto chars = exported<const wchar_t*(*)(Object)>("il2cpp_string_chars");
    const int n = length(value);
    return n>0 && n<256 ? std::wstring(chars(value),n) : std::wstring();
}
int property(const wchar_t *name) {
    static const auto make = exported<Object(*)(const wchar_t*,int)>("il2cpp_string_new_utf16");
    static const auto id = icall<int(*)(Object)>("UnityEngine.Shader::PropertyToID");
    return id(make(name,int(wcslen(name))));
}
struct Target {
    int type=2, name=-1, instance=0;
    void *buffer=nullptr;
    int mip=0,face=-1,slice=0;
};
static_assert(sizeof(Target)==40);
// Unity 2019.4 RenderTextureDescriptor and RenderTargetIdentifier layouts.
struct Descriptor {
    int width,height,samples=1,volume=1,mips=1,format,stencil=0,depthBits=0;
    int dimension=2,shadow=0,vr=0,flags=32,memoryless=0;
};
static_assert(sizeof(Descriptor)==0x34);
struct Surface {
    Object texture{};
    Target target{};
    uint32_t handle{},name{};
};
Surface targets[3];
uint32_t generation=0;
// Each wrapper belongs to one set of render textures. Keep both the managed
// references and the Unity texture alive across Resources.UnloadUnusedAssets.
struct ExternalOutput {
    Object texture{},properties{};
    uint32_t textureHandle{},propertiesHandle{},name{};
    bool attempted=false;

    void retire() {
        if(texture){
            static const auto destroy=icall<void(*)(Object,float)>("UnityEngine.Object::Destroy");
            destroy(texture,0.f);
        }
        static const auto free=exported<void(*)(uint32_t)>("il2cpp_gchandle_free");
        if(propertiesHandle)free(propertiesHandle);
        if(textureHandle)free(textureHandle);
        *this={};
    }
    Object bind(uint32_t next,int width,int height) {
        if(!next)return nullptr;
        if(attempted)return name==next ? properties : nullptr;
        attempted=true;
        static const auto domain=exported<Object(*)()>("il2cpp_domain_get");
        static const auto assembly=exported<Object(*)(Object,const char*)>("il2cpp_domain_assembly_open");
        static const auto image=exported<Object(*)(Object)>("il2cpp_assembly_get_image");
        static const auto cls=exported<Object(*)(Object,const char*,const char*)>("il2cpp_class_from_name");
        static const auto method=exported<Object(*)(Object,const char*,int)>("il2cpp_class_get_method_from_name");
        static const auto invoke=exported<Object(*)(Object,Object,Object*,Object*)>("il2cpp_runtime_invoke");
        static const auto allocate=exported<Object(*)(Object)>("il2cpp_object_new");
        static const auto keep=exported<uint32_t(*)(Object,bool)>("il2cpp_gchandle_new");
        static const auto flags=icall<void(*)(Object,int)>("UnityEngine.Object::set_hideFlags");
        static const auto native=icall<void*(*)(Object)>("UnityEngine.Texture::GetNativeTexturePtr");
        static const auto set=icall<void(*)(Object,int,Object)>("UnityEngine.MaterialPropertyBlock::SetTextureImpl");
        static const auto core=assembly(domain(),"UnityEngine.CoreModule");
        static const auto types=core?image(core):nullptr;
        static const auto textureClass=types?cls(types,"UnityEngine","Texture2D"):nullptr;
        static const auto blockClass=types?cls(types,"UnityEngine","MaterialPropertyBlock"):nullptr;
        static const auto create=textureClass?method(textureClass,"CreateExternalTexture",6):nullptr;
        static const auto construct=blockClass?method(blockClass,".ctor",0):nullptr;
        if(!create || !construct)throw std::runtime_error("missing external texture or draw property constructor");
        int rgbaHalf=17;
        bool mipmaps=false,linear=true;
        uintptr_t glName=next;
        Object error=nullptr,args[]{&width,&height,&rgbaHalf,&mipmaps,&linear,&glName};
        texture=invoke(create,nullptr,args,&error);
        if(error || !texture)throw std::runtime_error("could not wrap the FSR output");
        textureHandle=keep(texture,false);
        flags(texture,32); // HideFlags.DontUnloadUnusedAsset
        if(uintptr_t(native(texture))!=next)throw std::runtime_error("external FSR texture name mismatch");
        properties=allocate(blockClass);
        if(!properties)throw std::runtime_error("could not allocate final draw properties");
        propertiesHandle=keep(properties,false);
        invoke(construct,properties,nullptr,&error);
        if(error)throw std::runtime_error("could not initialize final draw properties");
        set(properties,property(L"_BlitTex"),texture);
        set(properties,property(L"_MainTex"),texture);
        name=next;
        status("Direct FSR output ready: %dx%d",width,height);
        return properties;
    }
} externalOutput;
std::array<uint32_t,4> dimensions{};
bool resizing(uint32_t width,uint32_t height) {
    static std::array<uint32_t,2> pending{};
    static ULONGLONG since=0;
    if(!generation || (dimensions[2]==width && dimensions[3]==height)){
        pending={};
        return false;
    }
    const std::array<uint32_t,2> size{width,height};
    const auto now=GetTickCount64();
    if(size!=pending){pending=size;since=now;}
    // Render natively while a drag is changing the client size. Recreating
    // the Vulkan device for every intermediate WM_SIZE stalls the render queue.
    return now-since<160;
}
void prepareTargets(const SceneFrame& data) {
    std::array<uint32_t,4> next={data.renderWidth,data.renderHeight,data.width,data.height};
    if(next==dimensions)return;
    externalOutput.retire();
    static const auto create=icall<Object(*)(const Descriptor*)>("UnityEngine.RenderTexture::GetTemporary_Internal_Injected");
    static const auto release=icall<void(*)(Object)>("UnityEngine.RenderTexture::ReleaseTemporary");
    static const auto idOffset=icall<int(*)()>("UnityEngine.Object::GetOffsetOfInstanceIDInCPlusPlusObject")();
    static const auto native=icall<void*(*)(Object)>("UnityEngine.Texture::GetNativeTexturePtr");
    static const auto realize=icall<uint8_t(*)(Object)>("UnityEngine.RenderTexture::Create");
    static const auto isCreated=icall<uint8_t(*)(Object)>("UnityEngine.RenderTexture::IsCreated");
    static const auto keep=exported<uint32_t(*)(Object,bool)>("il2cpp_gchandle_new");
    static const auto free=exported<void(*)(uint32_t)>("il2cpp_gchandle_free");
    for (unsigned i=0;i<3;++i){
        auto& t=targets[i];
        if(t.texture){release(t.texture);free(t.handle);t=Surface{};}
        Descriptor d{int(i==2?data.width:data.renderWidth),int(i==2?data.height:data.renderHeight)};
        d.format=i==1?49:48;
        t.texture=create(&d);
        if(!t.texture)throw std::runtime_error("Unity could not allocate FSR render textures");
        t.handle=keep(t.texture,false);
        void *object=nullptr;
        std::memcpy(&object,static_cast<char*>(t.texture)+2*sizeof(void*),sizeof(object));
        if(!object || idOffset<=0 || idOffset>256)throw std::runtime_error("Unity render texture identity unavailable");
        t.target.type=-2;
        std::memcpy(&t.target.instance,static_cast<char*>(object)+idOffset,sizeof(int));
        if(!isCreated(t.texture) && !realize(t.texture))throw std::runtime_error("Unity could not create the FSR render target");
        t.name=uint32_t(reinterpret_cast<uintptr_t>(native(t.texture)));
        if(!t.name)throw std::runtime_error("Unity did not expose the OpenGL texture for target "+std::to_string(i));
    }
    dimensions=next;
    ++generation;
}
std::atomic<bool> failed{};
Object camera{}, command{};
Target source{};
SceneFrame frame{};
// Unity records the event and draws into the same command buffer. Finish the
// choice of output while recording, before ExecuteCommandBuffer submits it.
SceneFrame *recording{};
bool needsCopiedOutput=false;
Matrix originalProjection{};
float originalCameraRect[4]{};
bool internalRect=false;
void restoreRect(){
    if(!internalRect)return;
    static const auto set=icall<void(*)(Object,const float*)>("UnityEngine.Camera::set_rect_Injected");
    set(camera,originalCameraRect);
    internalRect=false;
}
Object asset{};
float originalScale{};
uint8_t originalDepth{};
bool modified=false;
bool jittered=false;
unsigned quality=1, sharpness=20;
int setting(const char *key,int fallback,int limit) {
    const char *value=std::getenv(key);
    if (!value || !*value) return fallback;
    char *end=nullptr;long n=std::strtol(value,&end,10);
    return !*end && n>=0 && n<=limit ? int(n) : fallback;
}
Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix out{};
    for(unsigned col=0; col<4; ++col) for(unsigned row=0; row<4; ++row)
        for(unsigned k=0; k<4; ++k) out[4*col+row] += a[4*k+row]*b[4*col+k];
    return out;
}
float halton(unsigned index,unsigned base) {
    float value=0, scale=1;
    for(; index; index/=base) {scale/=base;value+=(index%base)*scale;}
    return value-.5f;
}
void restore() {
    restoreRect();
    if(jittered){
        static const auto reset=icall<void(*)(Object)>("UnityEngine.Camera::ResetProjectionMatrix");
        static const auto get=icall<void(*)(Object,float*)>("UnityEngine.Camera::get_projectionMatrix_Injected");
        static const auto projection=icall<void(*)(Object,const float*)>("UnityEngine.Camera::set_projectionMatrix_Injected");
        reset(camera);
        Matrix automatic;
        get(camera,automatic.data());
        if(!std::equal(automatic.begin(),automatic.end(),originalProjection.begin(),[](float a,float b){return std::abs(a-b)<1e-6f;}))
            projection(camera,originalProjection.data());
        jittered=false;
    }
    if(modified){
        std::memcpy(static_cast<char*>(asset)+0x48,&originalScale,sizeof(float));
        *(static_cast<uint8_t*>(asset)+0x38)=originalDepth;
        modified=false;
    }
    camera=nullptr;command=nullptr;recording=nullptr;
}
void prepare(Object cam) {
    restore();
    if(failed.load(std::memory_order_relaxed) || !cam)return;
    static const auto activeScene=icall<void(*)(int*)>("UnityEngine.SceneManagement.SceneManager::GetActiveScene_Injected");
    static const auto sceneName=icall<Object(*)(int)>("UnityEngine.SceneManagement.Scene::GetNameInternal");
    activeScene(&frame.scene);
    static std::optional<int> lastScene;
    static bool menu=false;
    if(lastScene!=frame.scene){
        lastScene=frame.scene;
        // Mama's room and the out-of-game tabs share the "default" scene.
        // Their text and banners are drawn by the perspective scene camera.
        menu=text(sceneName(frame.scene))==L"default";
        if(menu)status("Rendering menu scene at native resolution");
    }
    if(menu)return;
    static const auto target=icall<Object(*)(Object)>("UnityEngine.Camera::get_targetTexture");
    static const auto ortho=icall<uint8_t(*)(Object)>("UnityEngine.Camera::get_orthographic");
    static const auto stereo=icall<uint8_t(*)(Object)>("UnityEngine.Camera::get_stereoEnabled");
    static const auto camType=icall<int(*)(Object)>("UnityEngine.Camera::get_cameraType");
    if(target(cam) || ortho(cam) || stereo(cam) || camType(cam)!=1)return;
    static const auto current=icall<Object(*)()>("UnityEngine.Rendering.GraphicsSettings::get_INTERNAL_currentRenderPipeline");
    asset=current();
    if(!asset)return;
    static const auto klass=exported<Object(*)(Object)>("il2cpp_object_get_class");
    static const auto field=exported<Object(*)(Object,const char*)>("il2cpp_class_get_field_from_name");
    static const auto offset=exported<int(*)(Object)>("il2cpp_field_get_offset");
    static Object verified=nullptr;
    if(klass(asset)!=verified){
        auto type=klass(asset), scale=field(type,"m_RenderScale"), depth=field(type,"m_RequireDepthTexture");
        if(!scale || !depth || offset(scale)!=0x48 || offset(depth)!=0x38)
            throw std::runtime_error("the active pipeline does not expose LWRP scene resolution and depth");
        verified=type;
        quality=setting("REINCOMP_FSR_QUALITY",1,4);
        sharpness=setting("REINCOMP_FSR_SHARPNESS",20,100);
        status("LWRP camera selected, quality=%u sharpness=%u",quality,sharpness);
    }
    static const auto w=icall<int(*)(Object)>("UnityEngine.Camera::get_pixelWidth");
    static const auto h=icall<int(*)(Object)>("UnityEngine.Camera::get_pixelHeight");
    static const auto screenW=icall<int(*)()>("UnityEngine.Screen::get_width");
    static const auto screenH=icall<int(*)()>("UnityEngine.Screen::get_height");
    int width=w(cam),height=h(cam);
    if(width<=0 || height<=0 || width!=screenW() || height!=screenH())return;
    if(resizing(width,height))return;
    static const auto getRect=icall<void(*)(Object,float*)>("UnityEngine.Camera::get_rect_Injected");
    getRect(cam,originalCameraRect);
    if(std::abs(originalCameraRect[0])>1.e-5f || std::abs(originalCameraRect[1])>1.e-5f ||
       std::abs(originalCameraRect[2]-1.f)>1.e-5f || std::abs(originalCameraRect[3]-1.f)>1.e-5f)return;
    static const auto proj=icall<void(*)(Object,float*)>("UnityEngine.Camera::get_projectionMatrix_Injected");
    static const auto setProjection=icall<void(*)(Object,const float*)>("UnityEngine.Camera::set_projectionMatrix_Injected");
    static const auto view=icall<void(*)(Object,float*)>("UnityEngine.Camera::get_worldToCameraMatrix_Injected");
    static const auto gpuProjection=icall<void(*)(const float*,bool,float*)>("UnityEngine.GL::GetGPUProjectionMatrix_Injected");
    static const auto inverse=icall<void(*)(const float*,float*)>("UnityEngine.Matrix4x4::Inverse_Injected");
    static const auto nearClip=icall<float(*)(Object)>("UnityEngine.Camera::get_nearClipPlane");
    static const auto farClip=icall<float(*)(Object)>("UnityEngine.Camera::get_farClipPlane");
    static const auto fov=icall<float(*)(Object)>("UnityEngine.Camera::get_fieldOfView");
    static const auto count=icall<int(*)()>("UnityEngine.Time::get_frameCount");
    camera=cam;
    frame.index=count(); frame.width=width; frame.height=height; frame.camera=reinterpret_cast<uintptr_t>(cam);
    Upscaler::renderSize(quality,width,height,frame.renderWidth,frame.renderHeight);
    std::memcpy(&originalScale,static_cast<char*>(asset)+0x48,sizeof(float));
    originalDepth=*(static_cast<uint8_t*>(asset)+0x38);
    // The camera rect sizes both the LWRP attachments and the scene
    // viewport. Applying renderScale as well would scale the scene twice.
    const float unit=1.f;
    std::memcpy(static_cast<char*>(asset)+0x48,&unit,sizeof(float));
    *(static_cast<uint8_t*>(asset)+0x38)=1; modified=true;
    proj(cam,originalProjection.data());
    Matrix worldToView,clipFromView;
    view(cam,worldToView.data());gpuProjection(originalProjection.data(),true,clipFromView.data());
    frame.clipFromWorld=multiply(clipFromView,worldToView);
    inverse(frame.clipFromWorld.data(),frame.worldFromClip.data());
    unsigned phase=frame.index%std::max(1u,unsigned(8.f*width/float(frame.renderWidth)*width/float(frame.renderWidth)))+1;
    frame.jitterX=halton(phase,2);frame.jitterY=halton(phase,3);
    auto jitter=originalProjection;
    jitter[8] += 2*frame.jitterX/frame.renderWidth; jitter[9] += 2*frame.jitterY/frame.renderHeight;
    setProjection(cam,jitter.data()); jittered=true;
    frame.nearClip=nearClip(cam);frame.farClip=farClip(cam);frame.verticalFov=fov(cam)*.01745329252f;
    frame.sharpness=sharpness*.01f;frame.reversed=false; // Desktop OpenGL uses forward Z.
    if(frame.renderWidth != frame.width || frame.renderHeight != frame.height){
        static const auto set=icall<void(*)(Object,const float*)>("UnityEngine.Camera::set_pixelRect_Injected");
        const float rect[4]={0,0,float(frame.renderWidth),float(frame.renderHeight)};
        set(cam,rect);
        internalRect=true;
    }
}
}

void status(const char *format,...) {
    static FILE *log=[]() -> FILE* {
        const auto data = _wgetenv(L"REINCOMP_PLAYER_DATA");
        return data && *data ? _wfopen((std::filesystem::path(data)/L"fsr31.log").c_str(),L"w") : nullptr;
    }();
    if(!log)return;
    va_list args;va_start(args,format);std::vfprintf(log,format,args);va_end(args);
    std::fputc('\n',log);std::fflush(log);
}
void disable(const char *reason) {
    if(!failed.exchange(true))status("FSR disabled: %s",reason);
}
extern "C" __declspec(dllexport) void FsrBeforeCull(Object cam) {
    try {prepare(cam);} catch(const std::exception& error){disable(error.what());}
}
extern "C" __declspec(dllexport) const Target* FsrSetTexture(Object cmd,int id,const Target *target) {
    if(!camera || failed.load(std::memory_order_relaxed) || !target)return target;
    try {
        static const int blit=property(L"_BlitTex");
        static const auto name=icall<Object(*)(Object)>("UnityEngine.Rendering.CommandBuffer::get_name");
        if(id!=blit)return target;
        auto pass=text(name(cmd));
        std::transform(pass.begin(),pass.end(),pass.begin(),::towlower);
        if(pass.find(L"final")==pass.npos || pass.find(L"blit")==pass.npos)return target;
        restoreRect();
        source=*target;
        static bool identified=false;
        if(!identified){status("Final blit identified, source type=%d property=%d",source.type,source.name);identified=true;}
        struct Vec2 {float x,y;};
        static const auto recordBlit=icall<void(*)(Object,const Target*,const Target*,Object,int,const Vec2*,const Vec2*,int,int)>(
            "UnityEngine.Rendering.CommandBuffer::Blit_Identifier_Injected");
        static const auto issue=icall<void(*)(Object,decltype(&renderEvent),int,void*)>(
            "UnityEngine.Rendering.CommandBuffer::IssuePluginEventAndDataInternal");
        static const int depth=property(L"_CameraDepthTexture");
        Target depthTarget{};depthTarget.type=-4;depthTarget.name=depth;
        prepareTargets(frame);
        frame.generation=generation;
        frame.sceneColor=targets[0].name;frame.sceneDepth=targets[1].name;frame.destination=targets[2].name;
        const Vec2 scale{1,1},offset{0,0};
        auto copy=[&](const Target& a,const Target& b){recordBlit(cmd,&a,&b,nullptr,-1,&scale,&offset,0,0);};
        copy(source,targets[0].target);
        copy(depthTarget,targets[1].target);
        auto pending=std::make_unique<SceneFrame>(frame);
        issue(cmd,renderEvent,Composite,pending.get());
        recording=pending.release();
        needsCopiedOutput=false;
        command=cmd;
        return &targets[2].target;
    }catch(const std::exception& error){disable(error.what());return target;}
}
// The two staging blits above target internal-resolution textures. The
// fullscreen mesh in LWRP's final pass must cover the camera output even if
// Unity retained the last staging viewport across a target switch or resize.
extern "C" __declspec(dllexport) void FsrFinalViewport(Object cmd) {
    if (!camera || !command || cmd != command) return;
    static const auto set = icall<void(*)(Object,const float*)>(
        "UnityEngine.Rendering.CommandBuffer::SetViewport_Injected");
    const float viewport[] = {0.f, 0.f, float(frame.width), float(frame.height)};
    set(cmd, viewport);
}
extern "C" __declspec(dllexport) const Target* FsrBlitSource(Object cmd,const Target *target) {
    if(cmd==command && camera && target && target->type==source.type && target->name==source.name &&
       target->instance==source.instance && !failed.load(std::memory_order_relaxed)){
        FsrFinalViewport(cmd);
        needsCopiedOutput=true;
        if(recording)recording->copyOutput=true;
        return &targets[2].target;
    }
    return target;
}
extern "C" __declspec(dllexport) Object FsrDrawProperties(Object cmd,Object block) {
    if(!camera || cmd!=command || !recording)return block;
    if(!block && !failed.load(std::memory_order_relaxed)){
        try {
            const auto name=sharedOutput(recording->generation);
            if(auto properties=externalOutput.bind(name,recording->width,recording->height)){
                recording->directOutput=name;
                recording->copyOutput=needsCopiedOutput;
                return properties;
            }
        }catch(const std::exception& error){status("FSR using copied output: %s",error.what());}
    }
    needsCopiedOutput=true;
    recording->copyOutput=true;
    return block;
}
extern "C" __declspec(dllexport) void FsrExecute(Object cmd) {
    // A submit also closes the camera scope when a renderer skips its final blit.
    if(camera && (!cmd || cmd==command))restore();
    static bool retired=false;
    if(!cmd && failed.load(std::memory_order_acquire) && !retired){
        // The last submitted buffer may contain draws using the shared image.
        // Retire it on the render thread after those draws, including fallback.
        retired=true;
        try {
            static const auto issue=icall<void(*)(decltype(&releasePass),int)>("UnityEngine.GL::GLIssuePluginEvent");
            externalOutput.retire();
            issue(releasePass,0);
        }catch(const std::exception& error){status("Could not retire FSR output: %s",error.what());}
    }
}
}
