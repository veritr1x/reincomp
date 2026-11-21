#include "../../host/module_registry.h"
#include "../../runtime/fast_tls.h"
#include "../../host/pthread_host.h"
#include "../../host/signal_host.h"
#include "../../host/game_frame_rate.h"
#include "../../host/jni_host.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using bunker::windows::ModuleRegistry;
using bunker::windows::Pthreads;
using bunker::windows::SignalHost;
#include "webview-jni.h"
namespace {
Pthreads *threads;
ModuleRegistry *modules;
ModuleRegistry *awaitingJni;
bool androidReady=false;
bunker::jni::Host *playerJni=nullptr;
extern "C" void RegisterPlayerJni(bunker::jni::Host *host){playerJni=host;webview_jni::install(*host);}
size_t androidBindings=0;
std::atomic<unsigned> actorConditions{};
std::atomic<ULONGLONG> waitingAtTitle{};
std::atomic<ULONGLONG> actorUpdated{};
std::atomic<ULONGLONG> loadingOut{};
addr_t handle;
void *callerFiber;
void *runtimeFiber;
DWORD owner;
HANDLE ownerHandle;
std::atomic<const char *> ownerCall{"Windows player"};
const bool sampleStoryStacks=std::getenv("REINCOMP_SAMPLE_STORY_STACKS")!=nullptr;
const bool sampleCalls=std::getenv("REINCOMP_NATIVE_CAPTURE") || sampleStoryStacks;
const bool traceCalls=std::getenv("REINCOMP_NATIVE_TRACE_CALLS")!=nullptr;
struct CurrentCall {
    const char *previous=nullptr;
    explicit CurrentCall(const char *name){
        if(sampleCalls && GetCurrentThreadId()==owner){
            previous=ownerCall.load(std::memory_order_relaxed);
            ownerCall.store(name,std::memory_order_relaxed);
        }
    }
    ~CurrentCall(){if(previous)ownerCall.store(previous,std::memory_order_relaxed);}
};
SignalHost *signals;
uint64_t parentToken;
struct ExternalTokenTag; constexpr recomp::ThreadLocalRef<uint64_t,ExternalTokenTag> externalToken{};
struct OwnedTag; constexpr recomp::ThreadLocalRef<bool,OwnedTag> guestOwnedWorker{};
struct CompareStoreTag; constexpr recomp::ThreadLocalRef<bool,CompareStoreTag> compareOriginalStore{};
recomp::Runtime *guestRuntime;
// Restarter.Start awaits ApplicationApi.Run, which ends in a direct jump to
// AdjustUtility.Initialize. This jump retains the restart coroutine's LR.
// Dispatch the desktop implementation before it unloads the transition scene.
extern "C" Memory *__real_sub_836175ec(State &,addr_t,Memory *);
extern "C" Memory *__wrap_sub_836175ec(State &state,addr_t address,Memory *memory) {
    if(state.gpr.x30.qword==0x8279ac04)
        return guestRuntime->Dispatch(state,address,memory);
    return __real_sub_836175ec(state,address,memory);
}
constexpr addr_t pluginMarker=0x7ffcba000000ULL,logMarker=pluginMarker+8,bunkerTapMarker=pluginMarker+16,engineBase=pluginMarker+0x100;
// Real translated leaf addresses used as nested test callbacks in the
// isolated GC test process. Their normal return behavior is retained.
constexpr addr_t rootOuter=0x8254da08,rootInner=0x8254da14;
thread_local bool testingParents=false;
void *pluginCallback,*logCallback;
uint64_t registered;
uint64_t bound;
uint32_t guideTextHandle;
unsigned menuOpenRequests;
static void releaseGuideText(){
    if(guideTextHandle){
        threads->Run(modules->Symbol(handle,"il2cpp_gchandle_free"),{guideTextHandle});
        guideTextHandle=0;
    }
}
using EngineCall=void(*)(State &,void *);
using BindCall=EngineCall(*)(const char *,void *);
BindCall bindCall;
using Stop=void(*)(const char *);
Stop boundary;
constexpr addr_t liveMarkerBase=pluginMarker+0x80000;
constexpr unsigned liveSlots=16;
std::atomic<addr_t> liveTargets[liveSlots*2]{};
static addr_t livenessMarker(addr_t callback,bool objects){
    if(!callback)return 0;
    HMODULE image=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(callback),&image)||
        (image!=GetModuleHandleW(L"UnityPlayer.dll")&&image!=GetModuleHandleW(L"GameAssembly.dll")))
        boundary("unknown native liveness callback");
    auto first=objects?0u:liveSlots;
    for(unsigned i=first;i<first+liveSlots;++i){
        addr_t empty=0;
        if(liveTargets[i].compare_exchange_strong(empty,callback)||empty==callback)
            return liveMarkerBase+8*i;
    }
    boundary("native liveness callback table exhausted");
    return 0;
}
void registerEngine(const char *name,void *fn) {
    const auto marker=engineBase+8*registered++;
    const std::string key=name;
    if(androidReady){
        if(key=="UnityEngine.AndroidJNI::FromFloatArray"){
            // Android's helper allocates through the Android Unity scripting
            // manager. The desktop player owns that manager and the IL2CPP domain.
            guestRuntime->AddImport(marker,[](State &s){
                const auto reference=s.gpr.x0.qword;
                s.gpr.x0.qword=0;
                if(!reference)return;
                State jni{};
                const auto env=playerJni->EnvAddress();
                jni.gpr.x0.qword=env;jni.gpr.x1.qword=reference;
                playerJni->InvokeEnv(171,jni);
                if(!playerJni->PendingException().empty())return;
                const uint32_t length=jni.gpr.x0.dword;
                static const auto type=[]{
                    auto image=threads->Run(modules->Symbol(handle,"il2cpp_get_corlib"),{});
                    return threads->Run(modules->Symbol(handle,"il2cpp_class_from_name"),{image,addr_t("System"),addr_t("Single")});
                }();
                static const auto header=threads->Run(modules->Symbol(handle,"il2cpp_array_object_header_size"),{});
                const auto array=threads->Run(modules->Symbol(handle,"il2cpp_array_new"),{type,length});
                s.gpr.x0.qword=array;
                if(!array || !length)return;
                jni.gpr.x0.qword=env;jni.gpr.x1.qword=reference;jni.gpr.x2.qword=0;
                jni.gpr.x3.qword=length;jni.gpr.x4.qword=array+header;
                playerJni->InvokeEnv(205,jni);
            });
            threads->Run(modules->Symbol(handle,"il2cpp_add_internal_call"),{addr_t(name),marker});
            ++androidBindings;
            return;
        }
        static const std::map<std::string,addr_t> originals={
#include "original-android-jni.inc"
        };
        if(auto it=originals.find(key);it!=originals.end()){
            threads->Run(modules->Symbol(handle,"il2cpp_add_internal_call"),{reinterpret_cast<addr_t>(name),it->second});
            ++androidBindings;
            return;
        }
    }
    auto callable=bindCall(name,fn);
    if(callable)++bound;
    guestRuntime->AddImport(marker,[key,fn,callable](State &s){
        CurrentCall current(key.c_str());
        if(callable){callable(s,fn);return;}
        boundary(key.c_str());
    });
    const auto add=modules->Symbol(handle,"il2cpp_add_internal_call");
    threads->Run(add,{reinterpret_cast<addr_t>(name),marker});
}
extern "C" void __real__ZN6recomp7Runtime9AddImportEySt8functionIFvR5StateEE(recomp::Runtime *,addr_t,std::function<void(State &)>);
extern "C" void __wrap__ZN6recomp7Runtime9AddImportEySt8functionIFvR5StateEE(recomp::Runtime *runtime,addr_t marker,std::function<void(State &)> bridge) {
    if(traceCalls && runtime==guestRuntime&&marker==0x7ff600100018ULL){
        auto actual=std::move(bridge);
        bridge=[actual=std::move(actual)](State &s){
            static std::atomic<unsigned> count{0};
            if(s.gpr.x1.dword==30 && s.gpr.x0.qword==parentToken && GetCurrentThreadId()!=owner && count.fetch_add(1)<8){
                CONTEXT c{};c.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER;
                uint64_t stack[96]{};SIZE_T bytes=0;
                auto suspended=SuspendThread(ownerHandle);
                const char *phase=ownerCall.load();
                if(suspended!=DWORD(-1)){
                    if(GetThreadContext(ownerHandle,&c))
                        ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void *>(c.Rsp),stack,sizeof(stack),&bytes);
                    ResumeThread(ownerHandle);
                }
                auto unity=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"UnityPlayer.dll"));
                std::fprintf(stderr,"Native GC target tid=%lu phase=%s RIP=%llx stack=%zu Unity RVAs",owner,phase,c.Rip,bytes);
                for(size_t i=0;i<bytes/8;++i)if(stack[i]>unity&&stack[i]<unity+0x1450000)std::fprintf(stderr," %llx",stack[i]-unity);
                std::fputc('\n',stderr);std::fflush(stderr);
            }
            actual(s);
        };
    }
    __real__ZN6recomp7Runtime9AddImportEySt8functionIFvR5StateEE(runtime,marker,std::move(bridge));
}
extern "C" int __real__ZN6bunker7windows10SignalHost11BindCurrentEy(SignalHost *,uint64_t);
extern "C" int __wrap__ZN6bunker7windows10SignalHost11BindCurrentEy(SignalHost *instance,uint64_t token) {
    auto result=__real__ZN6bunker7windows10SignalHost11BindCurrentEy(instance,token);
    if(!result && !signals && owner==GetCurrentThreadId()){signals=instance;parentToken=token;}
    return result;
}
}
#include "native-gc.h"
#include "sample-player-main.h"
#include "field-metadata.h"
#include "field-load-queue.h"
#include "story-work.h"
#include "weapon-evolution.h"
#include "liveness-walk.h"
#include "battle-cleanup.h"
#include "bunker-events.h"
#include "unity-object.h"
extern "C" Memory *sub_830de5a0(State &,addr_t,Memory *);
static Memory *scrollContentChanged(State &s,addr_t pc,Memory *memory) {
    // CreateAsync can resume after leaving the Library. A disposed scroll
    // area has no content size to update. The original predicate reads its
    // RectTransform field at +0x28 before comparing with the cached size.
    const auto scroll = s.gpr.x0.qword;
    if (scroll && !nativeUnityObjectAlive(field_metadata::read<void *>(scroll + 0x28))) {
        s.gpr.x0.qword = 0;
        s.gpr.pc.qword = s.gpr.x30.qword;
        return memory;
    }
    return sub_830de5a0(s,pc,memory);
}
extern "C" Memory *sub_84d7fd7c(State &,addr_t,Memory *);
static Memory *summonCameraEulerAngles(State &s,addr_t pc,Memory *memory) {
    // GachaCameraProperty.RotateCamera waits for exact equality after converting
    // a quaternion back to Euler angles. Windows can return the adjacent float.
    constexpr addr_t compareYaw = 0x83703d9c;
    const bool comparing = s.gpr.x30.qword == compareYaw;
    const auto camera = s.gpr.x20.qword;
    memory = sub_84d7fd7c(s,pc,memory);
    if (comparing && camera && s.gpr.pc.qword == compareYaw) {
        const float target = field_metadata::read<float>(camera + 0x64);
        float yaw;
        std::memcpy(&yaw,&s.simd.v[1],sizeof(yaw));
        if (std::fabs(std::remainder(yaw-target,360.f)) <= 0.0001f)
            std::memcpy(&s.simd.v[1],&target,sizeof(target));
    }
    return memory;
}
extern "C" Memory *StartFollowPathRelease(State &,addr_t,Memory *);
extern "C" Memory *UpdateSnapPositionBunker(State &,addr_t,Memory *);
extern "C" void __real__ZN6recomp21PublishGuestOverridesEv();
extern "C" void __wrap__ZN6recomp21PublishGuestOverridesEv(){
    liveness_walk::installTestVisitors();
    battle_cleanup::install();
    recomp::AddGuestDirectEntry(0x84d7fd7c,summonCameraEulerAngles);
    if(std::getenv("REINCOMP_TRACE_GAME_PAGES")){
        struct Entry{addr_t address;const char *name;};
        for(auto e:{Entry{0x82f11bf0,"Title.Notices"},Entry{0x82eb7740,"PreTitleMenu.Notices"},
                    Entry{0x82b92190,"TitleMenu.Notices"},Entry{0x82ed8d08,"DialogHelper.ShowDialogWebView"},
                    Entry{0x82b757b4,"WebViewDialog.Setup"},Entry{0x82b7641c,"WebViewDialog.SetFrame"}})
            recomp::AddGuestProbe(e.address,[e](State &){std::fprintf(stderr,"Game page: %s\n",e.name);std::fflush(stderr);});
    }
    if(std::getenv("REINCOMP_OPEN_SCREEN")){
        // OnChangeView can return normally while the field HUD is fading in.
        // Its synchronous call to OnOpenMomMenu confirms it accepted the tap.
        recomp::AddGuestProbe(0x82cf3e0c,[](State &){++menuOpenRequests;});
        // PlayNavigationCharacterText calls Play on this specific text player.
        // The completion delegate clears the coroutine's "waiting for text" bit.
        recomp::AddGuestProbe(0x8288b9a4,[](State &s){
            if(s.gpr.x30.qword!=0x8305f7cc || GetCurrentThreadId()!=owner)return;
            releaseGuideText();
            guideTextHandle=static_cast<uint32_t>(threads->Run(modules->Symbol(handle,"il2cpp_gchandle_new_weakref"),{s.gpr.x0.qword,0}));
        });
        recomp::AddGuestProbe(0x8305efbc,[](State &){if(GetCurrentThreadId()==owner)releaseGuideText();});
    }
    if(std::getenv("REINCOMP_NATIVE_CHECKPOINT")){
        bunker_events::install();
        recomp::AddGuestDirectEntry(0x830de5a0,scrollContentChanged);
        recomp::AddGuestDirectEntry(0x830df4e8,UpdateSnapPositionBunker);
        weapon_evolution::install();
        if(std::getenv("REINCOMP_PROFILE_STORY_WORK"))
            recomp::AddGuestDirectEntry(0x830cfc3c,[](State &s,addr_t pc,Memory *memory){
                return timeStoryWork(s,pc,memory,StartFollowPathRelease,"FollowPathController.StartFollowPath");
            });
        else recomp::AddGuestDirectEntry(0x830cfc3c,StartFollowPathRelease);
        const auto profile=std::getenv("REINCOMP_BENCH_PROFILE");
        if(!profile || !std::strcmp(profile,"native-checkpoint-field")){
            recomp::AddGuestDirectEntry(0x83da6318,field_load_queue::canExecute);
        }
    }
    if(std::getenv("REINCOMP_TRACE_METHOD_INFLATION"))traceMethodInflation();
    if(std::getenv("REINCOMP_PROFILE_MANAGED") || std::getenv("REINCOMP_PROFILE_STORY_WORK") || sampleStoryStacks)profileStoryWork();
    if(std::getenv("REINCOMP_VERIFY_NATIVE_ROOTS")){
        recomp::AddGuestOverride(rootOuter,[](State &s){
            if(!testingParents){s.gpr.x0.qword=0;return;}
            s.gpr.x19.qword=s.gpr.x0.qword^0x539ac8b178d52f40ULL;
            s.gpr.x0.qword=threads->Run(rootInner,{s.gpr.x1.qword});
            s.gpr.x19.qword=0;
        });
        recomp::AddGuestOverride(rootInner,[](State &s){
            if(!testingParents)return;
            s.gpr.x19.qword=0;
            threads->Run(modules->Symbol(handle,"il2cpp_gchandle_free"),{s.gpr.x0.qword});
            for(unsigned i=0;i<4;++i)threads->Run(modules->Symbol(handle,"il2cpp_gc_collect"),{2});
            s.gpr.x0.qword=1;
        });
    }
    if(std::getenv("REINCOMP_NATIVE_CHECKPOINT")){
        if(auto f=std::fopen("route-trace.log","w"))std::fclose(f);
        if(std::getenv("REINCOMP_TRACE_LOAD_PROGRESS")){
            recomp::AddGuestProbe(0x8306ea38,[previous=-1](State &s) mutable {
                if(GetCurrentThreadId()!=owner)return;
                float fraction;
                std::memcpy(&fraction,&s.simd.v[0],sizeof(fraction));
                if(!(fraction>=0.f && fraction<=1.f))return;
                int percent=static_cast<int>(100*fraction);
                if(percent/5==previous)return;
                previous=percent/5;
                if(auto f=std::fopen("route-trace.log","a")){
                    std::fprintf(f,"%llu Loading.Progress percent=%d\n",GetTickCount64(),percent);
                    std::fclose(f);
                }
            });
        }
        struct Probe {addr_t pc;const char *label;};
        const Probe points[]={{0x82f10440,"Splash.Start"},{0x82f0e818,"Splash.NoteIn"},
            {0x82f0e968,"Splash.NoteOut"},{0x82f0ea10,"Splash.LogoIn"},{0x82f0eab8,"Splash.LogoOut"},
            {0x82f0eb60,"Splash.SoundIn"},{0x82f0ec08,"Splash.SoundOut"},{0x82f0e8c0,"Splash.Wait"},
            {0x82f105f4,"Splash.Animation"},{0x82f106a8,"Splash.Tap"},
            {0x82f102e4,"Title.WaitTap"},{0x82f1038c,"Title.Exit"},{0x8306e628,"Loading.In"},
            {0x8306e6d4,"Loading.Out"},{0x8271e334,"Movie.TryGetTitleId"},{0x8271ea08,"Movie.GetTitlePath"},
            {0x828a96e0,"Movie.TitleFlowMoveNext"},{0x8321dc14,"Movie.PlayTitleFlow"},
            {0x8321cba8,"PreTitle.PlayMovieAsync"},{0x8321ce40,"PreTitle.Play"},{0x8321ce34,"PreTitle.Skip"},
            {0x82f11ca4,"Title.MovieButton"},{0x82f11d58,"Title.Tap"}};
        for(auto p:points)recomp::AddGuestProbe(p.pc,[p,count=0u](State &s) mutable {
            if(p.pc==0x82f102e4)waitingAtTitle=GetTickCount64();
            if(p.pc==0x8306e628){
                loadingOut=0;
                if(GetCurrentThreadId()==owner){
                    field_load_queue::loading=true;
                    field_metadata::begin();
                }
            }
            if(p.pc==0x8306e6d4){
                loadingOut=GetTickCount64();
                if(GetCurrentThreadId()==owner){
                    field_load_queue::loading=false;
                    field_metadata::loading=false;
                }
            }
            if(p.pc==0x82f1038c || p.pc==0x82f11d58)waitingAtTitle=0;
            if(count++>8)return;
            if(auto f=std::fopen("route-trace.log","a")){
                std::fprintf(f,"%llu %s object=%llx",GetTickCount64(),p.label,s.gpr.x0.qword);
                if(p.pc==0x8306e6d4){
                    LARGE_INTEGER now,hz;QueryPerformanceCounter(&now);QueryPerformanceFrequency(&hz);
                    std::fprintf(f," qpcMs=%.4f",1000.*now.QuadPart/hz.QuadPart);
                }
                std::fputc('\n',f);
                std::fclose(f);
            }
        });
        recomp::AddGuestProbe(0x82d62444,[](State &s){
            static const bool traceInput=std::getenv("REINCOMP_TRACE_DESKTOP_INPUT")!=nullptr;
            if(traceInput){
                static auto record=reinterpret_cast<void(*)(void *)>(GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),"TraceInputActor"));
                if(record)record(reinterpret_cast<void *>(s.gpr.x0.qword));
            }
            uint8_t picturebook{};
            recomp::ReadGuestMemory(&picturebook,s.gpr.x0.qword+0x213,1);
            unsigned kind=picturebook?2u:1u;
            auto now=GetTickCount64();
            static ULONGLONG checked=0;
            unsigned previous=actorConditions.load();
            if((previous&3u)!=kind || now-checked>=1000){
                checked=now;
                // The replay requests Best explicitly. In ordinary play the
                // player owns the preset selected in the game's Settings.
                static const bool replay=std::getenv("REINCOMP_ROUTE_CHECKPOINT")!=nullptr;
                if(bunker::windows::frame_rate::ApplyScenePreset(1,replay))kind|=4u;
                actorConditions.store(kind);
                if(previous!=kind){
                    if(auto log=std::fopen("route-trace.log","a")){
                        std::fprintf(log,"%llu Actor.Update %s Highest=%d\n",now,picturebook?"2D":"3D",bool(kind&4));
                        std::fclose(log);
                    }
                }
            }
            actorUpdated.store(now);
        });
    }
    recomp::AddGuestDirectEntry(0x825aad50,native_gc::stop);
    recomp::AddGuestDirectEntry(0x825aae3c,native_gc::start);
    recomp::AddGuestDirectEntry(0x825b25d0,native_gc::scan);
    recomp::AddGuestDirectEntry(0x825b25cc,native_gc::scan);
    recomp::AddGuestDirectEntry(0x825b04a8,native_gc::section);
    __real__ZN6recomp21PublishGuestOverridesEv();
}
extern "C" void __real__ZN6recomp7Runtime9AddTracesEPKNS_5TraceEy(recomp::Runtime *,const recomp::Trace *,size_t);
extern "C" void __wrap__ZN6recomp7Runtime9AddTracesEPKNS_5TraceEy(recomp::Runtime *runtime,const recomp::Trace *traces,size_t count) {
    if(count>10000&&!guestRuntime)guestRuntime=runtime;
    // Unity calls the public liveness APIs through Dispatch. Register those
    // bodies in the trace table, avoiding a per-call override filter hit for
    // unrelated gameplay getters sharing the low bits of either address.
    if(runtime==guestRuntime && count>10000 && !std::getenv("REINCOMP_REFERENCE_LIVENESS")){
        size_t first=0;
        for(size_t i=0;i<count;++i){
            auto walker=liveness_walk::publicWalker(traces[i].address);
            if(!walker)continue;
            __real__ZN6recomp7Runtime9AddTracesEPKNS_5TraceEy(runtime,traces+first,i-first);
            recomp::Trace trace{traces[i].address,walker};
            __real__ZN6recomp7Runtime9AddTracesEPKNS_5TraceEy(runtime,&trace,1);
            ++liveness_walk::registeredWalkers;
            first=i+1;
        }
        traces+=first;count-=first;
    }
    __real__ZN6recomp7Runtime9AddTracesEPKNS_5TraceEy(runtime,traces,count);
}
extern "C" int main(int, char **);
extern "C" void __real__ZN6bunker7windows8Pthreads7InstallEv(Pthreads *);
extern "C" void __wrap__ZN6bunker7windows8Pthreads7InstallEv(Pthreads *instance) {
    threads = instance;
    __real__ZN6bunker7windows8Pthreads7InstallEv(instance);
}
extern "C" addr_t __real__ZN6bunker7windows14ModuleRegistry4OpenEPKci(ModuleRegistry *, const char *, int);
static void enterWindowsPlayer(ModuleRegistry *registry,bool useAndroid){
        modules = registry;
        if(useAndroid){
            // Construct the same owned Unity native string as its original startup,
            // resolve libil2cpp through its Java loader, and populate all 223 API slots.
            alignas(16) uint8_t library[40]{};
            uint32_t tag=0x49;
            std::memcpy(library+32,&tag,sizeof tag);
            auto address=reinterpret_cast<addr_t>(library);
            auto found=threads->Run(0x7041a214,{0x70ed01b3,address});
            if(!found)std::abort();
            auto loaded=threads->Run(0x703b7650,{address});
            threads->Run(0x700e0044,{address});
            if(!loaded)std::abort();
            androidReady=true;
        }
        handle = __real__ZN6bunker7windows14ModuleRegistry4OpenEPKci(registry, "libil2cpp.so", 2);
        if (!handle || !threads || !guestRuntime) std::abort();
        auto adapter=GetModuleHandleW(L"GameAssembly.dll");
        auto engine=LoadLibraryW(L"EngineCalls.dll");
        bindCall=engine?reinterpret_cast<BindCall>(GetProcAddress(engine,"BindEngineCall")):nullptr;
        boundary=reinterpret_cast<Stop>(GetProcAddress(adapter,"UnimplementedCallback"));
        using Enumerate=void(*)(void(*)(const char *,void *));
        auto enumerate=reinterpret_cast<Enumerate>(GetProcAddress(adapter,"VisitRegisteredCalls"));
        if(!boundary||!enumerate||!bindCall)std::abort();
        if(auto tap=reinterpret_cast<void(*)(void *)>(GetProcAddress(adapter,"BunkerHeaderTap")))
            guestRuntime->AddImport(bunkerTapMarker,[tap](State &s){tap(reinterpret_cast<void *>(s.gpr.x0.qword));});
        if(auto tap=reinterpret_cast<void(*)(unsigned,void *)>(GetProcAddress(adapter,"BunkerArenaTap")))
            for(unsigned row=0;row<2;++row)
                guestRuntime->AddImport(bunkerTapMarker+8*(1+row),[tap,row](State &s){tap(row,reinterpret_cast<void *>(s.gpr.x0.qword));});
        guestRuntime->AddImport(logMarker,[](State &s){
            if(!logCallback)std::abort();
            reinterpret_cast<void(*)(const char *)>(logCallback)(reinterpret_cast<const char *>(s.gpr.x0.qword));
        });
        for(unsigned i=0;i<liveSlots*2;++i)guestRuntime->AddImport(liveMarkerBase+8*i,[i](State &s){
            auto fn=liveTargets[i].load();
            if(!fn)boundary("unbound liveness callback");
            if(i<liveSlots)
                reinterpret_cast<void(*)(void **,int,void *)>(fn)(reinterpret_cast<void **>(s.gpr.x0.qword),
                    static_cast<int>(s.gpr.x1.dword),reinterpret_cast<void *>(s.gpr.x2.qword));
            else reinterpret_cast<void(*)()>(fn)();
        });
        enumerate(registerEngine);
        std::fprintf(stderr,"Native Android JNI implementations restored=%zu\n",androidBindings);
        std::fprintf(stderr,"Windows engine published %llu named callbacks to original IL2CPP; %llu scalar signatures bound\n",
            (unsigned long long)registered,(unsigned long long)bound);
        std::fprintf(stderr, "Stock player adapter initialized original IL2CPP module and its constructors\n");
        std::fflush(stderr);
        if(useAndroid && (std::getenv("REINCOMP_NATIVE_CAPTURE") || std::getenv("REINCOMP_SAMPLE_FIELD_CPU"))){
            if(auto sampler=CreateThread(nullptr,0,samplePlayerMain,nullptr,0,nullptr))CloseHandle(sampler);
        }
        if(useAndroid && sampleStoryStacks){
            if(auto sampler=CreateThread(nullptr,0,samplePostedWork,nullptr,0,nullptr))CloseHandle(sampler);
        }
        SwitchToFiber(callerFiber);
        std::abort();
}
extern "C" addr_t __wrap__ZN6bunker7windows14ModuleRegistry4OpenEPKci(ModuleRegistry *registry, const char *path, int flags) {
    if (!modules && !awaitingJni && path && !std::strcmp(path, "libunity.so")) {
        if(std::getenv("REINCOMP_NATIVE_CHECKPOINT"))awaitingJni=registry;
        else enterWindowsPlayer(registry,false);
    }
    return __real__ZN6bunker7windows14ModuleRegistry4OpenEPKci(registry, path, flags);
}
extern "C" uint64_t __real__ZN6bunker7windows8Pthreads3RunEySt16initializer_listIyE(Pthreads *,addr_t,std::initializer_list<addr_t>);
extern "C" uint64_t __wrap__ZN6bunker7windows8Pthreads3RunEySt16initializer_listIyE(Pthreads *instance,addr_t pc,std::initializer_list<addr_t> args){
    auto value=__real__ZN6bunker7windows8Pthreads3RunEySt16initializer_listIyE(instance,pc,args);
    if(awaitingJni && pc==0x70439db0){
        auto registry=awaitingJni;
        awaitingJni=nullptr;
        enterWindowsPlayer(registry,true);
    }
    return value;
}
static void WINAPI loadRuntime(void *) {
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr,executable,32768)) std::abort();
    auto folder=std::filesystem::path(executable).parent_path();
    auto install=folder.parent_path().parent_path()/"emu-client";
    if(auto path=_wgetenv(L"REINCOMP_INSTALL_ROOT"))install=std::filesystem::path(path);
    auto native=(install/"data/native").u8string(), apk=(install/"data/game-sound.apk").u8string();
    auto storage=(folder/"WindowsPlayer_Data").u8string(),angle=(install/"game/angle").u8string();
    const bool checkpoint=std::getenv("REINCOMP_NATIVE_CHECKPOINT")!=nullptr;
    if(checkpoint)storage=(folder.parent_path()/"checkpoint-client/storage").u8string();
    if(auto path=_wgetenv(L"REINCOMP_PLAYER_DATA"))storage=(std::filesystem::path(path)/"storage").u8string();
    auto firebase=(install/"game/FirebaseCppApp-9_6_0.dll").u8string();
    auto grpc=(install/"game/grpc_csharp_ext.x64.dll").u8string();
    auto log=(folder/"runtime.log").u8string();
    if (!std::freopen("NUL","rb",stdin) || !std::freopen("NUL","wb",stdout) ||
        !std::freopen(log.c_str(),"w",stderr)) std::abort();
    std::vector<const char *> args = {"native-runtime", native.c_str(), apk.c_str(),
        storage.c_str(), "--frame-gate", "640", "360", angle.c_str(),
        "--mute-audio", "--collector-delivery", "--first-frame"};
    if(checkpoint){
        const char *fps=std::getenv("REINCOMP_TARGET_FPS");
        args.insert(args.end(),{"--firebase-app-dll",firebase.c_str(),
            "--server-profile","--grpc-native",grpc.c_str(),"--target-fps",fps?fps:"165","--quiet-trace"});
    }
    auto argc=static_cast<int>(args.size());
    args.push_back(nullptr);
    int code = main(argc, const_cast<char **>(args.data()));
    std::fprintf(stderr,"Guest bootstrap returned before native player handoff, code %d\n",code);
    std::fflush(stderr);
    ExitProcess(130);
}
static void releasePlayerThread(){
    native_gc::detach();
    if(!guestOwnedWorker && (threads->ReleaseCurrent() || signals->DropThread(externalToken)))
        boundary("native worker detach failed");
    externalToken=0;
    guestOwnedWorker=false;
}
static void NTAPI onPlayerThreadExit(void *value){
    auto *slot=static_cast<native_gc::Thread *>(value);
    if(!slot || slot->id!=GetCurrentThreadId() || native_gc::current.get()!=slot || !externalToken)return;
    // The Windows player can end a native worker without calling the Android
    // il2cpp_thread_detach export. Its managed foreground thread must leave
    // the VM on this same OS thread, while its pthread keys are still valid.
    {
        native_gc::GuestScope scope(*slot);
        const auto managed=threads->Run(modules->Symbol(handle,"il2cpp_thread_current"));
        if(managed)threads->Run(modules->Symbol(handle,"il2cpp_thread_detach"),{managed});
    }
    releasePlayerThread();
}
static DWORD playerThreadExitSlot(){
    static const auto slot=FlsAlloc(onPlayerThreadExit);
    if(slot==FLS_OUT_OF_INDEXES)boundary("native worker exit registration failed");
    return slot;
}
static native_gc::Thread &enterPlayerCallback(const char *name) {
    // The slot belongs to the attached Windows thread until its explicit
    // detach. Nested callbacks reuse the registration and still publish depth.
    if(auto *attached=native_gc::current.get())return *attached;
    if (!runtimeFiber) {
        owner = GetCurrentThreadId();
        if(!DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&ownerHandle,0,FALSE,DUPLICATE_SAME_ACCESS))std::abort();
        callerFiber = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
        runtimeFiber = CreateFiberEx(0, 8u << 20, FIBER_FLAG_FLOAT_SWITCH, loadRuntime, nullptr);
        if (!callerFiber || !runtimeFiber) std::abort();
        SwitchToFiber(runtimeFiber);
    }
    const bool external=GetCurrentThreadId()!=owner;
    if(external&&!externalToken){
        const auto token=threads->Self();
        if(!signals||!parentToken)boundary("native worker parent signal registration missing");
        guestOwnedWorker=bool(signals->Lookup(token));
        if((!guestOwnedWorker&&signals->RegisterThread(token,parentToken))||signals->BindCurrent(token))
            boundary("native worker signal registration failed");
        externalToken=token;
        std::fprintf(stderr,"%s worker %lu entered guest thread %llx via %s\n",guestOwnedWorker?"Re-entering Android":"Native Windows",GetCurrentThreadId(),(unsigned long long)token,name);
    }
    if(!modules||!handle)std::abort();
    auto &slot=native_gc::attach(external?externalToken.get():parentToken,external&&guestOwnedWorker);
    if(external && !guestOwnedWorker && !FlsSetValue(playerThreadExitSlot(),&slot))boundary("native worker exit handler missing");
    return slot;
}
extern "C" __declspec(dllexport) unsigned MenuOpenRequests(){return menuOpenRequests;}
extern "C" __declspec(dllexport) bool AdvanceMenuGuide(){
    if(!guideTextHandle || GetCurrentThreadId()!=owner)return false;
    native_gc::GuestScope guestScope(enterPlayerCallback("menu guide tap"));
    auto player=threads->Run(modules->Symbol(handle,"il2cpp_gchandle_get_target"),{guideTextHandle});
    if(!player){releaseGuideText();return false;}
    threads->Run(0x8288cb58,{player,0});
    return true;
}
extern "C" __declspec(dllexport) uint64_t InvokeManaged(const uint64_t *arguments) {
    constexpr auto name="il2cpp_runtime_invoke";
    native_gc::GuestScope guestScope(enterPlayerCallback(name));
    static const addr_t entry=modules->Symbol(handle,name);
    if(!entry)boundary(name);
    CurrentCall current(name);
    return threads->Run(entry,arguments,4);
}
extern "C" __declspec(dllexport) uint64_t InvokeOriginal(const char *name, const uint64_t *arguments, size_t count) {
    if(count>8)std::abort();
    auto &thread=enterPlayerCallback(name);
    native_gc::GuestScope guestScope(thread);
    // The pinned Android export is STR X2,[X1]; RET. Keep guest memory
    // reservation invalidation while avoiding a full translated invocation.
    if(!compareOriginalStore && count==3 && !std::strcmp(name,"il2cpp_gc_wbarrier_set_field")){
        recomp::WriteGuestMemory(arguments[1],arguments+2,sizeof(addr_t));
        return 0;
    }
    struct Entry {addr_t address;uint64_t calls;};
    using Entries=std::unordered_map<std::string_view,Entry>;
    struct EntriesTag; static constexpr recomp::ThreadLocalRef<Entries *,EntriesTag> cached{};
    auto *entries=cached.get();
    if(!entries){static thread_local Entries local;entries=&local;cached=entries;}
    auto &record=(*entries)[name];
    if(!record.address)record.address=modules->Symbol(handle,name);
    const auto entry=record.address;
    if (!entry) std::abort();
    const bool report=!std::strcmp(name,"il2cpp_shutdown") ||
                      (traceCalls && (++record.calls<=2 || (record.calls%8192)==0));
    std::array<addr_t,8> args;
    if(count)std::memcpy(args.data(),arguments,count*sizeof(addr_t));
    auto adapt=[&](const char *target,void *&slot,addr_t marker){
        if(std::strcmp(name,target))return;
        if(count!=1)std::abort();
        slot=reinterpret_cast<void *>(args[0]);
        const auto nativeBase=reinterpret_cast<addr_t>(GetModuleHandleW(L"UnityPlayer.dll"));
        std::fprintf(stderr,"Native callback %s RVA %#llx\n",name,(unsigned long long)(slot?args[0]-nativeBase:0));
        args[0]=slot?marker:0;
    };
    adapt("il2cpp_register_log_callback",logCallback,logMarker);
    if(!std::strcmp(name,"il2cpp_unity_liveness_calculation_begin")){
        if(count!=6)boundary("liveness begin argument count");
        args[2]=livenessMarker(args[2],true);
        args[4]=livenessMarker(args[4],false);
        args[5]=livenessMarker(args[5],false);
    }
    if (!std::strcmp(name,"il2cpp_unity_install_unitytls_interface")) {
        if (count!=1 || !args[0]) std::abort();
        auto table=threads->Run(0x707c9334);
        if (table!=0x712d5e08 || std::memcmp(reinterpret_cast<void *>(table),
            reinterpret_cast<void *>(args[0]),16)) std::abort();
        const auto *slots=reinterpret_cast<const addr_t *>(table);
        for (int i=2;i<33;++i) if (slots[i]<0x707c9300 || slots[i]>0x707d0000) std::abort();
        args[0]=table;
    }
    if(report){
        std::fprintf(stderr,"Windows player -> original %s %#llx (%zu arguments), call %llu\n",name,(unsigned long long)entry,count,(unsigned long long)record.calls);
        std::fflush(stderr);
    }
    CurrentCall current(name);
    auto value = threads->Run(entry,args.data(),count);
    if(report){std::fprintf(stderr,"Original returned %s %#llx\n",name,(unsigned long long)value);std::fflush(stderr);}
    if(thread.id!=owner&&!std::strcmp(name,"il2cpp_thread_detach")){
        if(!guestOwnedWorker)FlsSetValue(playerThreadExitSlot(),nullptr);
        releasePlayerThread();
    }
    return value;
}
extern "C" __declspec(dllexport) uint64_t InvokeBound(const char *name,std::atomic<uint64_t> *entry,const uint64_t *args,size_t count){
    if(traceCalls)return InvokeOriginal(name,args,count);
    if(count>8)std::abort();
    native_gc::GuestScope guestScope(enterPlayerCallback(name));
    auto pc=entry->load(std::memory_order_relaxed);
    if(!pc){
        pc=modules->Symbol(handle,name);
        if(!pc)boundary(name);
        entry->store(pc,std::memory_order_relaxed);
    }
    CurrentCall current(name);
    return threads->Run(pc,args,count);
}
extern "C" __declspec(dllexport) void CompareOriginalStore(bool enabled){compareOriginalStore=enabled;}
extern "C" __declspec(dllexport) uintptr_t NativeBunkerTapAddress(){return bunkerTapMarker;}
extern "C" __declspec(dllexport) uintptr_t NativeArenaTapAddress(unsigned row){return row<2?bunkerTapMarker+8*(1+row):0;}
extern "C" __declspec(dllexport) unsigned NativeActorConditions(){
    auto out=loadingOut.load();
    auto now=GetTickCount64();
    auto actor=now-actorUpdated.load()<500?actorConditions.load():0;
    return actor | (out && now-out>=3000 ? 8u : 0u);
}
extern "C" __declspec(dllexport) int NativeRenderInterval(int target){
    namespace rate=bunker::windows::frame_rate;
    field_metadata::prepareNext();
    static const bool bunkerPacing=std::getenv("REINCOMP_BUNKER_IDLE")!=nullptr;
    rate::ApplyData(target,!bunkerPacing);
    int interval=0;
    if(auto statics=rate::Statics(rate::kOnDemandClassSlot))
        recomp::ReadGuestMemory(&interval,statics,sizeof(interval));
    return interval;
}
extern "C" __declspec(dllexport) void NativeSetRenderInterval(int interval){
    namespace rate=bunker::windows::frame_rate;
    if(interval>0)
        if(auto statics=rate::Statics(rate::kOnDemandClassSlot))
            recomp::WriteGuestMemory(statics,&interval,sizeof(interval));
}
extern "C" __declspec(dllexport) bool NativeTitleReady(){
    auto start=waitingAtTitle.load();
    return start && GetTickCount64()-start>=2000;
}
extern "C" __declspec(dllexport) bool CollectWithParentRoot(uint64_t masked,uint32_t strong){
    testingParents=true;
    bool result=false;
    try {result=threads->Run(rootOuter,{masked,strong})==1;}
    catch(const std::exception &e){std::fprintf(stderr,"Parent root test: %s\n",e.what());}
    testingParents=false;
    return result;
}
