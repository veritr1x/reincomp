// Schedule one native render-thread callback per distinct Unity render frame.
namespace render_timing {
using Event=void(WINAPI *)(int);
static void (*issue)(Event,int);
static int (*renderedFrame)();
static bool (*batching)();
static void (*setBatching)(bool);
static const bool compareBatching=std::getenv("REINCOMP_COMPARE_SRP_BATCHING")!=nullptr;
static const bool compareAffinity=std::getenv("REINCOMP_COMPARE_MAIN_AFFINITY")!=nullptr;
static int requestedBatching=-1;
static unsigned chosenCore;
static DWORD_PTR previousAffinity;
static bool applyAffinity(bool pin){
    if(pin){
        previousAffinity=SetThreadAffinityMask(GetCurrentThread(),DWORD_PTR(1)<<chosenCore);
        return previousAffinity!=0;
    }
    if(previousAffinity && !SetThreadAffinityMask(GetCurrentThread(),previousAffinity))return false;
    previousAffinity=0;
    return true;
}
static double nowMs(){
    static const double scale=[] {LARGE_INTEGER f;QueryPerformanceFrequency(&f);return 1000.0/f.QuadPart;}();
    LARGE_INTEGER q;QueryPerformanceCounter(&q);return q.QuadPart*scale;
}
static void WINAPI completed(int frame){
    static auto gl=GetModuleHandleW(L"opengl32.dll");
    using CurrentDC=HDC(WINAPI *)();
    using Proc=PROC(WINAPI *)(LPCSTR);
    using Sync=BOOL(WINAPI *)(HDC,INT64 *,INT64 *,INT64 *);
    static auto dc=gl?reinterpret_cast<CurrentDC>(GetProcAddress(gl,"wglGetCurrentDC")):nullptr;
    static auto lookup=gl?reinterpret_cast<Proc>(GetProcAddress(gl,"wglGetProcAddress")):nullptr;
    static Sync sync=[] {
        auto address=lookup?reinterpret_cast<uintptr_t>(lookup("wglGetSyncValuesOML")):0;
        return address>65535 && address!=UINTPTR_MAX?reinterpret_cast<Sync>(address):nullptr;
    }();
    auto at=nowMs();
    INT64 ust=0,msc=0,sbc=0;
    bool got=sync && dc && dc() && sync(dc(),&ust,&msc,&sbc);
    static FILE *log=std::fopen("render-thread.csv","w");
    if(log){
        std::fprintf(log,"%d,%.4f,%d,%lld,%lld,%lld\n",frame,at,got,ust,msc,sbc);
        if(!(frame%64))std::fflush(log);
    }
}
static void compareFrame(int frame,double at){
    static auto actor=reinterpret_cast<unsigned(*)()>(GetProcAddress(GetModuleHandleW(L"GuestRuntime.dll"),"NativeActorConditions"));
    static double start=0;
    static int original=-1,previousPhase=-1;
    static bool done=false;
    if(done || (!compareAffinity && (!batching || !setBatching)) || !actor)return;
    const unsigned state=actor();
    if((state&13)!=13)return;
    if(!start){
        start=at;original=compareAffinity?0:batching();
        chosenCore=GetCurrentProcessorNumber();
    }
    const int phase=int((at-start)/12000);
    static constexpr int sequence[]={0,1,1,0,1,0,0,1};
    const bool actual=compareAffinity?previousAffinity!=0:batching();
    ULONG64 cycles=0;QueryThreadCycleTime(GetCurrentThread(),&cycles);
    static FILE *profile=std::fopen("render-profile.tsv","w");
    if(profile){
        std::fprintf(profile,"%d\t%.4f\t%llu\t%lu\t%d\t%d\t%d\n",frame,at,
            (unsigned long long)cycles,GetCurrentProcessorNumber(),previousPhase,actual,original);
        if(!(frame%128))std::fflush(profile);
    }
    if(phase>=int(sizeof(sequence)/sizeof(*sequence))){
        if(compareAffinity){if(!applyAffinity(false))std::abort();}
        else {requestedBatching=-1;setBatching(original!=0);}
        done=true;
        if(profile)std::fflush(profile);
        return;
    }
    if(compareAffinity){
        if(actual!=bool(sequence[phase]) && !applyAffinity(sequence[phase]!=0))std::abort();
    }else{
        requestedBatching=sequence[phase];
        if(actual!=bool(requestedBatching))setBatching(requestedBatching!=0);
    }
    previousPhase=phase;
}
static void submit(State &s,void *fn){
    invoke<void,P>(s,fn);
    if(!issue || !renderedFrame)return;
    int frame=renderedFrame();
    static int last=-1;
    if(frame==last)return;
    last=frame;
    const double at=nowMs();
    if(compareBatching || compareAffinity)compareFrame(frame,at);
    static FILE *log=std::fopen("render-submit.csv","w");
    if(log){
        std::fprintf(log,"%d,%.4f\n",frame,at);
        if(!(frame%64))std::fflush(log);
    }
    issue(completed,frame);
}
static Call bind(const char *key,void *fn){
    if(compareBatching){
        if(!std::strcmp(key,"UnityEngine.Rendering.GraphicsSettings::get_useScriptableRenderPipelineBatching"))
            batching=reinterpret_cast<decltype(batching)>(fn);
        if(!std::strcmp(key,"UnityEngine.Rendering.GraphicsSettings::set_useScriptableRenderPipelineBatching")){
            setBatching=reinterpret_cast<decltype(setBatching)>(fn);
            return [](State &s,void *){setBatching(requestedBatching<0?bool(s.gpr.x0.dword):bool(requestedBatching));};
        }
    }
    if(!std::strcmp(key,"UnityEngine.GL::GLIssuePluginEvent"))issue=reinterpret_cast<decltype(issue)>(fn);
    if(!std::strcmp(key,"UnityEngine.Time::get_renderedFrameCount"))renderedFrame=reinterpret_cast<decltype(renderedFrame)>(fn);
    if(!std::strcmp(key,"UnityEngine.Rendering.ScriptableRenderContext::Submit_Internal_Injected") &&
       std::getenv("REINCOMP_NATIVE_RENDER_TIMING"))return submit;
    return nullptr;
}
}
