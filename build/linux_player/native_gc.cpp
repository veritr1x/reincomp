#include <signal.h>
#include "native_gc.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

extern "C" Memory *__real_sub_825aad50(State &, addr_t, Memory *);
extern "C" Memory *sub_825aae3c(State &, addr_t, Memory *);
extern "C" Memory *sub_825b25d0(State &, addr_t, Memory *);
extern "C" Memory *sub_825b25cc(State &, addr_t, Memory *);
extern "C" Memory *sub_825b04a8(State &, addr_t, Memory *);

namespace linux_gc {
NativeCollector *NativeCollector::active;
thread_local NativeCollector::Thread *NativeCollector::local;
namespace {
[[noreturn]] void fail(const char *message) {
    std::fprintf(stderr,"Linux native GC: %s\n",message);
    std::abort();
}
template<class T> T load(addr_t address) {
    T data;
    std::memcpy(&data,reinterpret_cast<const void *>(address),sizeof(data));
    return data;
}
template<class T> void store(addr_t address,T data) {
    std::memcpy(reinterpret_cast<void *>(address),&data,sizeof(data));
}
// GC_threads and its record layout are from the pinned Global Boehm image.
addr_t recordFor(pthread_t id) {
    auto hash=uint32_t(id>>8)^uint32_t(id);
    hash^=hash>>16;
    auto record=load<addr_t>(0x879a6b28 + 8*(hash&255));
    for (unsigned n=0;record && n<4096;++n) {
        if (load<uint64_t>(record+8)==id && !(load<uint8_t>(record+0x20)&1)) return record;
        record=load<addr_t>(record);
    }
    return 0;
}
enum Phase { idle, requested, suspended, released };
static_assert(std::atomic<int>::is_always_lock_free && sizeof(std::atomic<int>)==4);
void notify(std::atomic<int> &phase,int next) {
    phase.store(next,std::memory_order_release);
    syscall(SYS_futex,&phase,FUTEX_WAKE_PRIVATE,1,nullptr,nullptr,0);
}
void waitWhile(std::atomic<int> &phase,int expected,bool bounded) {
    const timespec limit{5,0};
    while (phase.load(std::memory_order_acquire)==expected) {
        if (syscall(SYS_futex,&phase,FUTEX_WAIT_PRIVATE,expected,bounded?&limit:nullptr,nullptr,0)<0 &&
            errno==ETIMEDOUT && phase.load(std::memory_order_acquire)==expected) fail("thread did not acknowledge a collector request");
    }
}
}

NativeCollector::NativeCollector(Run pushRoots,Park withProviderLocks) : push(std::move(pushRoots)),locked(std::move(withProviderLocks)) {
    struct sigaction action{};
    action.sa_handler=onSignal;
    action.sa_flags=SA_RESTART;
    sigemptyset(&action.sa_mask);
    for (int candidate=SIGRTMAX;candidate>=SIGRTMIN;++candidate) {
        if (!sigaction(candidate,nullptr,&previous) && previous.sa_handler==SIG_DFL &&
            !sigaction(candidate,&action,nullptr)) { signal=candidate; break; }
    }
    if (!signal) fail("no unused native real-time signal");
    active=this;
}
NativeCollector::~NativeCollector() {
    if (signal) sigaction(signal,&previous,nullptr);
    active=nullptr;
}
std::shared_ptr<NativeCollector::Thread> NativeCollector::attach(uintptr_t guestLow,size_t bytes) {
    auto thread=std::make_shared<Thread>();
    thread->id=pthread_self();
    pthread_attr_t attr;
    if (pthread_getattr_np(thread->id,&attr)) fail("could not locate native thread stack");
    void *stack=nullptr; size_t size=0;
    int error=pthread_attr_getstack(&attr,&stack,&size);
    pthread_attr_destroy(&attr);
    if (error || !stack || !size) fail("invalid native thread stack bounds");
    thread->nativeLow=uintptr_t(stack); thread->nativeHigh=thread->nativeLow+size;
    thread->guestLow=guestLow; thread->guestHigh=guestLow+bytes;
    local=thread.get();
    sigset_t set,old;
    sigemptyset(&set); sigaddset(&set,signal);
    if (pthread_sigmask(SIG_UNBLOCK,&set,&old)) fail("could not receive collector signal");
    thread->originallyMasked=sigismember(&old,signal)==1;
    std::lock_guard guard(mutex);
    threads.push_back(thread);
    return thread;
}
void NativeCollector::retire(const std::shared_ptr<Thread> &thread) {
    std::lock_guard guard(mutex);
    threads.erase(std::remove(threads.begin(),threads.end(),thread),threads.end());
    if (thread->originallyMasked) {
        sigset_t set; sigemptyset(&set); sigaddset(&set,signal);
        pthread_sigmask(SIG_BLOCK,&set,nullptr);
    }
    local=nullptr;
}
void NativeCollector::onSignal(int) {
    int saved=errno;
    auto *thread=local;
    if (thread && thread->phase.load(std::memory_order_acquire)==requested) {
        // The kernel's signal frame above this local includes interrupted CPU
        // and SIMD registers and the x86-64 red zone. Keep it alive during GC.
        alignas(16) volatile uintptr_t marker=0;
        thread->stoppedLow=reinterpret_cast<uintptr_t>(&marker);
        notify(thread->phase,suspended);
        waitWhile(thread->phase,suspended,false);
        notify(thread->phase,idle);
    }
    errno=saved;
}
void NativeCollector::park(Thread &thread) {
    thread.phase.store(requested,std::memory_order_release);
    if (pthread_kill(thread.id,signal)) fail("could not signal registered Unity thread");
    waitWhile(thread.phase,requested,true);
    if (thread.stoppedLow<thread.nativeLow || thread.stoppedLow>=thread.nativeHigh) fail("signal frame outside native stack");
}
void NativeCollector::resume(Thread &thread) {
    notify(thread.phase,released);
    waitWhile(thread.phase,released,true);
}
void NativeCollector::capture() {
    if (stopped || !local) fail("invalid collector stop");
    stopped=true; scanned=false;
    if (getcontext(&senderContext)) fail("could not capture collector registers");
    auto sp=uintptr_t(senderContext.uc_mcontext.gregs[REG_RSP]);
    if (sp<local->nativeLow+128 || sp>local->nativeHigh) fail("collector stack pointer out of range");
    sp-=128;
    senderStack.resize(local->nativeHigh-sp);
    std::memcpy(senderStack.data(),reinterpret_cast<void *>(sp),senderStack.size());
    std::lock_guard guard(mutex);
    parked.clear(); parked.reserve(threads.size());
    for (const auto &thread:threads) {
        if (thread.get()==local) continue;
        auto record=recordFor(thread->id);
        if (!record) continue;
        auto high=(load<uint8_t>(record+0x20)&4) ? load<addr_t>(0x879a6ac0) : load<addr_t>(record+0x28);
        if (high!=thread->guestHigh || load<addr_t>(record+0x30) || load<addr_t>(record+0x38) || load<addr_t>(record+0x50))
            fail("unsupported registered guest stack");
        parked.push_back({thread,record,load<addr_t>(record+0x18),load<uint8_t>(record+0x21)});
    }
    locked([&]{
        for (auto &slot:parked) {
            park(*slot.thread);
            auto low=(slot.thread->depth.load()||slot.oldBlocked)?slot.thread->guestLow:slot.thread->guestHigh-16;
            store(slot.record+0x18,low);
            store<uint8_t>(slot.record+0x21,1);
        }
    });
}
void NativeCollector::scan() {
    if (!stopped || scanned) return;
    scanned=true;
    auto roots=[&](const void *data,size_t bytes){
        if (bytes) push(0x825b04a8,uintptr_t(data),uintptr_t(data)+bytes);
    };
    roots(&senderContext,sizeof(senderContext));
    roots(senderStack.data(),senderStack.size());
    for (const auto &slot:parked) roots(reinterpret_cast<const void *>(slot.thread->stoppedLow),slot.thread->nativeHigh-slot.thread->stoppedLow);
}
void NativeCollector::release() {
    if (!stopped) fail("restart without a collector stop");
    for (auto &slot:parked) {
        store(slot.record+0x18,slot.oldSp);
        store(slot.record+0x21,slot.oldBlocked);
    }
    for (auto &slot:parked) resume(*slot.thread);
    parked.clear(); stopped=false;
}
Memory *NativeCollector::stopWorld(State &s,addr_t pc,Memory *m) {
    active->capture();
    return __real_sub_825aad50(s,pc,m);
}
// The liveness entry tail-calls the original stop across object files. It
// bypasses the indirect dispatch table after acquiring the collector lock.
extern "C" Memory *__wrap_sub_825aad50(State &s,addr_t pc,Memory *m) { return NativeCollector::stopWorld(s,pc,m); }
void NativeCollector::install() {
    recomp::AddGuestDirectEntry(0x825aad50,stopWorld);
    recomp::AddGuestDirectEntry(0x825aae3c,[](State &s,addr_t pc,Memory *m){
        auto out=sub_825aae3c(s,pc,m);
        active->release(); return out;
    });
    auto scan=[](State &s,addr_t pc,Memory *m){
        auto out=pc==0x825b25cc?sub_825b25cc(s,pc,m):sub_825b25d0(s,pc,m);
        active->scan(); return out;
    };
    recomp::AddGuestDirectEntry(0x825b25cc,scan);
    recomp::AddGuestDirectEntry(0x825b25d0,scan);
    recomp::AddGuestDirectEntry(0x825b04a8,[](State &s,addr_t pc,Memory *m){ auto out=sub_825b04a8(s,pc,m); active->scan(); return out; });
}
}
