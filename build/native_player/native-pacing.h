// Run on Unity's graphics worker after the scene has submitted its last camera.
// The scene and its input can start preparing the next frame in the meantime.
namespace render_pacing {
static const int rate=[]{
    const char *setting=std::getenv("REINCOMP_NATIVE_PRESENT_RATE");
    if(!setting)return 0;
    char *end=nullptr;const long value=std::strtol(setting,&end,10);
    return end && !*end && value>120 && value<=165?int(value):0;
}();
static void WINAPI releaseFrame(int frame){
    static double deadline=0;
    if(frame<0){deadline=0;return;}
    const double period=1000./rate;
    const double arrived=render_timing::nowMs();
    if(!deadline || arrived>deadline+period)deadline=arrived;
    static HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,2,TIMER_ALL_ACCESS);
    if(!timer)return;
    const double wait=deadline-arrived;
    if(wait>0.20){
        LARGE_INTEGER relative;relative.QuadPart=-static_cast<LONGLONG>((wait-0.15)*10000.);
        if(SetWaitableTimer(timer,&relative,0,nullptr,nullptr,FALSE))WaitForSingleObject(timer,INFINITE);
    }
    while(render_timing::nowMs()<deadline)YieldProcessor();
    const double released=render_timing::nowMs();
    static FILE *log=std::getenv("REINCOMP_NATIVE_RENDER_TIMING")?std::fopen("frame-release.csv","w"):nullptr;
    if(log){
        std::fprintf(log,"%d,%.4f,%.4f\n",frame,arrived,released);
        if(!(frame%128))std::fflush(log);
    }
    deadline=(std::max)(deadline,released-0.15)+period;
}
static void afterPipeline(){
    if(!rate || !render_timing::issue || !render_timing::renderedFrame)return;
    static auto actor=reinterpret_cast<unsigned(*)()>(GetProcAddress(GetModuleHandleW(L"GuestRuntime.dll"),"NativeActorConditions"));
    static int last=-1;
    const int frame=render_timing::renderedFrame();
    if(frame==last)return;
    last=frame;
    DWORD foreground=0;GetWindowThreadProcessId(GetForegroundWindow(),&foreground);
    static bool pacing=false;
    const bool eligible=actor && (actor()&13)==13 && foreground==GetCurrentProcessId();
    if(eligible)render_timing::issue(releaseFrame,frame);
    else if(pacing)render_timing::issue(releaseFrame,-1);
    pacing=eligible;
}
}
