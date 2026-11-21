// Correlate Windows thread cycle counts with elapsed time at Unity's input
// and render callbacks. Keep raw counts so the analysis can check clock rates.
namespace frame_cpu {
static const bool enabled=std::getenv("REINCOMP_PROFILE_FRAME_CPU")!=nullptr;
struct Mark { double elapsed; ULONG64 cycles; };
static thread_local Mark input,render;
static thread_local int frame=-1;
static Mark mark(){
    static const double scale=[]{LARGE_INTEGER f;QueryPerformanceFrequency(&f);return 1000./f.QuadPart;}();
    LARGE_INTEGER time;QueryPerformanceCounter(&time);
    ULONG64 cycles=0;QueryThreadCycleTime(GetCurrentThread(),&cycles);
    return {time.QuadPart*scale,cycles};
}
static void begin(int id){
    frame=id;input=mark();render={};
}
static void beginRender(){
    if(frame>=0 && !render.cycles)render=mark();
}
static void finishRender(){
    if(!render.cycles)return;
    const auto end=mark();
    FILETIME creation,exit,kernel,user;
    if(!GetThreadTimes(GetCurrentThread(),&creation,&exit,&kernel,&user))return;
    auto ticks=[](FILETIME t){return (uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
    static FILE *log=std::fopen("frame-cpu.tsv","w");
    if(log){
        std::fprintf(log,"%d\t%.4f\t%llu\t%.4f\t%llu\t%.4f\t%llu\t%llu\n",frame,
            input.elapsed,input.cycles,render.elapsed,render.cycles,end.elapsed,end.cycles,ticks(kernel)+ticks(user));
        if(!(frame%256))std::fflush(log);
    }
    frame=-1;render={};
}
}
