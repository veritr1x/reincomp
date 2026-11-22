#include "remill/Arch/AArch64/Runtime/State.h"
#include <windows.h>
#include <xinput.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <sstream>
#include "../../host/windows_shared_preferences.h"
#include <cstdio>
#include <string>
#include <string_view>
#include <shlwapi.h>
#include <tuple>
#include <type_traits>
#include <vector>
#include "desktop-touch.h"
#include "desktop-gamepad.h"
#include "unity-object.h"
#include "../../host/keyboard_touch.h"
#include "../../host/web_controller.h"
namespace {
using P=void *;
using Call=void(*)(State &,void *);
struct Cursor {
    State &s;
    unsigned gen=0,fp=0;
    uintptr_t stack;
    std::array<uint64_t,8> g;
    explicit Cursor(State &state):s(state),stack(state.gpr.sp.qword),g{state.gpr.x0.qword,state.gpr.x1.qword,state.gpr.x2.qword,state.gpr.x3.qword,
        state.gpr.x4.qword,state.gpr.x5.qword,state.gpr.x6.qword,state.gpr.x7.qword}{}
    template<class T> T read(){
        if constexpr(std::is_floating_point_v<T>){
            if(fp<8){T v;std::memcpy(&v,&s.simd.v[fp++],sizeof v);return v;}
        }else if(gen<8){
            if constexpr(std::is_pointer_v<T>)return reinterpret_cast<P>(g[gen++]);
            else return static_cast<T>(g[gen++]);
        }
        // AAPCS64 passes each overflow scalar in its own eight-byte stack slot.
        T value;
        std::memcpy(&value,reinterpret_cast<const void *>(stack),sizeof value);
        stack+=8;
        return value;
    }
    template<class T> void store(T value){
        if constexpr(std::is_floating_point_v<T>){std::memcpy(&s.simd.v[0],&value,sizeof value);}
        else if constexpr(std::is_pointer_v<T>){s.gpr.x0.qword=reinterpret_cast<uint64_t>(value);}
        else if constexpr(sizeof(T)<=4){s.gpr.x0.qword=static_cast<uint32_t>(value);}
        else{s.gpr.x0.qword=static_cast<uint64_t>(value);}
    }
};
template<class R,class... A> void invoke(State &s,void *fn){
    Cursor cursor(s);
    std::tuple<A...> args{cursor.read<A>()...};
    auto call=reinterpret_cast<R(*)(A...)>(fn);
    if constexpr(std::is_void_v<R>)std::apply(call,args);
    else cursor.store(std::apply(call,args));
}
struct Method {const char *name;uint32_t rva;Call call;};
static const Method methods[]={
#include "engine-calls.inc"
};
static void createTexture2D(State &s,void *original){
    constexpr uint32_t rgb8Srgb=3, rgb8Unorm=7;
    // ScreenCaptureBlur.Activate displays a camera readback. The Android
    // material samples those bytes directly. An sRGB desktop texture would
    // decode the captured color a second time before the mask is drawn.
    if(s.gpr.x4.dword==rgb8Srgb && s.gpr.x30.qword==0x84d7cce0){
        // The APK's Texture2D constructors tail-call Internal_Create. Its
        // saved caller identifies GetScreenCapture, whose caller identifies
        // the blur material even when another feature uses the same helper.
        struct Frame {uintptr_t parent,caller;};
        Frame texture{},capture{};
        auto fp=reinterpret_cast<const void *>(s.gpr.x29.qword);
        if(ReadProcessMemory(GetCurrentProcess(),fp,&texture,sizeof(texture),nullptr) &&
           texture.caller==0x83634cb0 &&
           ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void *>(texture.parent),&capture,sizeof(capture),nullptr) &&
           capture.caller==0x832573b8)
            s.gpr.x4.dword=rgb8Unorm;
    }
    invoke<uint8_t,P,int32_t,int32_t,int32_t,int32_t,int32_t,P>(s,original);
}
static P managedPath(const std::wstring &value){
    static auto make=reinterpret_cast<P(*)(const wchar_t *,int)>(GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),"il2cpp_string_new_utf16"));
    return make(value.c_str(),static_cast<int>(value.size()));
}
static std::wstring pathText(P object){
    if(!object)return {};
    static auto dll=GetModuleHandleW(L"GameAssembly.dll");
    static auto length=reinterpret_cast<int(*)(P)>(GetProcAddress(dll,"il2cpp_string_length"));
    static auto chars=reinterpret_cast<const wchar_t *(*)(P)>(GetProcAddress(dll,"il2cpp_string_chars"));
    const int n=length(object);
    return n>0&&n<32768?std::wstring(chars(object),n):std::wstring();
}
static auto privateClient(){
    if(auto path=_wgetenv(L"REINCOMP_PLAYER_DATA"))return std::filesystem::path(path);
    wchar_t exe[32768]{};
    GetModuleFileNameW(nullptr,exe,32768);
    return std::filesystem::path(exe).parent_path().parent_path()/"checkpoint-client";
}
#include "desktop-bundles.h"
#include "checkpoint-prefs.h"
#include "client-days.h"
#include "render-timing.h"
#include "native-pacing.h"
#include "desktop-input.h"
#include "fsr31-engine.h"
static thread_local float flyingMomDelta=-1.f;
static std::filesystem::path storagePath(const std::wstring &name){
    const std::wstring prefix=L"/data/user/0/bunker/";
    if(name.compare(0,prefix.size(),prefix))return {};
    auto tail=name.substr(prefix.size());
    std::filesystem::path part(tail);
    if(part.has_root_path() || tail.find_first_of(L"\\:")!=tail.npos)return {};
    for(const auto &s:part)if(s=="..")return {};
    return privateClient()/"storage"/part;
}
static void persistentPath(State &s,void *){s.gpr.x0.qword=reinterpret_cast<uintptr_t>(managedPath(L"/data/user/0/bunker/files"));}
static void cachePath(State &s,void *){s.gpr.x0.qword=reinterpret_cast<uintptr_t>(managedPath(L"/data/user/0/bunker/cache"));}
static void packagedPath(State &s,void *){s.gpr.x0.qword=reinterpret_cast<uintptr_t>(managedPath(L"/data/app/bunker/base.apk"));}
static void streamingPath(State &s,void *){s.gpr.x0.qword=reinterpret_cast<uintptr_t>(managedPath(L"jar:file:///data/app/bunker/base.apk!/assets"));}
template<bool bundled,bool async> static void traceMenuAsset(State &s,void *fn){
    static const auto active=reinterpret_cast<bool(*)()>(GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),"MenuTransitionActive"));
    const bool tracing=active&&active();
    std::wstring path;
    LARGE_INTEGER start{},end{},hz{};
    if(tracing){
        path=pathText(reinterpret_cast<P>(bundled?s.gpr.x1.qword:s.gpr.x0.qword));
        QueryPerformanceCounter(&start);
    }
    if constexpr(bundled)invoke<P,P,P,P>(s,fn);else invoke<P,P,P>(s,fn);
    if(tracing){
        QueryPerformanceCounter(&end);QueryPerformanceFrequency(&hz);
        if(auto log=std::fopen("menu-assets.log","a")){
            std::fprintf(log,"%.4f %.3f %s %s %ls\n",1000.0*start.QuadPart/hz.QuadPart,
                1000.0*(end.QuadPart-start.QuadPart)/hz.QuadPart,bundled?"bundle":"resource",async?"async":"sync",path.c_str());
            std::fclose(log);
        }
    }
}
static void loadBundle(State &s,void *fn){
    auto path=storagePath(pathText(reinterpret_cast<P>(s.gpr.x0.qword)));
    if(!path.empty()&&!s.gpr.x2.qword&&GetEnvironmentVariableW(L"REINCOMP_DESKTOP_BUNDLES",nullptr,0)){
        auto converted=convertBundleFile(path,s.gpr.x1.dword);
        if(!converted.empty()){path=std::move(converted);s.gpr.x1.qword=0;}
    }
    if(!path.empty())s.gpr.x0.qword=reinterpret_cast<uintptr_t>(managedPath(path.native()));
    invoke<P,P,uint32_t,uint64_t>(s,fn);
}
static void setAssetUrl(State &s,void *fn){
    auto object=reinterpret_cast<P>(s.gpr.x1.qword);
    auto url=pathText(object);
    static const std::wstring normal=L"http://127.0.0.1:38080/";
    if(GetEnvironmentVariableW(L"REINCOMP_ISOLATED_SERVER",nullptr,0) && url.compare(0,normal.size(),normal)==0){
        url.replace(0,normal.size(),L"http://127.0.0.1:39080/");
        s.gpr.x1.qword=reinterpret_cast<uintptr_t>(managedPath(url));
    }
    auto useFile=[&](const std::filesystem::path &path){
        wchar_t converted[32768];DWORD count=32768;
        if(FAILED(UrlCreateFromPathW(path.c_str(),converted,&count,0)))return;
        std::wstring fileUrl(converted,count);
        // Unity 2019's Windows file handler opens %20 as part of the filename.
        // Restore spaces only after converting the native path to a file URL.
        for(auto space=fileUrl.find(L"%20");space!=fileUrl.npos;space=fileUrl.find(L"%20",space+1)){
            fileUrl.replace(space,3,1,L' ');
        }
        s.gpr.x1.qword=reinterpret_cast<uintptr_t>(managedPath(fileUrl));
    };
    auto jar=url.find(L"jar:file:");
    if(jar!=url.npos){
        auto end=url.find_first_of(L"?#",jar);
        auto marker=url.find(L"!/assets/",jar);
        if(marker!=url.npos && (end==url.npos || end>marker+9)){
            auto name=url.substr(marker+9,end==url.npos?url.npos:end-marker-9);
            std::filesystem::path relative(name);
            bool safe=!relative.empty()&&!relative.has_root_path()&&name.find_first_of(L"\\:")==name.npos;
            for(const auto &part:relative)if(part==".."||part==".")safe=false;
            if(safe){
                auto path=privateClient()/"apk-assets"/relative;
                bool found=std::filesystem::is_regular_file(path);
                if(auto log=std::fopen("asset-routing.log","a")){
                    std::fprintf(log,"APK asset %ls found=%d\n",name.c_str(),found);
                    std::fclose(log);
                }
                if(found)useFile(path);
            }
        }else if(auto log=std::fopen("asset-routing.log","a")){
            auto local=url.substr(jar,(std::min)(size_t(200),end==url.npos?url.npos:end-jar));
            std::fprintf(log,"Unmapped Android URL %.200ls\n",local.c_str());std::fclose(log);
        }
    }else if(url.compare(0,7,L"file://")==0){
        auto mapped=storagePath(url.substr(7));
        if(!mapped.empty())useFile(mapped);
    }
    invoke<int32_t,P,P>(s,fn);
}
}
extern "C" __declspec(dllexport) void AfterRenderPipeline(){render_pacing::afterPipeline();}
extern "C" __declspec(dllexport) void AdvanceDesktopInput(){desktop_input::advance();}
extern "C" __declspec(dllexport) bool ConsumeDesktopSkip(){
    const bool pressed=desktop_input::skipRequested;
    desktop_input::skipRequested=false;
    return pressed;
}
extern "C" __declspec(dllexport) void DesktopGamepadsChanged(){desktop_gamepad::devicesChanged.store(true);}
extern "C" __declspec(dllexport) void ShutdownDesktopGamepads(){desktop_gamepad::shutdown();}
extern "C" __declspec(dllexport) bool DesktopTouchActive(){
    using namespace desktop_input;
    return realCount || pointer.visible || movement.visible || padPointer.visible;
}
extern "C" __declspec(dllexport) bool FlyingMomTapBegan(){
    using namespace desktop_input;
    advance();
    for(const auto *contact:{&pointer,&padPointer})
        if(contact->visible && contact->touch.phase==bunker::desktop_touch::Began)return true;
    for(const auto &finger:fingers){
        if(finger.phase==bunker::desktop_touch::Began)return true;
    }
    return false;
}
extern "C" __declspec(dllexport) void FlyingMomSetDelta(float seconds){flyingMomDelta=seconds;}
extern "C" __declspec(dllexport) void DesktopMouseEdge(bool down,float x,float y){desktop_input::mouseEdge(down,x,y);}
extern "C" __declspec(dllexport) void DesktopMouseDrag(float x,float y){desktop_input::mouseDrag(x,y);}
extern "C" __declspec(dllexport) void CompareMenuPointer(bool sparse,uint64_t *counts){
    counts[0]=desktop_input::mouseChecks;counts[1]=desktop_input::idleChecks;
    desktop_input::mouseChecks=desktop_input::idleChecks=0;
    desktop_input::sparseMouse=sparse;
}
extern "C" __declspec(dllexport) Call BindEngineCall(const char *key,void *fn) {
    auto it=std::lower_bound(std::begin(methods),std::end(methods),key,[](const Method &m,const char *name){return std::strcmp(m.name,name)<0;});
    auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"UnityPlayer.dll"));
    if(it==std::end(methods)||std::strcmp(key,it->name)||it->rva!=reinterpret_cast<uintptr_t>(fn)-base)return nullptr;
    if(GetEnvironmentVariableW(L"REINCOMP_NATIVE_CHECKPOINT",nullptr,0)){
        if(auto callback=fsr_engine::bind(key))return callback;
        if(!std::strcmp(key,"UnityEngine.Texture2D::Internal_CreateImpl"))return createTexture2D;
        // The game's two RenderManager.SetResolution calls always request
        // mobile fullscreen. Windows owns the surface during interactive play.
        if(!std::strcmp(key,"UnityEngine.Screen::SetResolution") &&
           GetEnvironmentVariableW(L"REINCOMP_INTERACTIVE_WINDOW",nullptr,0))return [](State &,void *){};
        if(auto callback=desktop_input::bind(key,fn))return callback;
        if(!std::strcmp(key,"UnityEngine.Behaviour::get_isActiveAndEnabled"))return [](State &s,void *original){
            // The Exchange shop can finish loading a row icon after Unity has
            // destroyed its Graphic. SetVerticesDirty has nothing to schedule
            // in that case.
            if(s.gpr.x30.qword==0x83d31e94 && s.gpr.x0.qword){
                if(!nativeUnityObjectAlive(reinterpret_cast<P>(s.gpr.x0.qword))){s.gpr.x0.qword=0;return;}
            }
            invoke<uint8_t,P>(s,original);
        };
        if(!std::strcmp(key,"UnityEngine.Time::get_deltaTime"))return [](State &s,void *original){
            if(flyingMomDelta>=0.f)std::memcpy(&s.simd.v[0],&flyingMomDelta,sizeof(float));
            else invoke<float>(s,original);
        };
        if(std::getenv("REINCOMP_TRACE_MENU_ASSETS")){
            if(!std::strcmp(key,"UnityEngine.Resources::Load"))return traceMenuAsset<false,false>;
            if(!std::strcmp(key,"UnityEngine.Resources::LoadAsyncInternal"))return traceMenuAsset<false,true>;
            if(!std::strcmp(key,"UnityEngine.AssetBundle::LoadAsset_Internal"))return traceMenuAsset<true,false>;
            if(!std::strcmp(key,"UnityEngine.AssetBundle::LoadAssetAsync_Internal"))return traceMenuAsset<true,true>;
        }
        if(render_pacing::rate || GetEnvironmentVariableW(L"REINCOMP_NATIVE_RENDER_TIMING",nullptr,0))
            if(auto callback=render_timing::bind(key,fn))return callback;
        static const bool resetClientDays = std::filesystem::is_regular_file(client_days::marker());
        if(resetClientDays && std::string_view(key).find("UnityEngine.PlayerPrefs::")==0)
            return client_days::beforePreference;
        if(GetEnvironmentVariableW(L"REINCOMP_CHECKPOINT_PREFS",nullptr,0))
            if(auto callback=checkpoint_prefs::bind(key))return callback;
        if(GetEnvironmentVariableW(L"REINCOMP_DESKTOP_BUNDLES",nullptr,0)){
            if(!std::strcmp(key,"UnityEngine.AssetBundle::LoadFromStreamAsyncInternal"))return loadDesktopStream<true>;
            if(!std::strcmp(key,"UnityEngine.AssetBundle::LoadFromStreamInternal"))return loadDesktopStream<false>;
        }
        if(!std::strcmp(key,"UnityEngine.Networking.UnityWebRequest::SetUrl"))return setAssetUrl;
        if(!std::strcmp(key,"UnityEngine.Application::get_platform"))return [](State &s,void *original){
            // The APK's UniWebViewInterface constructor selects its Java bridge
            // here. That bridge is provided by webview-jni.h on Windows.
            if(s.gpr.x30.qword==0x8371a1ec){s.gpr.x0.qword=11;return;}
            invoke<int32_t>(s,original);
        };
        if(!std::strcmp(key,"UnityEngine.Application::get_persistentDataPath"))return persistentPath;
        if(!std::strcmp(key,"UnityEngine.Application::get_temporaryCachePath"))return cachePath;
        if(!std::strcmp(key,"UnityEngine.Application::get_dataPath"))return packagedPath;
        if(!std::strcmp(key,"UnityEngine.Application::get_streamingAssetsPath"))return streamingPath;
        if(!std::strcmp(key,"UnityEngine.AssetBundle::LoadFromFile_Internal")||
           !std::strcmp(key,"UnityEngine.AssetBundle::LoadFromFileAsync_Internal"))return loadBundle;
    }
    return it->call;
}
