#include <algorithm>
#include <chrono>
#include <vector>
#include "inspect-player-scene.h"
#include "player-window.h"
#include "screen-route.h"
#include "desktop-skip.h"
// Picturebook input snaps a reversal to RunPanLevel regardless of drag length.
// Use the game's walk level for that turn while a desktop walk gesture is held.
extern "C" __declspec(dllexport) void SetDesktopWalkTurn(bool walking){
    static P level=[]{
        auto cls=desktop_skip::type("Dark.Animation","ActorControllerWithGesturePan");
        if(!cls)return P{};
        il2cpp_runtime_class_init(cls);
        return il2cpp_class_get_field_from_name(cls,"RunPanLevel");
    }();
    if(!level)return;
    static int original;
    if(walking)invoke("il2cpp_field_static_get_value",level,&original);
    int desired=walking?1:original;
    invoke("il2cpp_field_static_set_value",level,&desired);
}
static std::chrono::steady_clock::time_point titleStarted{};
static uint32_t routeTitleHandle=0;
static uint32_t routeAutoHandle=0;
static bool autoDelivered=false;
// Read the actor itself during input tests. Some story cameras stay fixed
// while the character can move within the shot.
extern "C" __declspec(dllexport) void TraceInputActor(P object){
    static auto frame=reinterpret_cast<int(*)()>(registry.at("UnityEngine.Time::get_frameCount"));
    const int id=frame();
    if(id%30 || !object)return;
    static P position=nullptr;
    if(!position){
        auto type=reinterpret_cast<P>(invoke("il2cpp_object_get_class",object));
        position=il2cpp_class_get_method_from_name(type,"get_Position",0);
    }
    P error=nullptr;
    auto box=position?reinterpret_cast<P>(invoke("il2cpp_runtime_invoke",position,object,nullptr,&error)):nullptr;
    if(!box || error)return;
    auto values=reinterpret_cast<const float *>(invoke("il2cpp_object_unbox",box));
    if(auto log=std::fopen("desktop-input.log","a")){
        std::fprintf(log,"frame=%d actor=%p position=%.5f,%.5f,%.5f\n",id,object,values[0],values[1],values[2]);
        std::fclose(log);
    }
}
#ifdef REINCOMP_BUNKER_MODULES
extern "C" {
#include "title_now.h"
#include "badge_pace.h"
#include "render_effects.h"
#include "render_pace.h"
#include "flying_mom.h"
#include "roulette_ads.h"
#include "bunker_button.h"
#include "battle_mods.h"
void desktop_battle_update(void *);
void desktop_arena_tap(unsigned,void *);
void desktop_header_start(void *);
void desktop_header_update(void *);
void desktop_header_tap(void *);
#include "arena_season.h"
#include "big_hunt_season.h"
}
static void *bunkerApi(const char *name){
    return reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),name));
}
static void bunkerLog(const char *message){std::fprintf(stderr,"%s\n",message);}
extern "C" __declspec(dllexport) bool BunkerShowRoulette(P object,P method){
    static const bool ready=roulette_ads_init(bunkerApi,bunkerLog,nullptr)==0;
    if(ready)roulette_ads_show(object,method);
    return ready;
}
extern "C" void *desktop_bunker_tap_address(){
    static auto address=reinterpret_cast<uintptr_t(*)()>(GetProcAddress(GetModuleHandleW(L"GuestRuntime.dll"),"NativeBunkerTapAddress"));
    return address?reinterpret_cast<void *>(address()):nullptr;
}
extern "C" __declspec(dllexport) void BunkerHeaderTap(P self){desktop_header_tap(self);}
extern "C" void *desktop_arena_tap_address(unsigned row){
    static auto address=reinterpret_cast<uintptr_t(*)(unsigned)>(GetProcAddress(GetModuleHandleW(L"GuestRuntime.dll"),"NativeArenaTapAddress"));
    return address?reinterpret_cast<void *>(address(row)):nullptr;
}
extern "C" __declspec(dllexport) void BunkerArenaTap(unsigned row,P target){desktop_arena_tap(row,target);}
static void showBunkerFromHeader(){
    if(auto window=player_window::attached.load())PostMessageW(window,bunker::web_controller::ToggleLauncher,0,0);
}
static bool desktopHeaderReady=false;
static std::atomic<P> campaignBadgeUpdate{},headerStart{},headerUpdate{},flyingMomUpdate{},battleHudUpdate{};
static size_t campaignBadgeLabelOffset;
static void prepareBunkerMenuCallbacks(){
    if(campaignBadgeUpdate.load(std::memory_order_relaxed) && headerStart.load(std::memory_order_relaxed))return;
    size_t count=0;
    auto assemblies=static_cast<P *>(il2cpp_domain_get_assemblies(il2cpp_domain_get(),&count));
    for(size_t i=0;i<count;++i){
        P image=il2cpp_assembly_get_image(assemblies[i]);
        P type=il2cpp_class_from_name(image,"Dark.View.UserInterface.Outgame","CampaignSingleBadge");
        if(!type)continue;
        P battle=il2cpp_class_from_name(image,"Dark.View.HeadUpDisplay","BattleSystemMenuPresenter");
        const bool arenaReady=battle && desktop_arena_tap_address(0) &&
            battle_mods_init(bunkerApi,bunkerLog,"../data/server/battle_mods.conf")==0 && battle_mods_ui_init(bunkerApi,bunkerLog)==0;
        if(arenaReady)battleHudUpdate.store(il2cpp_class_get_method_from_name(battle,"Update",0),std::memory_order_release);
        bunkerLog(arenaReady?"Bunker Arena mods: ready":"Bunker Arena mods: could not resolve the original battle HUD");
        P mama=il2cpp_class_from_name(image,"Subsystem.MiniGame.FlyingMom","FlyingMomPresenter");
        if(mama)flyingMomUpdate.store(il2cpp_class_get_method_from_name(mama,"Update",0),std::memory_order_release);
        P header=il2cpp_class_from_name(image,"Dark.View.HeadUpDisplay.Header","HeaderButtons");
        if(header){
            desktopHeaderReady=desktop_bunker_tap_address() && bunker_button_init(bunkerApi,bunkerLog,showBunkerFromHeader)==0;
            bunkerLog(desktopHeaderReady?"Bunker header button: ready":"Bunker header button: could not resolve the original header");
            headerStart.store(il2cpp_class_get_method_from_name(header,"Start",0),std::memory_order_release);
            if(desktopHeaderReady)headerUpdate.store(il2cpp_class_get_method_from_name(header,"Update",0),std::memory_order_release);
        }
        P field=il2cpp_class_get_field_from_name(type,"_remainingTimeLabel");
        P method=il2cpp_class_get_method_from_name(type,"Update",0);
        if(field && method){
            campaignBadgeLabelOffset=il2cpp_field_get_offset(field);
            campaignBadgeUpdate.store(method,std::memory_order_release);
        }
        return;
    }
}
template<class Call> static P updateFlyingMom(P presenter,Call &call,P *exception){
    static auto rate=reinterpret_cast<int(*)()>(registry.at("UnityEngine.Application::get_targetFrameRate"));
    static auto delta=reinterpret_cast<float(*)()>(registry.at("UnityEngine.Time::get_deltaTime"));
    static auto unscaled=reinterpret_cast<float(*)()>(registry.at("UnityEngine.Time::get_unscaledDeltaTime"));
    static auto began=reinterpret_cast<bool(*)()>(GetProcAddress(GetModuleHandleW(L"EngineCalls.dll"),"FlyingMomTapBegan"));
    static auto setDelta=reinterpret_cast<void(*)(float)>(GetProcAddress(GetModuleHandleW(L"EngineCalls.dll"),"FlyingMomSetDelta"));
    if(rate()<=30 || !began || !setDelta)return call();
    static FlyingMomClock clock{};
    static P last=nullptr;
    if(last!=presenter){last=presenter;clock={};}
    if(!flying_mom_clock_tick(&clock,unscaled(),delta(),began())){
        if(exception)*exception=nullptr;
        return nullptr;
    }
    setDelta(clock.step_time);
    struct Restore {void (*set)(float);~Restore(){set(-1.f);}} restore{setDelta};
    return call();
}
static void applyBunkerSeasons(){
    static const bool hunt=big_hunt_season_init(bunkerApi,bunkerLog)==0;
    static const bool arena=arena_season_init(bunkerApi,bunkerLog)==0;
    if(hunt)big_hunt_season_name();
    if(arena)arena_season_dates();
}
static bool skipUnchangedCampaignBadge(P method,P object){
    if(!object || method!=campaignBadgeUpdate.load(std::memory_order_acquire) || !method)return false;
    // Unity invokes this MonoBehaviour's Update on the main thread. Use
    // Bunker's badge and label identity check before formatting its timer.
    static BadgePace pace{};
    P label=nullptr;
    std::memcpy(&label,static_cast<const char *>(object)+campaignBadgeLabelOffset,sizeof(label));
    return !badge_pace_due(&pace,object,label,GetTickCount64()/1000);
}
static void pollBunkerTitle(){
    // The launcher runs the player in game/. A relative path also works under
    // a Unicode installation name with the mobile module's narrow file API.
    static const bool ready=std::getenv("REINCOMP_INSTALL_ROOT") && title_now_init(bunkerApi,bunkerLog,"../data/server/title.now")==0;
    static ULONGLONG next=0;
    const auto now=GetTickCount64();
    if(ready && now>=next){next=now+1000;title_now_tick();}
}
static void applyBunkerEffects(){
    static int result=0;
    static const bool ready=[]{
        const char *value=std::getenv("REINCOMP_BUNKER_EFFECTS_OFF");
        const int mask=value?std::atoi(value):0;
        return mask>0 && mask<8 && render_effects_init(bunkerApi,bunkerLog,mask)==0;
    }();
    if(ready && !result && titleStarted.time_since_epoch().count())
        result=render_effects_apply();
}
static int applyBunkerIdle(int fps,int interval){
    static const bool enabled=std::getenv("REINCOMP_BUNKER_IDLE")!=nullptr;
    if(!enabled || fps<=30 || !interval || !titleStarted.time_since_epoch().count())return interval;
    // Bunker keeps battles at full speed. Resolve the same live battle fields
    // as its mobile render_pace module, against the original class metadata.
    struct Battle {P instance;size_t context,game;};
    static const auto battle=[]()->Battle{
        auto assembly=il2cpp_domain_assembly_open(il2cpp_domain_get(),"Assembly-CSharp");
        if(!assembly)return {};
        P image=il2cpp_assembly_get_image(assembly);
        auto find=[&](const char *ns,const char *type,const char *name){
            P klass=il2cpp_class_from_name(image,ns,type);
            return klass?il2cpp_class_get_field_from_name(klass,name):nullptr;
        };
        P counter=il2cpp_class_from_name(image,"Dark.Game.TurnBattle","TurnbaseFrameCounterController");
        if(!counter)return {};
        il2cpp_runtime_class_init(counter);
        P field=il2cpp_class_get_field_from_name(counter,"_instance");
        P context=find("Adam.Framework.GameKit.TurnBattle","BattleComponent","_battleContext");
        P game=find("Adam.Framework.GameKit.TurnBattle","BattleContext","_game");
        return field && context && game?Battle{field,il2cpp_field_get_offset(context),il2cpp_field_get_offset(game)}:Battle{};
    }();
    if(!battle.instance)return interval;
    static auto touch=reinterpret_cast<bool(*)()>(GetProcAddress(GetModuleHandleW(L"EngineCalls.dll"),"DesktopTouchActive"));
    static auto apply=reinterpret_cast<void(*)(int)>(GetProcAddress(GetModuleHandleW(L"GuestRuntime.dll"),"NativeSetRenderInterval"));
    static auto delta=reinterpret_cast<float(*)()>(registry.at("UnityEngine.Time::get_unscaledDeltaTime"));
    if(!touch || !apply)return interval;
    P counter=nullptr,context=nullptr,game=nullptr;
    invoke("il2cpp_field_static_get_value",battle.instance,&counter);
    if(counter)std::memcpy(&context,static_cast<char *>(counter)+battle.context,sizeof(context));
    if(context)std::memcpy(&game,static_cast<char *>(context)+battle.game,sizeof(game));
    static RenderPace pace{};
    const int next=render_pace_decide(&pace,fps,interval,delta(),touch(),game!=nullptr,0,1,0);
    if(next!=interval)apply(next);
    return next;
}
#endif
static double mainThreadMilliseconds(){
    FILETIME created{},ended{},kernel{},user{};
    if(!GetThreadTimes(GetCurrentThread(),&created,&ended,&kernel,&user))return 0;
    ULARGE_INTEGER k{},u{};
    k.LowPart=kernel.dwLowDateTime;k.HighPart=kernel.dwHighDateTime;
    u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;
    return (k.QuadPart+u.QuadPart)/10000.0;
}
// Apply the prepared installation's measured preference only in the loaded
// 3D field. Restore the calling thread's prior mask when it leaves the field.
static void applyFieldAffinity(unsigned state,FILE *log){
    static const int cpu=[]{
        const char *value=std::getenv("REINCOMP_NATIVE_MAIN_CPU");
        if(!value || !*value)return -1;
        char *end=nullptr;long parsed=std::strtol(value,&end,10);
        return end && !*end && parsed>=0 && parsed<64?int(parsed):-1;
    }();
    static DWORD_PTR previous=0;
    static bool unavailable=false;
    if(cpu<0 || unavailable)return;
    const bool field=(state&13)==13;
    if(field && !previous){
        GROUP_AFFINITY allowed{};
        const DWORD_PTR desired=DWORD_PTR(1)<<cpu;
        if(GetThreadGroupAffinity(GetCurrentThread(),&allowed) && allowed.Group==0 && (allowed.Mask&desired))
            previous=SetThreadAffinityMask(GetCurrentThread(),desired);
        if(!previous)unavailable=true;
        if(log)std::fprintf(log,"3D main thread logical CPU %d applied=%d\n",cpu,bool(previous));
    }else if(!field && previous){
        if(SetThreadAffinityMask(GetCurrentThread(),previous))previous=0;
    }
}
static void observePlayerFrame(){
    using Clock=std::chrono::steady_clock;
    static const auto frame=reinterpret_cast<int(*)()>(registry.at("UnityEngine.Time::get_frameCount"));
    static const auto width=reinterpret_cast<int(*)()>(registry.at("UnityEngine.Screen::get_width"));
    static const auto height=reinterpret_cast<int(*)()>(registry.at("UnityEngine.Screen::get_height"));
    static const auto level=reinterpret_cast<int(*)()>(registry.at("UnityEngine.QualitySettings::GetQualityLevel"));
    static const auto target=reinterpret_cast<int(*)()>(registry.at("UnityEngine.Application::get_targetFrameRate"));
    static const auto setTarget=reinterpret_cast<void(*)(int)>(registry.at("UnityEngine.Application::set_targetFrameRate"));
    static const auto setSize=reinterpret_cast<void(*)(int,int,int,int)>(registry.at("UnityEngine.Screen::SetResolution"));
    static const auto setVsync=reinterpret_cast<void(*)(int)>(registry.at("UnityEngine.QualitySettings::set_vSyncCount"));
    static const auto vsync=reinterpret_cast<int(*)()>(registry.at("UnityEngine.QualitySettings::get_vSyncCount"));
    static const auto fullScreenMode=reinterpret_cast<int(*)()>(registry.at("UnityEngine.Screen::get_fullScreenMode"));
    static const bool borderless=GetEnvironmentVariableW(L"REINCOMP_NATIVE_FULLSCREEN",nullptr,0)!=0;
    static const bool interactive=GetEnvironmentVariableW(L"REINCOMP_INTERACTIVE_WINDOW",nullptr,0)!=0;
    static auto actor=reinterpret_cast<unsigned(*)()>(GetProcAddress(GetModuleHandleW(L"GuestRuntime.dll"),"NativeActorConditions"));
    static auto applyInterval=reinterpret_cast<int(*)(int)>(GetProcAddress(GetModuleHandleW(L"GuestRuntime.dll"),"NativeRenderInterval"));
    static const auto scene=reinterpret_cast<void(*)(int *)>(registry.at("UnityEngine.SceneManagement.SceneManager::GetActiveScene_Injected"));
    static const auto sceneName=reinterpret_cast<P(*)(int)>(registry.at("UnityEngine.SceneManagement.Scene::GetNameInternal"));
    static const auto screenshot=reinterpret_cast<void(*)(P,int,int)>(registry.at("UnityEngine.ScreenCapture::CaptureScreenshot"));
    static FILE *log=std::fopen("frame-state.log","w");
    static const bool capture=GetEnvironmentVariableW(L"REINCOMP_NATIVE_CAPTURE",nullptr,0)!=0;
    static const bool inspect=GetEnvironmentVariableW(L"REINCOMP_TEST_TITLE_MOVIE",nullptr,0)!=0;
    static const bool titleDetails=GetEnvironmentVariableW(L"REINCOMP_NATIVE_INSPECT_TITLE",nullptr,0)!=0;
    static const bool checkpoint=GetEnvironmentVariableW(L"REINCOMP_NATIVE_CHECKPOINT",nullptr,0)!=0;
    static const int requestedRate=[]{
        const char *value=std::getenv("REINCOMP_TARGET_FPS");
        if(!value)return 165;
        return !std::strcmp(value,"game")?0:std::clamp(std::atoi(value),1,1000);
    }();
    static auto start=Clock::now(),previous=start,window=start;
    static int lastFrame=-1,lastScene=-1,captures=0;
    static unsigned inspections=0;
    static bool titleInspected=false;
    static bool detailWritten=false;
    static std::vector<double> intervals;
    static double windowCpu=mainThreadMilliseconds();
    static unsigned foreground=0,focusSamples=0;
    auto id=frame();
    if(lastFrame==id)return;
#ifdef REINCOMP_BUNKER_MODULES
    pollBunkerTitle();
    applyBunkerEffects();
#endif
    player_window::install();
    if(const auto size=player_window::pendingResolution.exchange(0,std::memory_order_acq_rel)){
        if(width()!=int(size>>16) || height()!=int(size&0xffff))
            setSize(int(size>>16),int(size&0xffff),fullScreenMode(),0);
    }
    game_pages::frame(width(),height());
    static auto desktopInput=reinterpret_cast<void(*)()>(GetProcAddress(GetModuleHandleW(L"EngineCalls.dll"),"AdvanceDesktopInput"));
    if(desktopInput)desktopInput();
    static auto skipPressed=reinterpret_cast<bool(*)()>(GetProcAddress(GetModuleHandleW(L"EngineCalls.dll"),"ConsumeDesktopSkip"));
    if(skipPressed && skipPressed())desktop_skip::press(desktop_skip::Action::Skip,width(),height());
    static auto autoRunPressed=reinterpret_cast<bool(*)()>(GetProcAddress(GetModuleHandleW(L"EngineCalls.dll"),"ConsumeDesktopAutoRun"));
    if(autoRunPressed && autoRunPressed())desktop_skip::press(desktop_skip::Action::AutoRun,width(),height());
    if(frame_cpu::enabled)frame_cpu::begin(id);
    int renderInterval=checkpoint && requestedRate && applyInterval ? applyInterval(requestedRate) : 0;
#ifdef REINCOMP_BUNKER_MODULES
    renderInterval=applyBunkerIdle(requestedRate,renderInterval);
#endif
    if(routeTitleHandle){
        static auto ready=reinterpret_cast<bool(*)()>(GetProcAddress(GetModuleHandleW(L"GuestRuntime.dll"),"NativeTitleReady"));
        if(ready&&ready()){
            auto title=reinterpret_cast<P>(invoke("il2cpp_gchandle_get_target",routeTitleHandle));
            if(title){
                auto type=reinterpret_cast<P>(invoke("il2cpp_object_get_class",title));
                auto tap=il2cpp_class_get_method_from_name(type,"OnTapTitle",0);
                P error=nullptr;
                if(tap)invoke("il2cpp_runtime_invoke",tap,title,nullptr,&error);
                if(log){std::fprintf(log,"Checkpoint title tap delivered=%d exception=%d\n",!!tap,!!error);std::fflush(log);}
            }
            invoke("il2cpp_gchandle_free",routeTitleHandle);
            routeTitleHandle=0;
        }
    }
    if(checkpoint && (lastFrame<0 || id%120==0)){
        const int desiredMode=(!interactive && borderless)?1:3;
        if(!interactive || (lastFrame<0 && player_window::launcherWindow())){
            const char *w=std::getenv("REINCOMP_WINDOW_WIDTH"),*h=std::getenv("REINCOMP_WINDOW_HEIGHT");
            RECT client{};
            const auto parent=player_window::launcherWindow();
            const bool embedded=parent && GetClientRect(parent,&client) && client.right>0 && client.bottom>0;
            const int desiredW=embedded?client.right:(interactive && w?std::atoi(w):3840);
            const int desiredH=embedded?client.bottom:(interactive && h?std::atoi(h):2160);
            if(width()!=desiredW || height()!=desiredH || (lastFrame<0 && fullScreenMode()!=desiredMode))
                setSize(desiredW,desiredH,desiredMode,0);
        }
        int desired=requestedRate;
        if(renderInterval==1 && desired>120 && desired<165 && actor && (actor()&13)==13 &&
           std::getenv("REINCOMP_NATIVE_PRESENT_RATE")){
            // The render-thread pacer provides the requested rate only in
            // foreground play. Keep Unity's own cap when that pacer is idle.
            DWORD pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&pid);
            if(pid==GetCurrentProcessId())desired=165;
        }
        if(desired && target()!=desired)setTarget(desired);
        if(desired && vsync())setVsync(0);
    }
    auto now=Clock::now();
    int handle=-1;
    scene(&handle);
    if(lastScene!=handle){
        auto name=sceneName(handle);
        auto size=static_cast<int>(name?invoke("il2cpp_string_length",name):0);
        auto chars=name?reinterpret_cast<const wchar_t *>(invoke("il2cpp_string_chars",name)):nullptr;
        if(log){std::fprintf(log,"Frame %d active scene %d %.*ls\n",id,handle,size,chars?chars:L"");std::fflush(log);}
        intervals.clear();
        window=now;
        windowCpu=mainThreadMilliseconds();
        foreground=focusSamples=0;
    }else if(lastFrame>=0){
        intervals.push_back(std::chrono::duration<double,std::milli>(now-previous).count());
    }
    lastFrame=id;lastScene=handle;previous=now;
    if(id%32==0){
        DWORD front=0;
        GetWindowThreadProcessId(GetForegroundWindow(),&front);
        ++focusSamples;
        if(front==GetCurrentProcessId())++foreground;
    }
    if(titleDetails&&!detailWritten&&titleStarted!=Clock::time_point{}&&now-titleStarted>=std::chrono::milliseconds(900)){
        detailWritten=true;
        inspectPlayerScene();
    }
    if(capture&&!titleInspected&&titleStarted!=Clock::time_point{}&&now-titleStarted>=std::chrono::seconds(8)){
        titleInspected=true;
        wchar_t exe[32768]{};
        GetModuleFileNameW(nullptr,exe,32768);
        std::wstring path=exe;
        path=path.substr(0,path.find_last_of(L"\\/")+1)+L"title-visible.png";
        screenshot(reinterpret_cast<P>(invoke("il2cpp_string_new_utf16",path.c_str(),path.size())),1,1);
        if(inspect)inspectPlayerScene();
    }
    if(inspect&&inspections<4&&now-start>=std::chrono::seconds(8+12*inspections)){
        ++inspections;
        inspectPlayerScene();
    }
    if(capture&&captures<4&&now-start>=std::chrono::seconds(5+12*captures)){
        wchar_t exe[32768]{};
        GetModuleFileNameW(nullptr,exe,32768);
        std::wstring path=exe;
        path=path.substr(0,path.find_last_of(L"\\/")+1)+L"player-frame-"+std::to_wstring(++captures)+L".png";
        auto managed=reinterpret_cast<P>(invoke("il2cpp_string_new_utf16",path.c_str(),path.size()));
        screenshot(managed,1,1);
    }
    static const bool navigate=std::getenv("REINCOMP_OPEN_SCREEN")!=nullptr;
    if(navigate)openRequestedScreen(actor?actor():0,id,log);
    if(now-window<std::chrono::seconds(5) || intervals.empty())return;
    static auto serviceBattleCleanup=reinterpret_cast<void(*)()>(
        GetProcAddress(GetModuleHandleW(L"GuestRuntime.dll"),"ServiceBattleCleanup"));
    if(serviceBattleCleanup)serviceBattleCleanup();
    const auto state=actor?actor():0;
    applyFieldAffinity(state,log);
    static const bool pointerTrial=std::getenv("REINCOMP_COMPARE_MENU_POINTER")!=nullptr;
    if(pointerTrial && requestedScreenVisible){
        static unsigned windowIndex=0;
        static bool sparse=false;
        static auto compare=reinterpret_cast<void(*)(bool,uint64_t *)>(GetProcAddress(GetModuleHandleW(L"EngineCalls.dll"),"CompareMenuPointer"));
        uint64_t counts[2]{};
        bool next=(windowIndex++/3)%2!=0;
        if(compare)compare(next,counts);
        if(log)std::fprintf(log,"Menu pointer idleFilter=%d checked=%llu suppressed=%llu\n",sparse,
                (unsigned long long)counts[0],(unsigned long long)counts[1]);
        sparse=next;
    }
    if(routeAutoHandle && !autoDelivered && (state&13)==13){
        auto button=reinterpret_cast<P>(invoke("il2cpp_gchandle_get_target",routeAutoHandle));
        P error=nullptr, tap=nullptr;
        if(button){
            auto klass=reinterpret_cast<P>(invoke("il2cpp_object_get_class",button));
            tap=il2cpp_class_get_method_from_name(klass,"OnTapAutoRunButton",0);
            if(tap)invoke("il2cpp_runtime_invoke",tap,button,nullptr,&error);
        }
        autoDelivered=tap && !error;
        if(log)std::fprintf(log,"Checkpoint Auto button frame=%d delivered=%d exception=%d\n",id,autoDelivered,!!error);
        invoke("il2cpp_gchandle_free",routeAutoHandle);
        routeAutoHandle=0;
    }
    static const bool traceInput=std::getenv("REINCOMP_TRACE_DESKTOP_INPUT")!=nullptr;
    if((autoDelivered || traceInput) && (state&13)==13 && log){
        static auto mainCamera=reinterpret_cast<P(*)()>(registry.at("UnityEngine.Camera::get_main"));
        static auto transform=reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.Component::get_transform"));
        static auto position=reinterpret_cast<void(*)(P,float *)>(registry.at("UnityEngine.Transform::get_position_Injected"));
        if(auto camera=mainCamera()){
            if(auto pose=transform(camera)){
                float xyz[3]{};position(pose,xyz);
                std::fprintf(log,"%s camera frame=%d position=%.4f,%.4f,%.4f\n",autoDelivered?"Auto":"Input",id,xyz[0],xyz[1],xyz[2]);
            }
        }
    }
    std::sort(intervals.begin(),intervals.end());
    if(log){
        auto n=intervals.size();
        auto mean=std::chrono::duration<double,std::milli>(now-window).count()/n;
        auto spent=mainThreadMilliseconds()-windowCpu;
        static unsigned lastCaptured=0;
        static auto firstPicturebook=Clock::time_point{};
        if((state&14)==14 && firstPicturebook==Clock::time_point{})firstPicturebook=now;
        const bool sceneVisible=(state&1) || (firstPicturebook!=Clock::time_point{} && now-firstPicturebook>=std::chrono::seconds(45));
        if(capture && sceneVisible && (state&12)==12 && (state&3)!=lastCaptured){
            wchar_t exe[32768]{};
            GetModuleFileNameW(nullptr,exe,32768);
            std::wstring path=exe;
            path=path.substr(0,path.find_last_of(L"\\/")+1)+(state&1?L"bridge-native.png":L"storybook-native.png");
            screenshot(reinterpret_cast<P>(invoke("il2cpp_string_new_utf16",path.c_str(),path.size())),1,1);
            lastCaptured=state&3;
        }
        LARGE_INTEGER counter,frequency;
        QueryPerformanceCounter(&counter);
        QueryPerformanceFrequency(&frequency);
        const double qpcMs=1000.0*counter.QuadPart/frequency.QuadPart;
        auto missed=intervals.end()-std::lower_bound(intervals.begin(),intervals.end(),8.3);
        std::fprintf(log,"qpcMs=%.4f mode=%d frame=%d scene=%d size=%dx%d unityQuality=%d targetFps=%d actor=%s highest=%d loaded=%d renderInterval=%d intervals=%zu mean=%.3f p95=%.3f p99=%.3f max=%.3f ms mainCPU=%.3f ms/frame over8.3=%zu/%zu foreground=%u/%u\n",
            qpcMs,fullScreenMode(),id,handle,width(),height(),level(),target(),(state&1)?"3D":(state&2)?"2D":"none",bool(state&4),bool(state&8),renderInterval,n,mean,intervals[(95*n+99)/100-1],intervals[(99*n+99)/100-1],intervals.back(),spent/n,size_t(missed),n,foreground,focusSamples);
        frame_memory::record(log,id,qpcMs,invoke("il2cpp_gc_get_used_size"),invoke("il2cpp_gc_get_heap_size"));
        std::fflush(log);
    }
    intervals.clear();window=now;windowCpu=mainThreadMilliseconds();foreground=focusSamples=0;
}
