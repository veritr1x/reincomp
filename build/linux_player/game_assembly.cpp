// Native Unity entry points backed by the original game's recompiled IL2CPP VM.
#include <cfenv> // glibc declares its exception flags before Remill adds ARM flag macros.
#include "absent_sdk_overrides.h"
#include "host_time_zone.h"
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
#include "liveness.h"
#include "asset_bundles.h"
#include "desktop_input.h"
#include "fastaes_native.h"
#include "firebase_app.h"
#include "server_profile.h"
#include "grpc_native_service.h"
#include "native_gc.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

std::function<void(State &)> bindUnityMethod(const char *, void *, unsigned assetPort,const std::filesystem::path &);
void installLinuxOctoDownloads(const unsigned &portOffset);
extern "C" void *il2cpp_string_new(const char *);
std::unique_ptr<bunker::windows::FirebaseAppService> linuxFirebase(
    const std::filesystem::path &, bunker::windows::FirebaseAppService::Runner,
    std::function<bool()>, std::function<bool(addr_t)>);
extern "C" const recomp::Trace linux_game_traces[];
extern "C" const size_t linux_game_trace_count;
static_assert(sizeof(recomp::Trace) == 16 && offsetof(recomp::Trace, entry) == 8);

namespace {
#include "init.inc"
constexpr addr_t bias = 0x80000000;

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
    linux_gc::NativeCollector &collector;
    std::shared_ptr<linux_gc::NativeCollector::Thread> roots;
    static std::mutex registryLock;
    static std::map<pthread_t,GuestThread *> registry;
    GuestThread(bunker::windows::SignalHost &host,linux_gc::NativeCollector &gc,const PosixThreadContext::Local &config)
        : signals(host),collector(gc) {
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
        roots=collector.attach(uintptr_t(stack)+guard,stackSize);
        std::lock_guard lock(registryLock);
        registry.emplace(pthread_self(),this);
    }
    ~GuestThread() {
        collector.retire(roots);
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
    linux_gc::NativeCollector collector{[this](addr_t pc,addr_t low,addr_t high){run(pc,{low,high,0});},
        [this](const std::function<void()> &park){
            recomp::WithGuestMemoryRead([&]{
                auto signalLock=signals.LockForThreadSuspension();
                std::lock_guard threadLock(GuestThread::registryLock);
                threads.withRegistryLocks([&] {
                    flockfile(stderr);
                    park();
                    funlockfile(stderr);
                });
            });
        }};
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
    std::atomic<bool> firstCollectorCycle{true};
    LinuxLiveness liveness;
    LinuxAssetBundles bundles;
    LinuxDesktopInput input;
    unsigned bunkerOffset=0;
    std::filesystem::path streamingAssets;
    std::unique_ptr<bunker::windows::FirebaseAppService> firebase;
    bunker::windows::fastaes_native::Service fastAes;
    std::unique_ptr<bunker::windows::grpc_native::Service> grpc;
    std::atomic<void *> logCallback{};
    addr_t logMarker{};
    std::mutex engineLock;
    const bool traceCalls=std::getenv("REINCOMP_NATIVE_TRACE_CALLS")!=nullptr;
    std::mutex traceLock;
    std::set<addr_t> seenMethods;
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
            slot.call=bundles.bind(name,fn,[this](const char *key){
                std::lock_guard lock(engineLock);
                return engine.at(key).native;
            });
            if (!slot.call) slot.call=input.bind(name,fn);
            if (!slot.call && fn) slot.call=bindUnityMethod(name,fn,8080+bunkerOffset,streamingAssets);
            if (!slot.native) ++numBindings;
            if (!fn) --numBindings;
            slot.native=fn;
        }
        return fn ? slot.marker : 0;
    }
    uint64_t runCallback(addr_t pc,const std::vector<addr_t> &args) {
        Args values{};
        if (args.size() > values.size()) fail("unexpected guest callback argument count");
        std::copy(args.begin(), args.end(), values.begin());
        return run(pc,values);
    }
public:
    Assembly() : libc([this](addr_t pc,const std::vector<addr_t> &args){return runCallback(pc,args);}) {
        runtime.AddTraces(linux_game_traces, linux_game_trace_count);
        liveness.install([this](Callback callback){ return bind(std::move(callback)); });
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
        collector.install();
        bunker::windows::server_profile::InstallEntryOverrides();
        bunker::windows::absent_sdks::Call()=[this](addr_t pc,const std::vector<addr_t> &args){return runCallback(pc,args);};
        bunker::windows::absent_sdks::AddOverrides(bunker::windows::absent_sdks::DeviceReport::Unavailable);
        installLinuxOctoDownloads(bunkerOffset);
        // The original locale code obtains its IANA identifier via java.util.TimeZone.
        recomp::AddGuestOverride(0x853f4d84, [](State &s){
            s.gpr.x0.qword = reinterpret_cast<addr_t>(il2cpp_string_new(bunker::windows::HostTimeZoneId().c_str()));
        });
        // Octo's APK selects app storage through UnityPlayer.currentActivity.
        // Linux has one app-owned files directory and no Android external volume.
        // Keep Octo's original cache naming, database setup and migration logic.
        recomp::AddGuestRedirect(0x84c33604,[](State &s)->addr_t {
            s.gpr.x0.qword=0;
            return 0x8491094c; // UnityEngine.Application.get_persistentDataPath
        });
        recomp::AddGuestOverride(0x84c33910,[](State &s){s.gpr.x0.qword=0;}); // GetExternalFilesDir
        recomp::AddGuestOverride(0x84c36d40,[](State &s){s.gpr.x0.qword=0;}); // IsExternalStorageWritable
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
            if (!stop && !restart) {
                std::fprintf(stderr,"Linux GameAssembly: signal %d at guest LR=%#llx\n",d.signal,
                             (unsigned long long)state->gpr.x30.qword);
                fail("unsupported guest signal frame at this collector boundary");
            }
            if (!thread.signalFrame) {
                using Frame=bunker::windows::SignalDelivery;
                auto low=uintptr_t(thread.stack)+thread.guard;
                thread.signalFrame=std::make_unique<Frame>(runtime,[this](addr_t pc){return runtime.HasGuestTrace(pc);},
                    0x825aada0,Frame::Bounds{low,low+thread.stackSize},signals,thread.token);
            }
            auto restoredMask=thread.signalFrame->Deliver(d);
            if (stop && firstCollectorCycle.exchange(false))
                std::fputs("Linux GameAssembly: original finalizer returned from collector stop and restart\n",stderr);
            return restoredMask;
        };
        threads.setSignalWait([this,current]() -> std::shared_ptr<bunker::windows::SignalWake> {
            auto *state=recomp::CurrentExecutionState();
            auto &local=threads.local();
            if (!local.guestCreated || !state || !recomp::IsOnlyExecutionState(*state) || state->gpr.x30.qword!=0x825505d8) return {};
            addr_t event{};
            recomp::ReadGuestMemory(&event,0x879a7a40,sizeof(event));
            if (!event || state->gpr.x0.qword!=event+0x2c || state->gpr.x1.qword!=event+4) return {};
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
        if (!dladdr(linux_game_traces,&info)) fail("could not locate GameAssembly.so");
        auto player = std::filesystem::absolute(info.dli_fname).parent_path();
        auto file = player / "Reincarnation_Data/Native/libil2cpp.so";
        streamingAssets = player / "Reincarnation_Data/StreamingAssets";
        if (const char *value=std::getenv("REINCOMP_BUNKER_PORT_OFFSET")) {
            auto end=value+std::strlen(value);
            auto result=std::from_chars(value,end,bunkerOffset);
            if (result.ec!=std::errc{} || result.ptr!=end || bunkerOffset>65535-8080)
                fail("invalid Bunker port offset");
        }
        auto firebaseLibrary = file.parent_path() / "FirebaseCppApp-9_6_0.so";
        if (std::filesystem::is_regular_file(firebaseLibrary)) {
            firebase = linuxFirebase(firebaseLibrary,[this](addr_t entry,const std::vector<addr_t> &args) {
                return runCallback(entry,args);
            },[this] { return recomp::CurrentExecutionState() && GuestThread::tokenFor(pthread_self()) &&
                modules.ReferenceCount("libil2cpp.so"); },[this](addr_t pc) { return runtime.HasGuestTrace(pc); });
            firebase->Install(runtime);
            modules.Add(firebase->MakeModule());
        }
        fastAes.Install(runtime);
        modules.Add(fastAes.MakeModule());
        auto grpcLibrary = file.parent_path() / "libgrpc_csharp_ext.x64.so";
        if (std::filesystem::is_regular_file(grpcLibrary)) {
            bunker::windows::grpc_native::Service::Config config;
            config.library_path = grpcLibrary.string();
            config.target_rewrites["127.0.0.1:443"] = "127.0.0.1:" + std::to_string(8003 + bunkerOffset);
            grpc = std::make_unique<bunker::windows::grpc_native::Service>(std::move(config));
            grpc->Install(runtime);
            modules.Add(grpc->MakeModule());
        }
        bundles.setWorker(player / "tools/bundle_worker.py");
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
        if (!local.execution) local.execution=std::make_shared<GuestThread>(signals,collector,local);
        auto &thread=*static_cast<GuestThread *>(local.execution.get());
        State state{};
        auto previous = recomp::CurrentExecutionState();
        if (previous) state = *previous;
        else state.gpr.sp.qword = uintptr_t(thread.stack) + thread.guard + thread.stackSize;
        state.sr.tpidr_el0.qword = uintptr_t(thread.tls.data());
        state.gpr.x30.qword = bunker::windows::GuestThreadReturnAddress();
        state.gpr.x0.qword=args[0]; state.gpr.x1.qword=args[1]; state.gpr.x2.qword=args[2];
        state.gpr.x3.qword=args[3]; state.gpr.x4.qword=args[4]; state.gpr.x5.qword=args[5];
        struct Invocation {
            std::atomic<unsigned> &depth;
            explicit Invocation(std::atomic<unsigned> &value):depth(value){++depth;}
            ~Invocation(){--depth;}
        } invocation(thread.roots->depth);
        runtime.Run(state,pc);
        return state.gpr.x0.qword;
    }
    ~Assembly() {
        if (firebase) firebase->CloseAfterManagedShutdown();
        threads.retireCurrent();
    }
    uint64_t call(uint32_t pc, Args args) {
        enum : uint32_t {
#define API(name, address) name = address,
#include "api.inc"
#undef API
        };
        if (traceCalls && pc==il2cpp_runtime_invoke && args[0]) {
            bool first;
            {
                std::lock_guard lock(traceLock);
                first=seenMethods.insert(args[0]).second;
            }
            if (first) {
                auto method=reinterpret_cast<const char *>(run(il2cpp_method_get_name,{args[0]}));
                auto klass=run(il2cpp_method_get_class,{args[0]});
                auto type=reinterpret_cast<const char *>(run(il2cpp_class_get_name,{klass}));
                auto ns=reinterpret_cast<const char *>(run(il2cpp_class_get_namespace,{klass}));
                std::fprintf(stderr,"Linux managed invocation: %s.%s::%s\n",ns,type,method);
            }
        }
        if (pc == il2cpp_add_internal_call)
            args[1] = engineCallback(reinterpret_cast<const char *>(args[0]),reinterpret_cast<void *>(args[1]));
        if (pc == il2cpp_register_log_callback) {
            logCallback=reinterpret_cast<void *>(args[0]);
            args[0]=args[0] ? logMarker : 0;
        }
        if (pc == il2cpp_unity_liveness_calculation_begin)
            return liveness.begin(args,[&](const Args &mapped){ return run(pc,mapped); });
        if (pc == il2cpp_unity_liveness_calculation_from_root || pc == il2cpp_unity_liveness_calculation_from_statics ||
            pc == il2cpp_unity_liveness_calculation_end) {
            auto state=pc == il2cpp_unity_liveness_calculation_from_root ? args[1] : args[0];
            return liveness.visit(state,pc==il2cpp_unity_liveness_calculation_end,[&]{return run(pc,args);});
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
