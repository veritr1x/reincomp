#include "../../build/linux_player/posix_threads.h"
#include <atomic>
#include <cstdio>
#include <thread>

int main() {
    constexpr addr_t destructor = 0x123400;
    constexpr addr_t start = 0x234500;
    constexpr addr_t initialize = 0x345600, nestedInitialize = 0x345610;
    pthread_once_t first = PTHREAD_ONCE_INIT, nested = PTHREAD_ONCE_INIT;
    std::atomic<unsigned> initCalls{}, nestedCalls{};
    std::atomic<unsigned> calls{};
    int value = 31;
    unsigned key;
    PosixThreads threads([&](addr_t pc,addr_t arg) -> addr_t {
        if (pc == initialize) {
            ++initCalls;
            State state{};
            state.gpr.x0.qword = uintptr_t(&nested);
            state.gpr.x1.qword = nestedInitialize;
            threads.lookup("pthread_once")(state);
            return state.gpr.x0.qword;
        }
        if (pc == nestedInitialize) {
            ++nestedCalls;
            return 0;
        }
        if (pc == start) {
            auto &local=threads.local();
            if (local.stackSize != 65536 || local.inheritedMask != 0x100 || arg != 17) std::abort();
            local.execution=std::make_shared<int>(42);
            State s{};
            s.gpr.x0.qword=key;
            s.gpr.x1.qword=uintptr_t(&value);
            threads.lookup("pthread_setspecific")(s);
            if (s.gpr.x0.qword) std::abort();
            return arg+1;
        }
        if (pc != destructor || arg != uintptr_t(&value)) std::abort();
        auto &local=threads.local();
        if (local.inheritedMask && (!local.execution || *static_cast<int *>(local.execution.get()) != 42)) std::abort();
        ++calls;
        return 0;
    },{},[]{return uint64_t(0x100);});
    auto call = [&](const char *name, addr_t a, addr_t b=0, addr_t c=0, addr_t d=0) {
        State state{};
        state.gpr.x0.qword = a;
        state.gpr.x1.qword = b;
        state.gpr.x2.qword = c;
        state.gpr.x3.qword = d;
        threads.lookup(name)(state);
        return state.gpr.x0.qword;
    };
    auto runOnce = [&] { if (call("pthread_once", uintptr_t(&first), initialize)) std::abort(); };
    std::thread initializer(runOnce);
    runOnce();
    initializer.join();
    if (initCalls != 1 || nestedCalls != 1) return 1;
    std::puts("PASS concurrent and nested Linux guest pthread_once initializers");
    if (call("pthread_key_create",uintptr_t(&key),destructor)) return 1;
    std::thread worker([&] {
        if (call("pthread_setspecific",key,uintptr_t(&value)) || call("pthread_getspecific",key) != uintptr_t(&value))
            std::abort();
    });
    worker.join();
    if (calls != 1 || call("pthread_getspecific",key)) return 1;
    bunker::windows::AndroidThreadAttr attr{};
    pthread_t thread;
    void *returned=nullptr;
    if (call("pthread_attr_init",uintptr_t(&attr)) || call("pthread_attr_setstacksize",uintptr_t(&attr),65536) ||
        call("pthread_create",uintptr_t(&thread),uintptr_t(&attr),start,17) ||
        call("pthread_join",thread,uintptr_t(&returned)) || uintptr_t(returned) != 18 ||
        calls != 2 || call("pthread_key_delete",key) || call("pthread_attr_destroy",uintptr_t(&attr))) return 1;

    alignas(8) uint32_t recursive[10]{0x4000};
    addr_t mutex = uintptr_t(recursive);
    if (call("pthread_mutex_lock",mutex) || call("pthread_mutex_trylock",mutex)) return 1;
    std::thread contender([&] {
        if (call("pthread_mutex_trylock",mutex) != EBUSY) std::abort();
    });
    contender.join();
    if (call("pthread_mutex_unlock",mutex) || call("pthread_mutex_unlock",mutex) || call("pthread_mutex_destroy",mutex)) return 1;
    uint32_t conditionWord=0;
    uint32_t plainMutex[10]{};
    auto condition=uintptr_t(&conditionWord), plain=uintptr_t(plainMutex);
    auto wake=std::make_shared<bunker::windows::SignalWake>();
    std::atomic<bool> enrolling{},pending{};
    unsigned delivered=0;
    threads.setSignalWait([&]{ enrolling=true; return wake; },[&]{return pending.load();},[&]{
        if (call("pthread_mutex_trylock",plain) || call("pthread_mutex_unlock",plain)) std::abort();
        ++delivered;
        pending=false;
    });
    for (bool interrupt:{false,true}) {
        enrolling=false;
        std::thread waiter([&]{
            if (call("pthread_mutex_lock",plain) || call("pthread_cond_wait",condition,plain) ||
                call("pthread_mutex_unlock",plain)) std::abort();
        });
        while (!enrolling) std::this_thread::yield();
        if (call("pthread_mutex_lock",plain)) return 1;
        if (!interrupt && call("pthread_cond_signal",condition)) return 1;
        if (call("pthread_mutex_unlock",plain)) return 1;
        if (interrupt) { pending=true; wake->Notify(); }
        waiter.join();
    }
    if (delivered!=1 || call("pthread_cond_destroy",condition) || call("pthread_mutex_destroy",plain)) return 1;
    std::puts("PASS Linux worker creation/join, inherited guest mask, TLS destructor and recursive mutex contention");
    std::puts("PASS guest condition notification and signal delivery with its mutex released");
}
