// Native Unity entry points backed by the original game's recompiled IL2CPP VM.
#include "runtime.h"
#include "bridge.h"
#include "elfload.h"
#include "libc.h"
#include "logging.h"
#include "posix_threads.h"
#include "posix_files.h"
#include "semaphore_host.h"
#include "gc_registers.h"
#include "module_registry_elf.h"
#include "signal_delivery.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

std::function<void(State &)> bindUnityMethod(const char *, void *);
void addLinuxGameTraces(recomp::Runtime &);

namespace {
#include "init.inc"
constexpr addr_t bias = 0x80000000;
constexpr addr_t returnAddress = 0x1000;

[[noreturn]] void fail(const std::string &message) {
    std::fprintf(stderr, "Linux GameAssembly: %s\n", message.c_str());
    std::abort();
}

struct GuestThread {
    size_t stackSize,guard;
    void *stack;
    static inline std::atomic<uint64_t> nextToken{1};
    const uint64_t token = nextToken.fetch_add(1);
    alignas(16) std::array<uintptr_t, 32> tls{};
    std::unique_ptr<bunker::windows::SignalDelivery> signalFrame;
    bunker::windows::SignalHost &signals;
    static std::mutex registryLock;
    static std::map<pthread_t,GuestThread *> registry;
    GuestThread(bunker::windows::SignalHost &host,const PosixThreadContext::Local &config) : signals(host) {
        const size_t page=sysconf(_SC_PAGESIZE);
        if (config.guardSize>SIZE_MAX-page || config.stackSize>SIZE_MAX-page) fail("guest stack size overflow");
        guard=(config.guardSize+page-1)&~(page-1);
        stackSize=(config.stackSize+page-1)&~(page-1);
        if (!stackSize || stackSize>SIZE_MAX-guard) fail("invalid guest stack size");
        stack=mmap(nullptr,guard+stackSize,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if (stack == MAP_FAILED || mprotect(static_cast<char *>(stack) + guard, stackSize, PROT_READ | PROT_WRITE))
            fail("could not allocate the guest stack");
        tls[0] = uintptr_t(tls.data());
        if (signals.RegisterThread(token) || signals.BindCurrent(token) ||
            (config.inheritedMask && signals.Mask(token,2,&*config.inheritedMask,nullptr)))
            fail("could not register guest thread signals");
        std::lock_guard lock(registryLock);
        registry.emplace(pthread_self(),this);
    }
    ~GuestThread() {
        signals.DropThread(token);
        std::lock_guard lock(registryLock);
        registry.erase(pthread_self());
        munmap(stack, guard + stackSize);
    }
    static bool query(pthread_t thread, addr_t &low, size_t &size) {
        std::lock_guard lock(registryLock);
        auto it=registry.find(thread);
        if (it==registry.end()) return false;
        low=uintptr_t(it->second->stack)+it->second->guard;
        size=it->second->stackSize;
        return true;
    }
    static uint64_t tokenFor(pthread_t thread) {
        std::lock_guard lock(registryLock);
        auto it=registry.find(thread);
        return it==registry.end() ? 0 : it->second->token;
    }
};
std::mutex GuestThread::registryLock;
std::map<pthread_t,GuestThread *> GuestThread::registry;

using Args = std::array<uint64_t, 6>;
using Callback = std::function<void(State &)>;

class Assembly {
    recomp::Runtime runtime;
    bunker::windows::LibC libc;
    bunker::windows::Logging logging;
    bunker::windows::SignalHost signals;
    bunker::windows::Semaphores semaphores{signals};
    bunker::windows::ModuleRegistry modules{[this](addr_t pc){ run(pc,{}); }};
    linux_gc::Jumps jumps{[] {
        addr_t base; size_t size;
        auto id=pthread_self();
        if (!GuestThread::query(id,base,size)) fail("guest stack unavailable for setjmp");
        // A saved frame has its own generation and is retired on return.
        auto token=GuestThread::tokenFor(id);
        return linux_gc::Jumps::Thread{token,token,base,base+size};
    },linux_gc::environment,[this] {
        uint64_t mask;
        if (signals.Mask(GuestThread::tokenFor(pthread_self()),0,nullptr,&mask)) fail("guest signal mask unavailable for setjmp");
        return mask;
    },[](addr_t pc){ return pc == 0x825b0580; }};
    PosixThreads threads{[this](addr_t pc,addr_t arg){ return run(pc,{arg}); },GuestThread::query,[this]{
        uint64_t mask;
        if (signals.Mask(GuestThread::tokenFor(pthread_self()),0,nullptr,&mask)) fail("guest thread mask unavailable");
        return mask;
    }};
    elf_image image{};
    std::string dataDirectory;
    alignas(8) std::array<char, 3 * 152> standardStreams{};
    std::map<std::string, addr_t> imports;
    struct EngineBinding { addr_t marker; void *native=nullptr; Callback call; };
    std::map<std::string,EngineBinding> engine;
    std::map<addr_t,EngineBinding *> byMarker;
    std::atomic<size_t> numBindings{0};
    std::atomic<void *> logCallback{};
    addr_t logMarker{};
    std::mutex engineLock;
    addr_t nextMarker = 0x7ff000000000;
    static thread_local Assembly *mapping;
    addr_t bind(Callback callback) {
        addr_t marker = nextMarker;
        nextMarker += 8;
        runtime.AddImport(marker, std::move(callback));
        return marker;
    }
    addr_t resolve(const std::string &name) {
        if (name == "__sF") return uintptr_t(standardStreams.data());
        if (name == "pthread_kill") {
            auto [slot,added]=imports.try_emplace(name);
            if (added) slot->second=bind(recomp::MakeBridge<int(pthread_t,int)>([this](auto target,int signal){
                auto token=GuestThread::tokenFor(target);
                return token ? signals.Queue(token,signal) : ESRCH;
            }));
            return slot->second;
        }
        try { return libc.Marker(name); } catch (const std::invalid_argument &) {}
        try { return logging.Marker(name); } catch (const std::invalid_argument &) {}
        try { return signals.Marker(name); } catch (const std::invalid_argument &) {}
        try { return semaphores.Marker(name); } catch (const std::invalid_argument &) {}
        try { return jumps.Marker(name); } catch (const std::invalid_argument &) {}
        try { return modules.Marker(name); } catch (const std::invalid_argument &) {}
        auto [entry, added] = imports.try_emplace(name);
        if (added) {
            auto callback = threads.lookup(name);
            if (!callback) callback = linux_files::lookup(name);
            if (!callback) callback = [name](State &state) {
                std::fprintf(stderr,"Linux GameAssembly: caller LR=%#llx\n",(unsigned long long)state.gpr.x30.qword);
                fail("unimplemented Android import " + name);
            };
            entry->second = bind(std::move(callback));
        }
        return entry->second;
    }
    addr_t engineCallback(const char *name, void *fn) {
        std::lock_guard lock(engineLock);
        auto it=engine.find(name);
        if (it==engine.end()) fail("unknown Linux Unity internal call " + std::string(name));
        auto &slot=it->second;
        if (slot.native != fn) {
            slot.call=fn ? bindUnityMethod(name,fn) : Callback{};
            if (!slot.native) ++numBindings;
            if (!fn) --numBindings;
            slot.native=fn;
        }
        return fn ? slot.marker : 0;
    }
public:
    Assembly() : libc([this](addr_t pc, const std::vector<addr_t> &args) {
        Args values{};
        if (args.size() > values.size()) fail("unexpected libc callback argument count");
        std::copy(args.begin(), args.end(), values.begin());
        return run(pc,values);
    }) {
        addLinuxGameTraces(runtime);
        static const char *const names[] = {
#include "engine-names.inc"
        };
        for (auto name:names) {
            auto &slot=engine[name];
            slot.marker=bind([this,&slot,name](State &s){
                Callback callback;
                {
                    std::lock_guard lock(engineLock);
                    callback=slot.call;
                }
                if (!callback) fail("unimplemented Unity internal call " + std::string(name));
                callback(s);
            });
            byMarker.emplace(slot.marker,&slot);
        }
        logMarker=bind([this](State &s) {
            auto fn=reinterpret_cast<void(*)(const char *)>(logCallback.load());
            if (!fn) fail("Unity logging callback is unregistered");
            fn(reinterpret_cast<const char *>(s.gpr.x0.qword));
        });
        linux_gc::jumps = &jumps;
        jumps.Install(runtime);
        recomp::AddGuestDirectEntry(0x825b0544,linux_gc::spill);
        recomp::PublishGuestOverrides();
        modules.SetProgramHeaderRunner([this](addr_t pc,addr_t info,uint64_t size,addr_t data){
            return int32_t(run(pc,{info,size,data}));
        });
        modules.Install(runtime);
        libc.SetErrorSink([](int err){linux_files::guestErrno=err;});
        logging.SetErrorSink([](int err){linux_files::guestErrno=err;});
        signals.SetErrorSink([](int err){linux_files::guestErrno=err;});
        semaphores.SetErrorSink([](int err){linux_files::guestErrno=err;});
        auto current=[] {return GuestThread::tokenFor(pthread_self());};
        auto delivery=[this](const bunker::windows::SignalHost::Delivery &d) -> std::optional<uint64_t> {
            auto *state=recomp::CurrentExecutionState();
            if (!state) fail("guest signal outside a translated call");
            auto &thread=*static_cast<GuestThread *>(threads.local().execution.get());
            bool stop=d.signal==30 && state->gpr.x30.qword==0x825505d8 && recomp::IsOnlyExecutionState(*state);
            bool restart=d.signal==24 && state->gpr.x30.qword==0x825b2c24;
            if (!stop && !restart) fail("unsupported guest signal frame at this collector boundary");
            if (!thread.signalFrame) {
                using Frame=bunker::windows::SignalDelivery;
                auto low=uintptr_t(thread.stack)+thread.guard;
                thread.signalFrame=std::make_unique<Frame>(runtime,[this](addr_t pc){return runtime.HasGuestTrace(pc);},
                    0x825aada0,Frame::Bounds{low,low+thread.stackSize},signals,thread.token);
            }
            return thread.signalFrame->Deliver(d);
        };
        threads.setSignalWait([this,current]() -> std::shared_ptr<bunker::windows::SignalWake> {
            auto *state=recomp::CurrentExecutionState();
            if (!state || !recomp::IsOnlyExecutionState(*state) || state->gpr.x30.qword!=0x825505d8) return {};
            return signals.Wake(current());
        },[this,current]{return signals.NeedsPoll(signals.Lookup(current()));},[this,delivery,current]{
            auto result=signals.DispatchOne(current(),delivery);
            if (result.error) fail("could not dispatch collector signal to sleeping guest");
        });
        signals.Install(runtime,current,delivery);
        semaphores.Install(runtime,current,delivery);
        libc.Install(runtime);
        logging.SetAllocator([this](uint64_t n) { return libc.Malloc(n); });
        logging.Install(runtime);
        Dl_info info{};
        if (!dladdr(reinterpret_cast<const void *>(addLinuxGameTraces),&info)) fail("could not locate GameAssembly.so");
        auto file = std::filesystem::path(info.dli_fname).parent_path() / "Reincarnation_Data/Native/libil2cpp.so";
        dataDirectory = (file.parent_path().parent_path() / "Managed").string();
        mapping = this;
        int err = elf_load_bionic(file.c_str(),bias,[](const char *name) {
            return reinterpret_cast<void *>(mapping->resolve(name));
        },&image);
        mapping = nullptr;
        if (err) fail("could not map " + file.string());
        bunker::windows::Module module{};
        module.name = "libil2cpp.so";
        module.address_image = bunker::windows::DescribeModuleElf(image.file,std::filesystem::file_size(file),bias,file.string());
        module.export_policy = bunker::windows::Module::ExportPolicy::BionicRawSymbols;
        for (const auto &symbol:module.address_image->symbols)
            if (symbol.lookup_eligible && symbol.section && !symbol.name.empty())
                module.exports.emplace(symbol.name,bias+symbol.value);
        auto initializers = reinterpret_cast<const addr_t *>(image.base + initOffset);
        module.initializers.assign(initializers,initializers+initCount);
        modules.Add(std::move(module));
        if (!modules.Open("libil2cpp.so",RTLD_NOW)) fail("could not initialize original IL2CPP module");
        std::fprintf(stderr,"Linux GameAssembly: initialized %zu game constructors\n",initCount);
    }
    uint64_t run(addr_t pc, const Args &args) {
        auto &local=threads.local();
        if (!local.execution) local.execution=std::make_shared<GuestThread>(signals,local);
        auto &thread=*static_cast<GuestThread *>(local.execution.get());
        State state{};
        auto previous = recomp::CurrentExecutionState();
        state.gpr.sp.qword = previous ? previous->gpr.sp.qword : uintptr_t(thread.stack) + thread.guard + thread.stackSize;
        state.sr.tpidr_el0.qword = uintptr_t(thread.tls.data());
        state.gpr.x30.qword = returnAddress;
        state.gpr.x0.qword=args[0]; state.gpr.x1.qword=args[1]; state.gpr.x2.qword=args[2];
        state.gpr.x3.qword=args[3]; state.gpr.x4.qword=args[4]; state.gpr.x5.qword=args[5];
        runtime.Run(state,pc);
        return state.gpr.x0.qword;
    }
    ~Assembly() { threads.retireCurrent(); }
    uint64_t call(uint32_t pc, Args args) {
        enum : uint32_t {
#define API(name, address) name = address,
#include "api.inc"
#undef API
        };
        if (pc == il2cpp_add_internal_call)
            args[1] = engineCallback(reinterpret_cast<const char *>(args[0]),reinterpret_cast<void *>(args[1]));
        if (pc == il2cpp_register_log_callback) {
            logCallback=reinterpret_cast<void *>(args[0]);
            args[0]=args[0] ? logMarker : 0;
        }
        if (pc == il2cpp_resolve_icall) {
            auto marker=run(pc,args);
            if (!marker) return 0;
            auto it=byMarker.find(marker);
            if (it==byMarker.end()) fail("cannot expose an unregistered guest internal call to Unity");
            std::lock_guard lock(engineLock);
            return uintptr_t(it->second->native);
        }
        if (pc == il2cpp_set_find_plugin_callback) args[0] = 0; // Guest P/Invokes use Android module names.
        if (pc == il2cpp_set_data_dir) args[0] = uintptr_t(dataDirectory.c_str());
        if (pc == il2cpp_init || pc == il2cpp_init_utf16) {
            std::fprintf(stderr,"Linux GameAssembly: entering original il2cpp_init with %zu Unity bindings\n",numBindings.load());
            auto result=run(pc,args);
            std::fprintf(stderr,"Linux GameAssembly: original il2cpp_init returned %#llx\n",(unsigned long long)result);
            return result;
        }
        return run(pc,args);
    }
};
thread_local Assembly *Assembly::mapping = nullptr;
}

extern "C" uint64_t linux_il2cpp_invoke(uint32_t pc, const uint64_t *registers) {
    static Assembly assembly;
    Args args{};
    std::copy_n(registers,args.size(),args.begin());
    return assembly.call(pc,args);
}
