// Compile with MinGW g++ -std=c++17 battle-cleanup.cpp -o battle-cleanup.exe
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <vector>

using addr_t = uintptr_t;
using ULONGLONG = unsigned long long;
struct Register { addr_t qword = 0; };
struct State { struct { Register x0, x30, sp, pc; } gpr; };
struct Memory {};
struct MEMORYSTATUSEX { unsigned length; uint64_t ullAvailPhys, ullAvailPageFile; };
static unsigned owner = 1, current = owner;
static ULONGLONG clockMs = 1000;
static uint64_t physical = 8ull << 30, commit = physical;
static unsigned GetCurrentThreadId() { return current; }
static ULONGLONG GetTickCount64() { return clockMs; }
static bool GlobalMemoryStatusEx(MEMORYSTATUSEX *s) {
    s->ullAvailPhys = physical;
    s->ullAvailPageFile = commit;
    return true;
}
namespace recomp {
using Entry = Memory *(*)(State &, addr_t, Memory *);
static std::map<addr_t, void(*)(State &)> probes;
static std::map<addr_t, Entry> entries;
static void AddGuestProbe(addr_t pc, void(*probe)(State &)) { probes[pc] = probe; }
static void AddGuestDirectEntry(addr_t pc, Entry fn) { entries[pc] = fn; }
}
struct Calls {
    std::vector<addr_t> log;
    addr_t Run(addr_t fn, std::initializer_list<addr_t>) { log.push_back(fn); return 0; }
} calls;
static auto *threads = &calls;
#include "../battle-cleanup.h"

static int collections;
namespace battle_cleanup {
extern "C" Memory *sub_82800f78(State &, addr_t, Memory *m) { ++collections; return m; }
extern "C" Memory *sub_832d2738(State &s, addr_t, Memory *m) { s.gpr.pc = s.gpr.x30; return m; }
extern "C" Memory *sub_832d2c8c(State &s, addr_t pc, Memory *m) { return sub_832d2738(s, pc, m); }
}

int main() {
    using namespace battle_cleanup;
    install();
    State s;
    auto request = [&](addr_t caller) {
        s.gpr.x30.qword = caller;
        recomp::entries.at(cleanAsync)(s, cleanAsync, nullptr);
    };
    auto enter = [&] { recomp::probes.at(0x832d2bec)(s); };
    auto leave = [&] { recomp::entries.at(0x832d2738)(s, 0x832d2738, nullptr); };
    request(afterWaveDispose);
    assert(collections == 1);
    enter();
    physical = 3ull << 30;
    request(afterWaveDispose);
    physical = 8ull << 30;
    commit = 3ull << 30;
    request(afterLoadingDispose);
    commit = physical;
    current = 2;
    request(afterWaveDispose);
    current = owner;
    assert(collections == 4 && deferredScans == 0);

    request(afterLoadingDispose);
    request(afterWaveDispose);
    request(afterWaveDispose);
    assert(collections == 4 && deferredScans == 3 && s.gpr.pc.qword == afterWaveDispose);
    request(afterWaveDispose);
    assert(collections == 5 && deferredScans == 0);
    request(afterWaveDispose);
    leave();
    request(0x828ab2c4);  // The game's regular scene-release scan consumes the pending work.
    clockMs += 2000;
    ServiceBattleCleanup();
    assert(collections == 6 && calls.log.empty());

    enter();
    request(afterLoadingDispose);
    leave();
    ServiceBattleCleanup();
    assert(calls.log.empty());
    clockMs += 1000;
    ServiceBattleCleanup();
    ServiceBattleCleanup();
    assert((calls.log == std::vector<addr_t>{cleanAsync, forgetTask}));

    enter();
    request(afterWaveDispose);
    leave();
    enter();  // Starting another match before the deadline flushes the last one.
    assert(calls.log.size() == 4 && deferredScans == 0 && !exitDeadline);
}
