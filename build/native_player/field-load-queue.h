// Octo's LateUpdate starts eligible requests synchronously, then tests whether
// it can admit another. Only that second test can yield the remaining queue.
namespace field_load_queue {
// Unity invokes the loading curtain and the Octo executor on the player thread.
static bool loading;
extern "C" Memory *sub_83da6318(State &,addr_t,Memory *);
static Memory *canExecute(State &s,addr_t pc,Memory *memory){
    const auto caller=s.gpr.x30.qword;
    const auto executor=s.gpr.x0.qword;
    auto result=sub_83da6318(s,pc,memory);
    if((caller!=0x83da6ccc && caller!=0x83da6dec) || s.gpr.pc.qword!=caller ||
       GetCurrentThreadId()!=owner)return result;
    static addr_t active=0;
    static int64_t start=0;
    static unsigned admitted=0;
    static int64_t budgetMicros=750;
    if(caller==0x83da6ccc){
        active=0;
        const auto loaded=loadingOut.load(std::memory_order_relaxed);
        const auto now=GetTickCount64();
        if(!s.gpr.x0.dword || field_metadata::read<int32_t>(executor+0x2c)<2)return result;
        if(!loading && (!loaded || now-loaded<3000 ||
           now-actorUpdated.load(std::memory_order_relaxed)>=500 ||
           !(actorConditions.load(std::memory_order_relaxed)&1)))return result;
        // In a loading screen one expensive request must always make progress.
        // Give Unity a chance to pump input before admitting another batch.
        budgetMicros=loading?16000:750;
        active=executor;
        admitted=0;
        LARGE_INTEGER at;QueryPerformanceCounter(&at);start=at.QuadPart;
    }else if(active==executor){
        ++admitted;
        if(!s.gpr.x0.dword || !field_metadata::read<int32_t>(executor+0x2c))return result;
        static auto frequency=[]{LARGE_INTEGER f;QueryPerformanceFrequency(&f);return f.QuadPart;}();
        LARGE_INTEGER at;QueryPerformanceCounter(&at);
        if((at.QuadPart-start)*1000000<frequency*budgetMicros)return result;
        s.gpr.x0.qword=0;
        active=0;
        if(std::getenv("REINCOMP_PROFILE_STORY_WORK")){
            static FILE *log=std::fopen("field-load-queue.tsv","w");
            if(log){std::fprintf(log,"%.4f\t%.3f\t%u\t%d\n",1000.*start/frequency,1000.*(at.QuadPart-start)/frequency,
                admitted,field_metadata::read<int32_t>(executor+0x2c));std::fflush(log);}
        }
    }
    return result;
}
}
