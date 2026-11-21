// Continuations present at the bridge's original scene-progress transition.
// Only the optional main-thread profiler installs these timing wrappers.
#define BRIDGE_CONTINUATIONS(X) \
    X(8281d448,"Receiver.ReceiveAsync") \
    X(8281d568,"FiniteStateMachineTask.DoUpdate") \
    X(8281d7a8,"FiniteStateMachineTask.UpdateEvent") \
    X(8281e018,"AutoActorPlayer.Update") \
    X(82820850,"UnitySynchronizationContext2.RunWorker") \
    X(82e5ce7c,"WorkRequest.Invoke") \
    X(843e9ae0,"SendOrPostCallback.Invoke") \
    X(82822be4,"Story.RequestUpdateForCurrentStory") \
    X(82786ca0,"Gameplay.BeginEventMap") \
    X(82787cf0,"Gameplay.WaitCompletionScene") \
    X(82788200,"Story.OnLiteUpdateSceneProgress") \
    X(83da6c84,"Octo.BaseExecutor.LateUpdate") \
    X(83d90ed0,"Octo.LoadAssetBundle.MoveNext") \
    X(83d9ae88,"Octo.Caching.IsCached") \
    X(83d9bae4,"Octo.Caching.UpdateStorageData") \
    X(84203148,"System.IO.File.OpenRead") \
    X(827e9d14,"CostumeEnhanceStatusIconChangeView.WaitAsync") \
    X(827c64bc,"DarkConsumableItemIcon.CheckCancel")
#define DECLARE_ENTRY(pc,label) extern "C" Memory *sub_##pc(State &,addr_t,Memory *);
BRIDGE_CONTINUATIONS(DECLARE_ENTRY)
#undef DECLARE_ENTRY

static std::atomic<State *> storyWorkState{};
// The AOT registration lists the exact closed types known to this game build.
// Record indices for types whose method arrays are first inflated during a walk.
static void traceMethodInflation(){
    recomp::AddGuestProbe(0x82576cd8,[](State &s){
        const auto loaded=loadingOut.load(std::memory_order_relaxed);
        if(!loaded || GetCurrentThreadId()!=owner || !(actorConditions.load(std::memory_order_relaxed)&1))return;
        auto load=[](addr_t p){addr_t v;std::memcpy(&v,reinterpret_cast<void *>(p),sizeof(v));return v;};
        const auto klass=s.gpr.x0.qword;
        const auto generic=load(klass+0x60);
        if(!generic || load(klass+0x98) || !uint16_t(load(klass+0x118)))return;
        static const auto indices=[&]{
            std::map<std::pair<uint32_t,addr_t>,unsigned> result;
            const auto registration=load(field_metadata::registrationSlot);
            const auto count=uint32_t(load(registration+0x30));
            const auto types=load(registration+0x38);
            for(unsigned i=0;i<count;++i){
                const auto type=load(types+8ull*i);
                if(!type || (load(type+8)&0x40ff0000)!=0x00150000)continue;
                const auto generic=load(type);
                if(generic && !load(generic+0x10))
                    result.emplace(std::make_pair(uint32_t(load(generic)),load(generic+8)),i);
            }
            return result;
        }();
        const auto found=indices.find({uint32_t(load(generic)),load(generic+8)});
        static FILE *log=std::fopen("field-inflation.tsv","w");
        if(log){
            std::fprintf(log,"%llu\t%d\t%u\t%.*s\n",GetTickCount64()-loaded,
                found==indices.end()?-1:int(found->second),uint32_t(load(generic)),160,
                reinterpret_cast<const char *>(load(klass+0x10)));
            std::fflush(log);
        }
    });
}
static DWORD WINAPI samplePostedWork(void *){
    static const auto scale=[]{LARGE_INTEGER f;QueryPerformanceFrequency(&f);return 1000./f.QuadPart;}();
    auto *log=std::fopen("posted-work-samples.tsv","w");
    if(!log)return 0;
    FILE *transition=nullptr;
    while(WaitForSingleObject(ownerHandle,1)==WAIT_TIMEOUT){
        auto *active=storyWorkState.load(std::memory_order_acquire);
        const auto loaded=loadingOut.load(std::memory_order_relaxed);
        auto age=GetTickCount64()-loaded;
        const bool crossing=loaded && (age<=5000 || (age>=30000 && age<=49000)) &&
            (actorConditions.load(std::memory_order_relaxed)&1);
        if(!active && !crossing)continue;
        CONTEXT ctx{};ctx.ContextFlags=CONTEXT_CONTROL;
        LARGE_INTEGER at;
        uintptr_t guestStack[240]{},hostStack[192]{};
        SIZE_T guestBytes=0,hostBytes=0;
        if(SuspendThread(ownerHandle)==DWORD(-1))break;
        const bool live=(crossing || storyWorkState.load(std::memory_order_acquire)==active) && GetThreadContext(ownerHandle,&ctx);
        const bool sameWork=active && storyWorkState.load(std::memory_order_acquire)==active;
        const char *phase=ownerCall.load(std::memory_order_relaxed);
        QueryPerformanceCounter(&at);
        if(live){
            if(sameWork)ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void *>(active->gpr.sp.qword),guestStack,sizeof(guestStack),&guestBytes);
            ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void *>(ctx.Rsp),hostStack,sizeof(hostStack),&hostBytes);
        }
        ResumeThread(ownerHandle);
        if(!live)continue;
        static auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"GuestRuntime.dll"));
        static auto size=reinterpret_cast<IMAGE_NT_HEADERS *>(base+reinterpret_cast<IMAGE_DOS_HEADER *>(base)->e_lfanew)->OptionalHeader.SizeOfImage;
        if(crossing){
            if(!transition)transition=std::fopen("transition-samples.tsv","w");
            static auto unity=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"UnityPlayer.dll"));
            static auto unitySize=reinterpret_cast<IMAGE_NT_HEADERS *>(unity+reinterpret_cast<IMAGE_DOS_HEADER *>(unity)->e_lfanew)->OptionalHeader.SizeOfImage;
            if(transition){
                std::fprintf(transition,"%.4f\t%s\t",at.QuadPart*scale,phase);
                auto emit=[&](uintptr_t address){
                    if(address>=base && address-base<size)std::fprintf(transition,"g%llx ",(unsigned long long)(address-base));
                    else if(address>=unity && address-unity<unitySize)std::fprintf(transition,"u%llx ",(unsigned long long)(address-unity));
                };
                emit(ctx.Rip);std::fputc('\t',transition);
                for(size_t i=0;i<hostBytes/8;++i)emit(hostStack[i]);
                std::fputc('\n',transition);
                static unsigned lines=0;if(!(++lines%128))std::fflush(transition);
            }
        }else if(transition){std::fflush(transition);}
        if(!sameWork)continue;
        std::fprintf(log,"%.4f\t%llx\t",at.QuadPart*scale,(unsigned long long)(ctx.Rip>=base && ctx.Rip-base<size?ctx.Rip-base:0));
        for(size_t i=0;i<guestBytes/8;++i)if(guestStack[i]>=0x82000000 && guestStack[i]<0x87000000)
            std::fprintf(log,"%llx ",(unsigned long long)guestStack[i]);
        std::fputc('\t',log);
        for(size_t i=0;i<hostBytes/8;++i)if(hostStack[i]>=base && hostStack[i]-base<size)
            std::fprintf(log,"%llx ",(unsigned long long)(hostStack[i]-base));
        std::fputc('\n',log);std::fflush(log);
    }
    if(transition)std::fclose(transition);
    std::fclose(log);return 0;
}

static Memory *timeStoryWork(State &s,addr_t pc,Memory *memory,recomp::Entry original,const char *name){
    if(GetCurrentThreadId()!=owner || !(actorConditions.load(std::memory_order_relaxed)&1))
        return original(s,pc,memory);
    static const auto frequency=[]{LARGE_INTEGER f;QueryPerformanceFrequency(&f);return f.QuadPart;}();
    LARGE_INTEGER begin,end;
    addr_t callback=0;
    if(pc==0x82e5ce7c || pc==0x843e9ae0){
        addr_t delegate=s.gpr.x0.qword;
        if(delegate && pc==0x82e5ce7c)std::memcpy(&delegate,reinterpret_cast<void *>(delegate),sizeof(delegate));
        if(delegate)std::memcpy(&callback,reinterpret_cast<void *>(delegate+0x10),sizeof(callback));
    }
    const bool sample=sampleStoryStacks && loadingOut.load(std::memory_order_relaxed)!=0 &&
        (pc==0x82820850 || pc==0x8281e018 || pc==0x82786ca0 || pc==0x82787cf0 || pc==0x83da6c84);
    struct SampleScope {
        bool enabled;
        State *previous;
        ~SampleScope(){if(enabled)storyWorkState.store(previous,std::memory_order_release);}
    } scope{sample,sample?storyWorkState.exchange(&s,std::memory_order_acq_rel):nullptr};
    QueryPerformanceCounter(&begin);
    auto result=original(s,pc,memory);
    QueryPerformanceCounter(&end);
    const auto ticks=end.QuadPart-begin.QuadPart;
    if(ticks*1000>=frequency || pc==0x830cfc3c){
        static FILE *log=std::fopen("story-work.tsv","w");
        if(log){
            std::fprintf(log,"%.4f\t%.3f\t%s",1000.*begin.QuadPart/frequency,1000.*ticks/frequency,name);
            if(callback)std::fprintf(log," 0x%llx",(unsigned long long)callback);
            std::fputc('\n',log);
            std::fflush(log);
        }
    }
    return result;
}
static void profileStoryWork(){
#define TIME_ENTRY(pc,label) \
    recomp::AddGuestDirectEntry(0x##pc,[](State &s,addr_t entry,Memory *memory){ \
        return timeStoryWork(s,entry,memory,sub_##pc,label); \
    });
    BRIDGE_CONTINUATIONS(TIME_ENTRY)
#undef TIME_ENTRY
}
#undef BRIDGE_CONTINUATIONS
