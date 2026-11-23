#pragma once
#include "runtime.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <vector>

namespace linux_gc {
class NativeCollector {
public:
    struct Thread {
        pthread_t id{};
        uintptr_t nativeLow{}, nativeHigh{}, guestLow{}, guestHigh{};
        std::atomic<unsigned> depth{};
        std::atomic<int> phase{};
        uintptr_t stoppedLow{};
        bool originallyMasked{};
    };
    using Run = std::function<void(addr_t, addr_t, addr_t)>;
    using Park = std::function<void(const std::function<void()> &)>;
    NativeCollector(Run pushRoots, Park withProviderLocks);
    ~NativeCollector();
    std::shared_ptr<Thread> attach(uintptr_t guestLow, size_t bytes);
    void retire(const std::shared_ptr<Thread> &);
    void install();
    static Memory *stopWorld(State &,addr_t,Memory *);
private:
    struct Parked {
        std::shared_ptr<Thread> thread;
        addr_t record, oldSp;
        uint8_t oldBlocked;
    };
    std::mutex mutex;
    std::vector<std::shared_ptr<Thread>> threads;
    std::vector<Parked> parked;
    std::vector<uint8_t> senderStack;
    ucontext_t senderContext{};
    bool stopped{}, scanned{};
    int signal{};
    struct sigaction previous{};
    Run push;
    Park locked;
    static NativeCollector *active;
    static thread_local Thread *local;
    static void onSignal(int);
    void park(Thread &);
    void resume(Thread &);
    void capture();
    void scan();
    void release();
};
}
