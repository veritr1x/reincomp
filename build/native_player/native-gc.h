// Driver for the native player's Windows threads, tied to the verified IL2CPP image.
extern "C" Memory *sub_825aad50(State &,addr_t,Memory *);
extern "C" Memory *sub_825aae3c(State &,addr_t,Memory *);
extern "C" Memory *sub_825b25d0(State &,addr_t,Memory *);
extern "C" Memory *sub_825b25cc(State &,addr_t,Memory *);
extern "C" Memory *sub_825b04a8(State &,addr_t,Memory *);
namespace native_gc {
constexpr unsigned limit=64;
constexpr size_t capacity=4u<<20;
struct Thread {
    std::atomic<uint64_t> token{};
    DWORD id{};
    HANDLE handle{};
    uintptr_t high{};
    const NT_TIB *tib{};
    unsigned char *snapshot{};
    std::atomic<unsigned> depth{};
    alignas(16) CONTEXT context{};
    size_t bytes{};
    bool suspended{},published{},scanned{};
    bool guestOwned{};
    addr_t record{},previousSp{},guestHigh{};
    uint8_t previousBlocked{};
};
static Thread pool[limit];
static std::atomic<unsigned> used{};
struct CurrentThreadTag; static constexpr recomp::ThreadLocalRef<Thread *,CurrentThreadTag> current{};
static bool stopped;
static bool livenessTraversal;
static bool scannedEpoch;
static unsigned epochs,scans;
static const bool measurePauses=std::getenv("REINCOMP_PROFILE_FRAME_CPU")!=nullptr;
static LARGE_INTEGER pauseStart;
struct SenderRoots {
    DWORD tid{};
    unsigned count{};
    State *parents[64]{};
    alignas(16) State regs[64]{};
    alignas(16) uint8_t guest[8u<<20]{};
    alignas(16) uint8_t windows[capacity]{};
    Thread native;
    size_t guestBytes{};
};
static SenderRoots sender;
template<class T> T load(addr_t address){T out;std::memcpy(&out,reinterpret_cast<const void *>(address),sizeof out);return out;}
template<class T> void store(addr_t address,T value){std::memcpy(reinterpret_cast<void *>(address),&value,sizeof value);}
[[noreturn]] static void fail(const char *reason){
    // Wake any thread parked by this diagnostic before reporting failure.
    for(auto &t:pool)if(t.suspended){ResumeThread(t.handle);t.suspended=false;}
    std::fprintf(stderr,"Native GC stopped: %s\n",reason);std::fflush(stderr);ExitProcess(132);
}
static Thread &attach(uint64_t token,bool guestOwned=false) {
    const auto index=used.fetch_add(1);
    if(index>=limit)fail("native thread slot limit");
    auto &t=pool[index];
    t.guestOwned=guestOwned;
    t.id=GetCurrentThreadId();
    if(!DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&t.handle,0,FALSE,DUPLICATE_SAME_ACCESS))fail("could not retain native thread");
    t.tib=reinterpret_cast<const NT_TIB *>(NtCurrentTeb());
    t.high=reinterpret_cast<uintptr_t>(t.tib->StackBase);
    t.snapshot=static_cast<unsigned char *>(VirtualAlloc(nullptr,capacity,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!t.snapshot)fail("native snapshot allocation");
    current=&t;t.token.store(token,std::memory_order_release);
    return t;
}
static void detach(){if(current){current->token.store(0,std::memory_order_release);current=nullptr;}}
struct GuestScope {
    Thread &slot;
    unsigned previous;
    // Only this thread enters or leaves its scopes. The collector reads depth.
    explicit GuestScope(Thread &thread):slot(thread),previous(thread.depth.load(std::memory_order_relaxed)){
        slot.depth.store(previous+1,std::memory_order_release);
    }
    ~GuestScope(){slot.depth.store(previous,std::memory_order_release);}
};
static addr_t findRecord(uint64_t token){
    auto h=uint32_t(token>>8)^uint32_t(token);h^=h>>16;
    addr_t r=load<addr_t>(0x879a6b28+8*(h&255));
    for(unsigned i=0;r&&i<4096;++i){
        if(load<uint64_t>(r+8)==token && !(load<uint8_t>(r+0x20)&1))return r;
        r=load<addr_t>(r);
    }
    return 0;
}
static void snapshot(Thread &t,bool self){
    t.context={};t.context.ContextFlags=CONTEXT_FULL|CONTEXT_FLOATING_POINT;
    if(self)RtlCaptureContext(&t.context);
    else if(!GetThreadContext(t.handle,&t.context))fail("could not read stopped native thread");
    auto sp=uintptr_t(t.context.Rsp);
    // Under Wine a thread paused inside host code, such as a Direct3D translation layer,
    // reports that code's stack pointer. Its Windows stack still holds every guest root,
    // so scan all of it from the lowest committed address.
    if(!self&&t.tib){
        const auto low=reinterpret_cast<uintptr_t>(t.tib->StackLimit);
        if(low&&low<t.high&&(!sp||sp>t.high||sp<low))sp=(low+15)&~uintptr_t(15);
    }
    if(!sp||sp>t.high||t.high-sp>capacity)fail("unexpected native stack bounds");
    t.bytes=t.high-sp;
    SIZE_T actual=0;
    if(t.bytes&&(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void *>(sp),t.snapshot,t.bytes,&actual)||actual!=t.bytes))
        fail("could not capture a complete native stack");
}
static Memory *stop(State &s,addr_t pc,Memory *m){
    if(stopped)fail("nested world stop");
    if(measurePauses)QueryPerformanceCounter(&pauseStart);
    stopped=true;scannedEpoch=false;++epochs;
    sender.count=0;sender.tid=GetCurrentThreadId();
    auto frames=recomp::CurrentExecutionFrames(s);
    if(frames.size()>1){
        if(frames.size()-1>std::size(sender.regs))fail("too many parent runtime frames");
        bunker::windows::AndroidThreadAttr attr{};
        if(threads->GetAttr(threads->Self(),&attr)||!attr.stack_base||!attr.stack_size)fail("missing collector stack");
        auto high=attr.stack_base+attr.stack_size,low=high;
        for(auto *f:frames){
            auto sp=f->gpr.sp.qword;
            if(sp<attr.stack_base||sp>high)fail("parent execution stack outside collector allocation");
            low=std::min(low,sp);
        }
        sender.guestBytes=high-low;
        if(sender.guestBytes>sizeof(sender.guest))fail("collector guest stack exceeds snapshot capacity");
        if(sender.guestBytes)std::memcpy(sender.guest,reinterpret_cast<const void *>(low),sender.guestBytes);
        for(size_t i=1;i<frames.size();++i){
            sender.parents[i-1]=frames[i];
            std::memcpy(&sender.regs[i-1],frames[i],sizeof(State));
        }
        sender.native.high=reinterpret_cast<uintptr_t>(reinterpret_cast<const NT_TIB *>(NtCurrentTeb())->StackBase);
        sender.native.snapshot=sender.windows;
        snapshot(sender.native,true);
        sender.count=static_cast<unsigned>(frames.size()-1);
    }
    const auto count=std::min(used.load(),limit);
    for(unsigned i=0;i<count;++i){
        auto &t=pool[i];t.bytes=0;t.scanned=false;
        const auto token=t.token.load(std::memory_order_acquire);
        if(!token)continue;
        const auto r=findRecord(token);if(!r)continue;
        bool self=t.id==GetCurrentThreadId();
        if(self){snapshot(t,true);continue;}
        if(t.guestOwned)continue;
        if(load<uint8_t>(0x879a62a8)!=1)fail("original collector has not enabled multithreaded locking");
        if(load<addr_t>(r+0x30)||load<addr_t>(r+0x38)||load<addr_t>(r+0x50))fail("unsupported split guest stack");
        bunker::windows::AndroidThreadAttr attr{};
        if(threads->GetAttr(token,&attr)||!attr.stack_base||!attr.stack_size)fail("missing guest stack allocation");
        const auto high=attr.stack_base+attr.stack_size;
        const auto scanHigh=(load<uint8_t>(r+0x20)&4)?load<addr_t>(0x879a6ac0):load<addr_t>(r+0x28);
        if(high!=scanHigh)fail("guest scanner high differs from registered stack");
        // The collector uses guest memory, signal dispatch and stderr while
        // workers are parked. Acquire those locks through the snapshot so a
        // suspended Unity thread cannot strand the collector behind one.
        recomp::WithGuestMemoryRead([&]{
            auto signalLock=signals->LockForThreadSuspension();
            _lock_file(stderr);
            if(SuspendThread(t.handle)==DWORD(-1)){
                _unlock_file(stderr);
                fail("cannot suspend external native thread");
            }
            t.suspended=true;
            snapshot(t,false);
            _unlock_file(stderr);
        });
        t.record=r;t.previousBlocked=load<uint8_t>(r+0x21);t.previousSp=load<addr_t>(r+0x18);t.guestHigh=high;
        // While inside game code the guest stack and all translated register spills can hold roots.
        // Outside an invocation it has returned in full. The Windows snapshot is scanned in either case.
        const auto guestLow=(t.depth.load()||t.previousBlocked)?attr.stack_base:high-16;
        store(r+0x18,guestLow);store<uint8_t>(r+0x21,1);t.published=true;
    }
    auto result=sub_825aad50(s,pc,m);
    // Android-created workers have now parked with the original collector's
    // signal handler. Keep its guest stack record and add their Windows roots.
    for(auto &t:pool){
        auto token=t.token.load(std::memory_order_acquire);
        if(!token||!t.guestOwned||t.id==GetCurrentThreadId()||!findRecord(token))continue;
        if(SuspendThread(t.handle)==DWORD(-1))fail("cannot snapshot parked Android worker");
        t.suspended=true;
        snapshot(t,false);
        if(ResumeThread(t.handle)==DWORD(-1))fail("cannot release Android worker snapshot");
        t.suspended=false;
    }
    return result;
}
static void visit(){
    if(!stopped||scannedEpoch)return;
    scannedEpoch=true;
    if(sender.count){
        auto push=[](const void *p,size_t bytes){if(bytes){auto low=reinterpret_cast<addr_t>(p);threads->Run(0x825b04a8,{low,low+bytes,0});}};
        push(sender.regs,sender.count*sizeof(State));
        push(sender.guest,sender.guestBytes);
        push(sender.native.snapshot,sender.native.bytes);
        push(&sender.native.context,sizeof(CONTEXT));
    }
    for(auto &t:pool)if(t.bytes){
        const auto low=reinterpret_cast<addr_t>(t.snapshot),regs=reinterpret_cast<addr_t>(&t.context);
        threads->Run(0x825b04a8,{low,low+t.bytes,0});
        threads->Run(0x825b04a8,{regs,regs+sizeof(t.context),0});
        t.scanned=true;++scans;
    }
}
static Memory *scan(State &s,addr_t pc,Memory *m){
    auto result=pc==0x825b25d0?sub_825b25d0(s,pc,m):sub_825b25cc(s,pc,m);
    visit();return result;
}
static Memory *section(State &s,addr_t pc,Memory *m){
    auto result=sub_825b04a8(s,pc,m);
    visit();return result;
}
static Memory *start(State &s,addr_t pc,Memory *m){
    auto result=sub_825aae3c(s,pc,m);
    if(!stopped)fail("restart without recorded stop");
    unsigned parked=0,scanned=0;size_t bytes=0;
    for(auto &t:pool){
        if(t.bytes){bytes+=t.bytes;if(t.scanned)++scanned;}
        if(t.published){
            if(!t.suspended||load<uint8_t>(t.record+0x21)!=1)fail("parked record changed");
            store(t.record+0x18,t.previousSp);store(t.record+0x21,t.previousBlocked);t.published=false;++parked;
        }
    }
    for(auto &t:pool)if(t.suspended){if(ResumeThread(t.handle)==DWORD(-1))fail("cannot restart native thread");t.suspended=false;}
    if(measurePauses){
        LARGE_INTEGER end;QueryPerformanceCounter(&end);
        static const double scale=[]{LARGE_INTEGER f;QueryPerformanceFrequency(&f);return 1000./f.QuadPart;}();
        static FILE *log=std::fopen("gc-pauses.tsv","w");
        if(log){
            std::fprintf(log,"%u\t%.4f\t%.3f\t%d\t%d\n",epochs,pauseStart.QuadPart*scale,
                (end.QuadPart-pauseStart.QuadPart)*scale,GetCurrentThreadId()==owner,livenessTraversal);
            std::fflush(log);
        }
    }
    if(sender.count){
        if(!livenessTraversal&&!scannedEpoch)fail("parent roots were not visited in this collection");
        if(epochs<100||epochs%40==0)std::fprintf(stderr,"Nested collector scanned %u saved register sets, %zu guest and %zu Windows stack bytes\n",sender.count,sender.guestBytes,sender.native.bytes);
    }
    sender.count=0;sender.tid=0;stopped=false;livenessTraversal=false;
    if(epochs<16||epochs%40==0)std::fprintf(stderr,"Native GC epoch=%u parked=%u scanned=%u native_stack_bytes=%zu total_root_ranges=%u\n",epochs,parked,scanned,bytes,2*scans);
    return result;
}
}
// The original external-stop entry holds the collector lock before its final
// tail branch. Liveness traverses the explicit roots submitted by the engine.
extern "C" __declspec(dllexport) Memory *NativeStopWorldForLiveness(State &s,addr_t pc,Memory *m){
    native_gc::livenessTraversal=true;
    return native_gc::stop(s,pc,m);
}
extern "C" bool NativePlayerSenderRoots(State *const *frames,size_t n){
    const auto &root=native_gc::sender;
    if(!native_gc::stopped||!root.count||GetCurrentThreadId()!=root.tid||n!=root.count+1||!root.native.bytes)return false;
    for(size_t i=1;i<n;++i)if(root.parents[i-1]!=frames[i])return false;
    return true;
}
