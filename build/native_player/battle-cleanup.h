// The original Big Hunt loader releases the outgoing wave's bundles and
// objects, then fire-and-forgets a global unused-asset scan. Unity scans
// the entire managed heap on the player thread for each such request.
namespace battle_cleanup {
extern "C" Memory *sub_82800f78(State &,addr_t,Memory *);
extern "C" Memory *sub_832d2738(State &,addr_t,Memory *);
extern "C" Memory *sub_832d2c8c(State &,addr_t,Memory *);

constexpr addr_t cleanAsync = 0x82800f78;
constexpr addr_t forgetTask = 0x8410cf30;
constexpr addr_t afterWaveDispose = 0x832d29d8;
constexpr addr_t afterLoadingDispose = 0x830700dc;
static bool inBigHunt;
static unsigned deferredScans;
static ULONGLONG exitDeadline;

static void trace(const char *event) {
    static const bool enabled = std::getenv("REINCOMP_TRACE_BATTLE_CLEANUP") != nullptr;
    if (enabled) {
        if (auto log = std::fopen("battle-cleanup.log", "a")) {
            std::fprintf(log, "%llu %s %u\n", GetTickCount64(), event, deferredScans);
            std::fclose(log);
        }
    }
}

static void flush() {
    trace("flush");
    exitDeadline = 0;
    deferredScans = 0;
    const auto task = threads->Run(cleanAsync, {0, 0});
    threads->Run(forgetTask, {task, 0});
}

extern "C" __declspec(dllexport) void ServiceBattleCleanup() {
    if (GetCurrentThreadId() == owner && exitDeadline && GetTickCount64() >= exitDeadline) flush();
}

static Memory *clean(State &state, addr_t pc, Memory *memory) {
    if (GetCurrentThreadId() != owner)
        return sub_82800f78(state, pc, memory);

    const addr_t caller = state.gpr.x30.qword;
    if (inBigHunt && deferredScans < 3 && (caller == afterWaveDispose || caller == afterLoadingDispose)) {
        MEMORYSTATUSEX status{sizeof(status)};
        constexpr uint64_t reserve = 4ull << 30;
        if (GlobalMemoryStatusEx(&status) && status.ullAvailPhys > reserve && status.ullAvailPageFile > reserve) {
            ++deferredScans;
            trace("defer");
            // Both callers discard this UniTask. A null awaiter encodes CompletedTask.
            state.gpr.x0.qword = 0;
            state.gpr.pc.qword = caller;
            return memory;
        }
    }
    if (deferredScans) trace("collected-by-game");
    exitDeadline = 0;
    deferredScans = 0;
    return sub_82800f78(state, pc, memory);
}

template<recomp::Entry dispose>
static Memory *finish(State &state, addr_t pc, Memory *memory) {
    if (GetCurrentThreadId() != owner)
        return dispose(state, pc, memory);

    const auto caller = state.gpr.x30.qword;
    const auto stack = state.gpr.sp.qword;
    inBigHunt = false;
    auto result = dispose(state, pc, memory);
    if (deferredScans && !exitDeadline && state.gpr.pc.qword == caller && state.gpr.sp.qword == stack) {
        // Returning to quests requests a scan as part of scene teardown.
        // Let that scan collect the outgoing waves. Other exit paths get a
        // bounded fallback from the player's main-thread maintenance tick.
        trace("exit");
        exitDeadline = GetTickCount64() + 1000;
    }
    return result;
}

static void install() {
    // BigHunt.OnInitialize, pre-initialize asset load, and next-wave asset load.
    for (addr_t pc : {0x832d2becull, 0x832d2688ull, 0x832d25c8ull})
        recomp::AddGuestProbe(pc, [](State &) {
            if (GetCurrentThreadId() != owner) return;
            if (exitDeadline) flush();
            inBigHunt = true;
        });
    recomp::AddGuestDirectEntry(cleanAsync, clean);
    recomp::AddGuestDirectEntry(0x832d2738, finish<sub_832d2738>); // TerminateBattle
    recomp::AddGuestDirectEntry(0x832d2c8c, finish<sub_832d2c8c>); // OnDispose
}
}
