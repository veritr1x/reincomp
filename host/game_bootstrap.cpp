// Winsock must precede <windows.h>.
#include "octo_http.h"
#include "avpro_video.h"
#include "opensles_audio.h"
#include "fmod_audio_device.h"
// Natural native-frame diagnostic with lazy real Unity and IL2CPP modules.
#include "jni_host.h"
#include "../runtime/fast_tls.h"
#include <atomic>
#include <cstring>
#include "android_context.h"
#include "android_build_manufacturer.h"
#include "windows_display_service.h"
#include "android_internal_storage.h"
#include "android_obb_storage.h"
#include "android_asset_pack_wrapper.h"
#include "android_shared_preferences.h"
#include "windows_shared_preferences.h"
#include "windows_android_id.h"
#include "jni_interface_proxy.h"
#include "reflection_launch_epoch.h"
#include "reflection_helper.h"
#include "firebase_app.h"
#include "java_native_libraries.h"
#include "java_native_loader.h"
#include "unity_network_proxy.h"
#include "java_tls_trust.h"
#include "android_choreographer.h"
#include "unicode.h"
#include "apk_assets.h"
#include "bunker_view.h"
#include "native_window.h"
#include "unity_touch_input.h"
#include "game_frame_rate.h"
#include "component_type_query.h"
#include "scene_route.h"
#include "keyboard_confirm.h"
#include "android_cpu_topology.h"
#include "keyboard_touch.h"
#include "unity_lifecycle.h"
#include "unity_soft_input.h"
#include "signal_host.h"
#include "signal_sleep.h"
#include "signal_nanosleep.h"
#include "signal_futex.h"
#include "thread_priority.h"
#include "android_process.h"
#include "thread_affinity.h"
#include "semaphore_host.h"
#include "sensor_service.h"
#include "nonlocal_jmp.h"
#include "audio_volume_session.h"
#include "egl_host.h"
#include "gl_host.h"
#include "gles_service_module.h"
#include "filesystem.h"
#include "socket_transport.h"
#include "module_registry.h"
#include "module_registry_elf.h"
#include "pthread_host.h"
#include "libc.h"
#include "logging.h"
#include "server_profile.h"
#include "bunker_ports.h"
#include "lift_gap_overrides.h"
#include "guest_probes.h"
#include "absent_sdk_overrides.h"
#include "grpc_native_service.h"
#include "fastaes_native.h"
#include "zlib_host.h"
#include "../runtime/elfload.h"
#include "../runtime/bridge.h"
#include "../runtime/guest_memory.h"
#include "game_exception_diagnostic.h"
#include "game_traces.h"
#include "game_metadata.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cerrno>
#include <cfenv>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <csignal>
#include <vector>
#include <windows.h>
#include <pthread.h>
#include <bcrypt.h>
#ifdef REINCOMP_EMBEDDED_PLAYER
extern "C" void RegisterPlayerJni(bunker::jni::Host *);
#endif
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
#if !defined(BUNKER_GAME_COOPERATIVE_POLLS) || !BUNKER_GAME_COOPERATIVE_POLLS
#error Original collector delivery requires the qualified cooperative cohort
#endif
#include "worker_signal_lifecycle.h"
#include "signal_delivery.h"
#include "../runtime/cooperative_poll.h"
#include <condition_variable>
#endif
extern "C" Memory *__real_sub_825b0544(State &,addr_t,Memory *);
extern "C" Memory *__real_sub_708d905c(State &,addr_t,Memory *);
extern "C" Memory *__real_sub_70a8a5c4(State &,addr_t,Memory *);
extern "C" Memory *__real_sub_70a8aa08(State &,addr_t,Memory *);
extern "C" Memory *sub_7042cab4(State &,addr_t,Memory *);  // Unity core topology (lifted)

#ifdef REINCOMP_EMBEDDED_PLAYER
extern "C" bool NativePlayerSenderRoots(State *const *,size_t);
#endif
namespace {
using namespace unity_image;
// Claim the translated images before Proton's heaps grow into their fixed
// address ranges. Physical pages are committed only after verifying the APK.
void *const unityReservation=VirtualAlloc(reinterpret_cast<void *>(kUnityBase),kUnityImageSpan,MEM_RESERVE,PAGE_NOACCESS);
void *const il2cppReservation=VirtualAlloc(reinterpret_cast<void *>(il2cpp_image::kIl2cppBase),il2cpp_image::kIl2cppSpan,MEM_RESERVE,PAGE_NOACCESS);
const auto &kInitializers=unity_image::kConstructors;
std::map<std::string,addr_t> resolved;
size_t constructor_index;
const char *stage="setup";
std::atomic<bool> jni_diagnostic_reached{false};
std::atomic<bool> init_java_diagnostic_active{false};
std::atomic<size_t> init_java_errors{0};
struct GameChoreographerParent {uint64_t token=0,mask=0;};
struct GameChoreographerWorker {uint64_t token=0;const void *parent=nullptr;bool registered=false;};
struct GameUnityLooperDispatcher {std::mutex mutex;bunker::windows::UnitySurfaceLifecycle *owner=nullptr;};
thread_local GameChoreographerWorker game_choreographer_worker;
std::atomic<DWORD> fourth_frame_observation_thread{0};
std::atomic<uint64_t> fourth_frame_observation_token{0};
bool ObserveFourthFrameThread(){return fourth_frame_observation_thread.load(std::memory_order_relaxed)==GetCurrentThreadId();}
struct FourthFrameNativeEnvironment {
  DWORD last_error=GetLastError();int saved_errno=errno;fenv_t floating{};
  FourthFrameNativeEnvironment(){std::fegetenv(&floating);}
  ~FourthFrameNativeEnvironment(){std::fesetenv(&floating);errno=saved_errno;SetLastError(last_error);}
};
void ObserveFourthFrameJni(const char *phase,bool vm,size_t slot,uint64_t sequence,
                          addr_t pc,addr_t lr,addr_t sp,const std::array<addr_t,4> &arguments,addr_t result){
  FourthFrameNativeEnvironment environment;
  std::fprintf(stderr,"Fourth-frame JNI: phase=%s native_tid=%lu sequence=%llu kind=%s slot=%zu PC=%#llx LR=%#llx SP=%#llx x1=%#llx x2=%#llx x3=%#llx x4=%#llx x0=%#llx; register observation only\n",
    phase,GetCurrentThreadId(),(unsigned long long)sequence,vm?"VM":"Env",slot,
    (unsigned long long)pc,(unsigned long long)lr,(unsigned long long)sp,
    (unsigned long long)arguments[0],(unsigned long long)arguments[1],
    (unsigned long long)arguments[2],(unsigned long long)arguments[3],(unsigned long long)result);
}
void ObserveFourthFrameWait(const char *phase,const char *name,uint64_t sequence,const State &state){
  FourthFrameNativeEnvironment environment;
  std::fprintf(stderr,"Fourth-frame wait: phase=%s native_tid=%lu registered_token=%#llx sequence=%llu import=%s PC=%#llx LR=%#llx SP=%#llx FP=%#llx x0=%#llx x1=%#llx x2=%#llx x3=%#llx; register observation, not a blocked-state proof\n",
    phase,GetCurrentThreadId(),(unsigned long long)fourth_frame_observation_token.load(std::memory_order_relaxed),
    (unsigned long long)sequence,name,(unsigned long long)state.gpr.pc.qword,
    (unsigned long long)state.gpr.x30.qword,(unsigned long long)state.gpr.sp.qword,
    (unsigned long long)state.gpr.x29.qword,(unsigned long long)state.gpr.x0.qword,
    (unsigned long long)state.gpr.x1.qword,(unsigned long long)state.gpr.x2.qword,(unsigned long long)state.gpr.x3.qword);
}
void Require(bool condition,const char *message){if(!condition)throw std::runtime_error(message);}

// BEGIN_GAME_FRAME_RUN_POLICY
struct GameFrameRunOptions {
  enum class Mode {FirstWitness,ReturnedFrames,UntilClose};
  Mode mode=Mode::FirstWitness;uint64_t returned_frames=0;
  bool Continuing()const noexcept{return mode!=Mode::FirstWitness;}
  bool ReachedLimit(uint64_t completed)const noexcept{return mode==Mode::ReturnedFrames&&completed>=returned_frames;}
  bool StopAfterReturnedFrame(uint64_t completed,bool witness)const noexcept{
    return mode==Mode::FirstWitness?witness:ReachedLimit(completed);
  }
  bool SnapshotBeforeNextFrame(uint64_t completed)const noexcept{
    return mode==Mode::ReturnedFrames&&returned_frames>1&&completed==returned_frames-1;
  }
};
GameFrameRunOptions ParseGameFrameRunTail(int &argc,char **argv){
  GameFrameRunOptions options;
  if(argc>1&&!std::strcmp(argv[argc-1],"--run-frames"))throw std::invalid_argument("--run-frames requires a positive returned-frame count");
  if(argc>2&&!std::strcmp(argv[argc-2],"--run-frames")){
    const char *first=argv[argc-1],*last=first+std::strlen(first);uint64_t count=0;
    const auto parsed=std::from_chars(first,last,count);
    if(first==last||parsed.ec!=std::errc{}||parsed.ptr!=last||!count)throw std::invalid_argument("--run-frames requires a positive uint64 decimal count");
    options.mode=GameFrameRunOptions::Mode::ReturnedFrames;options.returned_frames=count;argc-=2;
  }else if(argc>1&&!std::strcmp(argv[argc-1],"--run-until-close")){
    options.mode=GameFrameRunOptions::Mode::UntilClose;--argc;
  }
  return options;
}
// ~1ms sleep without changing the process timer resolution (the default
// 15.6ms tick otherwise adds up to a full tick of latency per frame poll).
void FramePollSleep(std::chrono::nanoseconds delay=std::chrono::milliseconds(1)){
  if(delay.count()<=0)return;
  static thread_local HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,0x00000002/*HIGH_RESOLUTION*/,TIMER_ALL_ACCESS);
  LARGE_INTEGER due;due.QuadPart=-((delay.count()+99)/100); // relative, in 100 ns units
  if(timer&&SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE)){WaitForSingleObject(timer,INFINITE);return;}
  std::this_thread::sleep_for(delay);
}
// --target-fps N: Unity's Android frame pacing (libunity 22be08) waits
// round(refresh/target) Choreographer vsyncs after each frame, where target is
// Application.targetFrameRate (libunity .data 1241af8) or 30 when the game
// leaves it unset (<= 0). Whole-vsync waits turn a 13 ms frame into 16.7 ms
// (60 fps at 120 Hz), so like a PC port's frame-rate option this sets that
// value high enough that Unity never waits and the host caps frame starts at
// N per second instead (GameFrameLimiter). The original pacing code is unchanged.
// Default "game" (0): keep the game's own Application.targetFrameRate and
// Unity's pacing. Measured on the 4K title screen, an unpaced target (1000)
// made Unity present only one of every ~32 nativeRender calls (0.5 fps), so
// the override is opt-in (-1 = display refresh) until that is understood.
int32_t &GameTargetFps(){static int32_t fps=0;return fps;}
int32_t DisplayRefreshFps(){
  DEVMODEW mode{};mode.dmSize=sizeof(mode);
  if(EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&mode)&&mode.dmDisplayFrequency>1&&mode.dmDisplayFrequency<=1000)return int32_t(mode.dmDisplayFrequency);
  return 60;
}
// --server-profile: connect the game to the local Lunar Tear backend (see
// server_profile.h for the client changes this applies).
bool &GameServerProfile(){static bool enabled=false;return enabled;}
// --sound: the game's audio plays (opensles_audio.h) and is not muted after resume.
bool &GameSound(){static bool enabled=false;return enabled;}
// --grpc-native PATH: official Grpc.Core 2.26.0 host native extension.
std::string &GameGrpcNative(){static std::string path;return path;}
constexpr addr_t kUnityTargetFrameRate=0x71241af8;
int32_t GameFrameCap(){static const int32_t cap=GameTargetFps()<0?DisplayRefreshFps():GameTargetFps();return cap;}
void ApplyGameTargetFps(){
  const int32_t cap=GameFrameCap();if(!cap)return;
  // Application.targetFrameRate value written (BUNKER_TARGET_WRITE overrides
  // for pacing tests; default: the cap itself).
  static const int32_t kUnpaced=[cap]{const char *v=std::getenv("BUNKER_TARGET_WRITE");return v&&*v?int32_t(std::atoi(v)):cap;}();
  int32_t current=0;recomp::ReadGuestMemory(&current,kUnityTargetFrameRate,sizeof(current));
  static bool reported=false;
  if(!reported){reported=true;std::fprintf(stderr,"Game bootstrap: --target-fps %d host frame cap; original Application.targetFrameRate=%d replaced by %d so Unity skips its whole-vsync wait\n",cap,current,kUnpaced);}
  if(current!=kUnpaced)recomp::WriteGuestMemory(kUnityTargetFrameRate,&kUnpaced,sizeof(kUnpaced));
  bunker::windows::frame_rate::ApplyData(kUnpaced);
}
// Host frame cap: frame starts are spaced 1/cap apart on a running deadline, so
// a late poll wakeup is absorbed by the next frame instead of lowering the rate.
// A frame that overruns restarts the schedule (no burst of catch-up frames).
class GameFrameLimiter {
 public:
  explicit GameFrameLimiter(int32_t cap):interval_(cap>0?std::chrono::nanoseconds(1000000000/cap):std::chrono::nanoseconds(0)){}
  bool Ready(std::chrono::steady_clock::time_point now) const {return interval_.count()==0||now>=next_;}
  std::chrono::nanoseconds PollDelay() const {
    const auto remaining=std::chrono::duration_cast<std::chrono::nanoseconds>(next_-std::chrono::steady_clock::now());
    return std::min(remaining,std::chrono::nanoseconds(1000000));
  }
  void WaitForSwap(){
    auto now=std::chrono::steady_clock::now();
    if(now<next_){
      constexpr auto margin=std::chrono::microseconds(160);
      FramePollSleep(next_-now-margin);
      while((now=std::chrono::steady_clock::now())<next_)YieldProcessor();
    }
    Started(now);
  }
  void Started(std::chrono::steady_clock::time_point now){
    if(interval_.count()==0)return;
    next_=now-next_>interval_?now+interval_:next_+interval_;
  }
 private:
  std::chrono::nanoseconds interval_;std::chrono::steady_clock::time_point next_{};
};
void RequireGameFrameRunProfile(const GameFrameRunOptions &options,bool first_frame,bool collector_delivery){
  if(options.Continuing()&&(!first_frame||!collector_delivery))throw std::invalid_argument("frame continuation requires the explicit --first-frame muted collector profile");
}
// END_GAME_FRAME_RUN_POLICY
template<class T>T Read(addr_t address){T result{};recomp::ReadGuestMemory(&result,address,sizeof(result));return result;}
// Opt-in (BUNKER_WAIT_PROFILE=1) wall-time profile of blocking guest waits,
// keyed by import, native thread and the guest frame-record chain. Every 5 s
// the top entries are printed and reset. Observation only.
bool WaitProfileEnabled(){static const bool enabled=std::getenv("BUNKER_WAIT_PROFILE")!=nullptr;return enabled;}
struct WaitProfileEntry {uint64_t nanos=0,calls=0;};
std::mutex &WaitProfileMutex(){static std::mutex m;return m;}
std::map<std::string,WaitProfileEntry> &WaitProfileTotals(){static std::map<std::string,WaitProfileEntry> t;return t;}
std::string WaitProfileKey(const std::string &identity,const State &s){
  auto readable=[](addr_t a){MEMORY_BASIC_INFORMATION mi{};return a&&!(a&7)&&VirtualQuery(reinterpret_cast<void*>(a),&mi,sizeof(mi))&&mi.State==MEM_COMMIT&&(mi.Protect&(PAGE_READWRITE|PAGE_READONLY|PAGE_EXECUTE_READWRITE|PAGE_WRITECOPY))&&!(mi.Protect&PAGE_GUARD)&&a+16<=reinterpret_cast<addr_t>(mi.BaseAddress)+mi.RegionSize;};
  char head[96];std::snprintf(head,sizeof(head),"%s tid=%lu lr=%#llx",identity.c_str(),GetCurrentThreadId(),(unsigned long long)s.gpr.x30.qword);
  std::string key=head;addr_t fp=s.gpr.x29.qword;
  for(unsigned i=0;i<5&&readable(fp);++i){const auto next=Read<addr_t>(fp),lr=Read<addr_t>(fp+8);char part[24];std::snprintf(part,sizeof(part)," <%#llx",(unsigned long long)lr);key+=part;if(next<=fp)break;fp=next;}
  return key;
}
void WaitProfileRecord(std::string key,uint64_t nanos){
  static std::once_flag started;
  std::call_once(started,[]{std::thread([]{for(;;){std::this_thread::sleep_for(std::chrono::seconds(5));std::vector<std::pair<std::string,WaitProfileEntry>> rows;{std::lock_guard l(WaitProfileMutex());rows.assign(WaitProfileTotals().begin(),WaitProfileTotals().end());WaitProfileTotals().clear();}
    std::sort(rows.begin(),rows.end(),[](auto&a,auto&b){return a.second.nanos>b.second.nanos;});
    std::fputs("Wait profile: top blocking guest waits over the last 5 s (wall ms, calls)\n",stderr);
    for(size_t i=0;i<rows.size()&&i<60;++i)std::fprintf(stderr,"Wait profile: %8.1f ms %7llu %s\n",rows[i].second.nanos/1e6,(unsigned long long)rows[i].second.calls,rows[i].first.c_str());}}).detach();});
  std::lock_guard l(WaitProfileMutex());auto &e=WaitProfileTotals()[std::move(key)];e.nanos+=nanos;++e.calls;
}
void InstallDiagnosticJni(bunker::jni::Host &host,recomp::Runtime &runtime){
  auto invoke=[&host](bool vm,size_t slot,State &state){
    if(!jni_diagnostic_reached.exchange(true))std::fprintf(stderr,"Unity bootstrap: opt-in JNI dispatch diagnostic reached\n");
    const auto pc=state.gpr.pc.qword,lr=state.gpr.x30.qword,sp=state.gpr.sp.qword;
    const std::array<addr_t,4> arguments={state.gpr.x1.qword,state.gpr.x2.qword,state.gpr.x3.qword,state.gpr.x4.qword};
    const bool observe=ObserveFourthFrameThread();
    static thread_local uint64_t observation_sequence=0;
    const auto sequence=observe?++observation_sequence:0;
    if(observe)ObserveFourthFrameJni("ENTRY",vm,slot,sequence,pc,lr,sp,arguments,state.gpr.x0.qword);
    auto prior=host.PendingException();
    try{if(vm)host.InvokeVm(slot,state);else host.InvokeEnv(slot,state);}
    catch(const std::exception &error){
      std::fprintf(stderr,"Unity bootstrap: JNI diagnostic %s slot%zu failed: %s; PC=%#llx LR=%#llx SP=%#llx x1=%#llx x2=%#llx x3=%#llx x4=%#llx pendingBefore=%d\n",
        vm?"VM":"Env",slot,error.what(),static_cast<unsigned long long>(pc),static_cast<unsigned long long>(lr),static_cast<unsigned long long>(sp),
        static_cast<unsigned long long>(arguments[0]),static_cast<unsigned long long>(arguments[1]),static_cast<unsigned long long>(arguments[2]),static_cast<unsigned long long>(arguments[3]),int(!prior.empty()));
      std::abort();
    }
    if(observe)ObserveFourthFrameJni("BODY_RETURN",vm,slot,sequence,pc,lr,sp,arguments,state.gpr.x0.qword);
    auto pending=host.PendingException();
    if(!pending.empty()&&pending!=prior){
      if(init_java_diagnostic_active.load())++init_java_errors;
      std::fprintf(stderr,"Unity bootstrap: original pending Java exception after %s slot%zu return=%#llx: %s\n",vm?"VM":"Env",slot,static_cast<unsigned long long>(state.gpr.x30.qword),pending.c_str());
      if(!vm&&slot==6&&pending.find("java/util/Locale")!=std::string::npos){
        static std::atomic<unsigned> reports{0};auto observed=reports.load(std::memory_order_relaxed);
        while(observed<2&&!reports.compare_exchange_weak(observed,observed+1,std::memory_order_relaxed)){}
        if(observed<2)GameExceptionDiagnostic::ReportJavaFailureFrames(state);
      }
      if(!vm&&slot==6&&pending.find("android/view/View")!=std::string::npos){
        static std::atomic<unsigned> reports{0};auto observed=reports.load(std::memory_order_relaxed);
        while(observed<2&&!reports.compare_exchange_weak(observed,observed+1,std::memory_order_relaxed)){}
        if(observed<2)GameExceptionDiagnostic::ReportJavaFailureFrames(state);
      }
    }
    if(observe)ObserveFourthFrameJni("COMPLETE",vm,slot,sequence,pc,lr,sp,arguments,state.gpr.x0.qword);
  };
  for(size_t slot=4;slot<bunker::jni::kEnvSlots;++slot)runtime.AddImport(host.EnvMarker(slot),[invoke,slot](State &state){invoke(false,slot,state);});
  for(size_t slot=3;slot<bunker::jni::kVmSlots;++slot)runtime.AddImport(host.VmMarker(slot),[invoke,slot](State &state){invoke(true,slot,state);});
}
std::string Sha256(const std::filesystem::path &path){
  // BCrypt's object storage must outlive the hash handle, including failures.
  std::vector<uint8_t> object;
  struct Cng {
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    ~Cng(){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}
  } cng;
  auto check=[](NTSTATUS status){if(status<0)throw std::runtime_error("Windows SHA256 operation failed");};
  check(BCryptOpenAlgorithmProvider(&cng.algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0));
  DWORD object_size=0,got=0;check(BCryptGetProperty(cng.algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&object_size),sizeof(object_size),&got,0));
  object.resize(object_size);check(BCryptCreateHash(cng.algorithm,&cng.hash,object.data(),object.size(),nullptr,0,0));
  std::ifstream input(path,std::ios::binary);if(!input)throw std::runtime_error("cannot open original input for SHA256");
  char buffer[65536];while(input.read(buffer,sizeof(buffer))||input.gcount())check(BCryptHashData(cng.hash,reinterpret_cast<PUCHAR>(buffer),static_cast<ULONG>(input.gcount()),0));
  if(!input.eof())throw std::runtime_error("input read failed during SHA256");
  uint8_t bytes[32];check(BCryptFinishHash(cng.hash,bytes,sizeof(bytes),0));
  std::string result;const char *hex="0123456789abcdef";for(auto b:bytes){result+=hex[b>>4];result+=hex[b&15];}return result;
}
static void *Resolve(const char *name){
  auto it=resolved.find(name);
  if(it==resolved.end()){std::fprintf(stderr,"Unity bootstrap: ELF import absent from verified manifest: %s\n",name);std::abort();}
  return reinterpret_cast<void*>(it->second);
}
template<class Provider> addr_t Try(Provider &provider,const std::string &name){
  try{return provider.Marker(name);}catch(const std::invalid_argument&){return 0;}
}
addr_t TryProcess(bunker::windows::SignalHost &provider,const std::string &name){
  try{return provider.ProcessMarker(name);}catch(const std::invalid_argument&){return 0;}
}
addr_t TryGlImport(bunker::windows::GlHost &provider,const std::string &name){
  return bunker::windows::FindTypedGlesProcedure(provider,name);
}
bool Translated(addr_t entry){
  // Generated trace table is ordered. Data exports are intentionally excluded.
  // The table is immutable. Frequently repeated callback PCs can reuse a
  // positive lookup. Collisions only cause another search of the full table.
  static std::atomic<addr_t> known[256]{};
  auto &slot=known[((entry>>2)^(entry>>11))&255];
  if(entry && slot.load(std::memory_order_relaxed)==entry)return true;
  size_t low=0,high=std::size(kTraces);
  while(low<high){size_t mid=low+(high-low)/2;if(kTraces[mid].address<entry)low=mid+1;else high=mid;}
  if(low==std::size(kTraces)||kTraces[low].address!=entry)return false;
  slot.store(entry,std::memory_order_relaxed);
  return true;
}
bool Executable(addr_t address){
  if(address&3)return false;
  for(const auto &range:kGameExecutableRanges)if(address>=range.low&&address<=range.high&&range.high-address>=4)return true;
  return false;
}
#include "game_bootstrap_detail.h"
#include "game_socket_sigpipe.h"
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
#include "game_collector_delivery.h"
#include "game_unity_main_collector.h"
#include "game_unity_worker_collector.h"
#include "game_socket_wait.h"
#endif
LONG WINAPI Crash(EXCEPTION_POINTERS *exception){
  std::fprintf(stderr,"Unity bootstrap: native exception %#lx at %p; stage=%s, completed constructors=%zu/%zu\n",
      exception->ExceptionRecord->ExceptionCode,exception->ExceptionRecord->ExceptionAddress,stage,constructor_index,std::size(kInitializers));
  std::fprintf(stderr,"Unity bootstrap: native crash thread=%lu module=%p\n",
      GetCurrentThreadId(),static_cast<void*>(GetModuleHandleW(nullptr)));
  const auto &record=*exception->ExceptionRecord;
  if((record.ExceptionCode==EXCEPTION_ACCESS_VIOLATION||record.ExceptionCode==EXCEPTION_IN_PAGE_ERROR)&&record.NumberParameters>=2)
    std::fprintf(stderr,"Unity bootstrap: native memory fault operation=%llu address=%#llx (0=read,1=write,8=execute)\n",
        static_cast<unsigned long long>(record.ExceptionInformation[0]),static_cast<unsigned long long>(record.ExceptionInformation[1]));
  if(const auto *context=exception->ContextRecord){
    std::fprintf(stderr,"Unity bootstrap: native context RIP=%#llx RSP=%#llx RBP=%#llx EFLAGS=%#lx\n",
        static_cast<unsigned long long>(context->Rip),static_cast<unsigned long long>(context->Rsp),static_cast<unsigned long long>(context->Rbp),context->EFlags);
    std::fprintf(stderr,"Unity bootstrap: native context RAX=%#llx RBX=%#llx RCX=%#llx RDX=%#llx RSI=%#llx RDI=%#llx\n",
        static_cast<unsigned long long>(context->Rax),static_cast<unsigned long long>(context->Rbx),static_cast<unsigned long long>(context->Rcx),
        static_cast<unsigned long long>(context->Rdx),static_cast<unsigned long long>(context->Rsi),static_cast<unsigned long long>(context->Rdi));
    std::fprintf(stderr,"Unity bootstrap: native context R8=%#llx R9=%#llx R10=%#llx R11=%#llx R12=%#llx R13=%#llx R14=%#llx R15=%#llx\n",
        static_cast<unsigned long long>(context->R8),static_cast<unsigned long long>(context->R9),static_cast<unsigned long long>(context->R10),static_cast<unsigned long long>(context->R11),
        static_cast<unsigned long long>(context->R12),static_cast<unsigned long long>(context->R13),static_cast<unsigned long long>(context->R14),static_cast<unsigned long long>(context->R15));
  }
  std::fflush(stderr);return EXCEPTION_EXECUTE_HANDLER;
}
struct LocalFrame {
  bunker::jni::Host &host;
  explicit LocalFrame(bunker::jni::Host &value):host(value){State s{};s.gpr.x0.qword=host.EnvAddress();s.gpr.x1.qword=64;host.InvokeEnv(19,s);if(s.gpr.x0.dword)throw std::runtime_error("initJni local frame allocation failed");}
  ~LocalFrame(){State s{};s.gpr.x0.qword=host.EnvAddress();host.InvokeEnv(20,s);}
};
void DefineGameAndroidApiProfile(bunker::jni::Host &host){
  // Chosen framework compatibility contract, independent of APK targetSdk and
  // Windows identity. This catalog still rejects unported API bodies explicitly.
  constexpr int32_t api_level=33;
  LocalFrame locals(host);
  host.DefineClass("android/os/Build$VERSION",{},"java/lang/Object",true);
  host.DefineField("android/os/Build$VERSION","SDK_INT","I",true);
  auto env=[&](size_t slot,addr_t a=0,addr_t b=0,addr_t c=0){State s{};s.gpr.x0.qword=host.EnvAddress();s.gpr.x1.qword=a;s.gpr.x2.qword=b;s.gpr.x3.qword=c;host.InvokeEnv(slot,s);Require(host.PendingException().empty(),"Android compatibility SDK field setup failed");return s.gpr.x0.qword;};
  const auto clazz=env(6,reinterpret_cast<addr_t>("android/os/Build$VERSION"));
  const auto field=env(144,clazz,reinterpret_cast<addr_t>("SDK_INT"),reinterpret_cast<addr_t>("I"));
  env(159,clazz,field,api_level);
  Require(env(150,clazz,field)==api_level,"Android compatibility SDK field differs from selected profile");
  std::fprintf(stderr,"Game bootstrap: explicit Android13 compatibility profile SDK_INT=%d; APK targetSdk and host identity are separate\n",api_level);
}
}
extern "C" Memory *__wrap_sub_825b0544(State &s,addr_t pc,Memory *memory){return GameOriginalSaveHelper(s,pc,memory);}
extern "C" Memory *__wrap_sub_708d905c(State &s,addr_t pc,Memory *memory){return GameOriginalFontCmapHelper(s,pc,memory);}
extern "C" Memory *__wrap_sub_70a8a5c4(State &s,addr_t pc,Memory *memory){return GameOriginalUnityDecodeOuter(s,pc,memory);}
extern "C" Memory *__wrap_sub_70a8aa08(State &s,addr_t pc,Memory *memory){return GameOriginalUnityDecodeInner(s,pc,memory);}
extern "C" void __bunker_set_fpsr_unobserved(bool) noexcept;  // whole/runtime.cpp
int main(int argc,char **argv){
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
  SetUnhandledExceptionFilter(Crash);_set_abort_behavior(0,3);
  // libil2cpp.so and libunity.so contain no MRS/MSR FPSR (checked over the
  // whole images), so per-operation FP exception clearing is unobservable.
  __bunker_set_fpsr_unobserved(true);
  // --quiet-trace may appear anywhere; it is removed before positional parsing.
  for(int i=1;i<argc;++i)if(!std::strcmp(argv[i],"--quiet-trace")){GameQuietTrace()=true;for(int j=i;j+1<argc;++j)argv[j]=argv[j+1];--argc;break;}
  for(int i=1;i+1<argc;++i)if(!std::strcmp(argv[i],"--grpc-native")){GameGrpcNative()=argv[i+1];for(int j=i;j+2<argc;++j)argv[j]=argv[j+2];argc-=2;break;}
  for(int i=1;i<argc;++i)if(!std::strcmp(argv[i],"--server-profile")){GameServerProfile()=true;for(int j=i;j+1<argc;++j)argv[j]=argv[j+1];--argc;break;}
  for(int i=1;i<argc;++i)if(!std::strcmp(argv[i],"--sound")){GameSound()=true;for(int j=i;j+1<argc;++j)argv[j]=argv[j+1];--argc;break;}
  // --bunker-view LOADER STATE DATA: Ctrl+B shows the Bunker's pages over the game (bunker_view.h).
  std::string bunker_view_loader,bunker_view_state,bunker_view_data;
  for(int i=1;i+3<argc;++i)if(!std::strcmp(argv[i],"--bunker-view")){bunker_view_loader=argv[i+1];bunker_view_state=argv[i+2];bunker_view_data=argv[i+3];for(int j=i;j+4<argc;++j)argv[j]=argv[j+4];argc-=4;break;}
  // --render-size WxH: Unity renders at this fixed size (e.g. 3840x2160) and the
  // presenter scales it to the window. --fullscreen: borderless on the monitor.
  int32_t render_width=0,render_height=0;bool start_fullscreen=false;
  for(int i=1;i+1<argc;++i)if(!std::strcmp(argv[i],"--render-size")){
    int w=0,h=0;char x=0;
    if(std::sscanf(argv[i+1],"%d%c%d",&w,&x,&h)!=3||(x!='x'&&x!='X')||w<=0||h<=0||w>16384||h>16384){std::fprintf(stderr,"Game bootstrap: --render-size requires WxH\n");return 2;}
    render_width=w;render_height=h;for(int j=i;j+2<argc;++j)argv[j]=argv[j+2];argc-=2;break;}
  for(int i=1;i<argc;++i)if(!std::strcmp(argv[i],"--fullscreen")){start_fullscreen=true;for(int j=i;j+1<argc;++j)argv[j]=argv[j+1];--argc;break;}
  for(int i=1;i+1<argc;++i)if(!std::strcmp(argv[i],"--target-fps")){
    const bool game=!std::strcmp(argv[i+1],"game");const auto value=game?0L:std::strtol(argv[i+1],nullptr,10);
    if(!game&&(value<1||value>1000)){std::fprintf(stderr,"Game bootstrap: --target-fps requires 1..1000 or game\n");return 2;}
    GameTargetFps()=int32_t(value);for(int j=i;j+2<argc;++j)argv[j]=argv[j+2];argc-=2;break;}
  // Android timed waits (futex, condition, nanosleep) are not quantized to the
  // 15.6 ms default Windows scheduler tick. Request 1 ms for this process.
  {using SetResolution=LONG(NTAPI*)(ULONG,BOOLEAN,PULONG);
   if(auto set=reinterpret_cast<SetResolution>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtSetTimerResolution"))){ULONG actual=0;set(10000,TRUE,&actual);}}
  // Windows 11 withdraws a finer timer resolution while the window is occluded
  // or minimized unless the process opts out (ProcessPowerThrottling,
  // PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION).
  {struct Throttling{ULONG version,control,state;}value{1,0x4,0};using SetInformation=BOOL(WINAPI*)(HANDLE,int,LPVOID,DWORD);
   if(auto set=reinterpret_cast<SetInformation>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"SetProcessInformation")))set(GetCurrentProcess(),4,&value,sizeof(value));}
  if(GameQuietTrace()){
    // Windows stderr is unbuffered: every line was a WriteFile. Buffer it,
    // flush periodically, and flush on abort (Fail/Check paths already fflush).
    static char stderr_buffer[1<<20];std::setvbuf(stderr,stderr_buffer,_IOFBF,sizeof(stderr_buffer));
    std::signal(SIGABRT,[](int){std::fflush(stderr);});
    std::thread([]{for(;;){Sleep(250);std::fflush(stderr);}}).detach();
    std::fputs("Game bootstrap: --quiet-trace enabled; per-call GL/Choreographer/syscall/worker-wait observations omitted, stderr buffered\n",stderr);
  }
  GameFrameRunOptions frame_run;
  try{frame_run=ParseGameFrameRunTail(argc,argv);}catch(const std::exception &error){std::fprintf(stderr,"Frame continuation: %s\n",error.what());return 2;}
  const char *firebase_dll_argument=nullptr;
  if(argc==13&&!std::strcmp(argv[11],"--firebase-app-dll")){firebase_dll_argument=argv[12];argc-=2;}
  const bool first_frame=argc==11&&!std::strcmp(argv[10],"--first-frame");
  if(first_frame)--argc;
  const bool collector_delivery=argc==10&&!std::strcmp(argv[8],"--mute-audio")&&!std::strcmp(argv[9],"--collector-delivery");
  const bool mute_audio=(argc==9&&!std::strcmp(argv[8],"--mute-audio"))||collector_delivery;
  const bool frame_gate=(argc==8||mute_audio)&&!std::strcmp(argv[4],"--frame-gate");
  const bool surface_gate=frame_gate||(argc==7&&!std::strcmp(argv[4],"--surface-gate"));
  try{RequireGameFrameRunProfile(frame_run,first_frame,collector_delivery);}catch(const std::exception &error){std::fprintf(stderr,"Frame continuation: %s\n",error.what());return 2;}
  if(first_frame&&!collector_delivery){std::fputs("First frame diagnostic requires the explicit muted collector profile\n",stderr);return 2;}
  if(firebase_dll_argument&&!first_frame){std::fputs("Windows Firebase SDK requires the explicit first-frame profile\n",stderr);return 2;}
  if(argc!=4&&!(argc==5&&!std::strcmp(argv[4],"--init-jni"))&&!surface_gate){std::fprintf(stderr,"Usage: bunker-game-bootstrap.exe <verified-native-directory> <original-game.apk> <writable-storage-directory> [--init-jni | --surface-gate <landscape-client-width> <height> | --frame-gate <width> <height> <pinned-ANGLE-DLL-directory> [--mute-audio [--collector-delivery]]] [--first-frame [--firebase-app-dll <pinned-SDK-DLL>] [--run-frames <positive-count> | --run-until-close]]\n");return 2;}
  const bool capture_frame = !(GameQuietTrace() && GameServerProfile() && frame_run.mode == GameFrameRunOptions::Mode::UntilClose);
  if(frame_run.Continuing())std::fprintf(stderr,"Frame continuation: enabled mode=%s returnedFrameLimit=%llu; first-frame capture %s\n",frame_run.mode==GameFrameRunOptions::Mode::ReturnedFrames?"returned-frames":"until-close",(unsigned long long)frame_run.returned_frames,capture_frame?"enabled":"off");
#if !defined(BUNKER_GAME_COLLECTOR_DELIVERY) || !BUNKER_GAME_COLLECTOR_DELIVERY
  if(collector_delivery){std::fputs("Game GC: original delivery profile is unavailable in this build\n",stderr);return 2;}
#endif
  const bool init_jni=argc!=4;
  bool frame_continues=true;
  try{
    using namespace bunker::windows;
    namespace fs=std::filesystem;
    int32_t width=0,height=0;
    if(surface_gate){
      auto dimension=[](const char *text){int32_t value=0;auto end=text+std::strlen(text);auto result=std::from_chars(text,end,value);if(result.ec!=std::errc{}||result.ptr!=end||value<=0||value>32767)throw std::invalid_argument("surface dimensions must be integer client pixels in1..32767");return value;};
      width=dimension(argv[5]);height=dimension(argv[6]);if(width<=height)throw std::invalid_argument("APK sensorLandscape surface gate requires width greater than height");
    }
    if(std::size(kInitializers)!=kExpectedConstructors)throw std::runtime_error("trace/metadata constructor count mismatch");
    auto native_directory=fs::canonical(fs::u8path(argv[1]));
    auto library=native_directory/"libunity.so",il2cpp_library=native_directory/"libil2cpp.so";
    auto apk=fs::canonical(fs::u8path(argv[2]));
    const auto firebase_dll=firebase_dll_argument?fs::canonical(fs::u8path(firebase_dll_argument)):fs::path{};
    const auto runtime_apk_sha256=Sha256(apk);
    // This exact data-only Windows variant preserves the verified DEX/native catalog.
    // The second hash is the same variant as the launcher writes it (launcher/main.go):
    // identical entries, with the settings asset stored instead of deflated.
    const bool windows_presentation_apk=first_frame&&collector_delivery&&(runtime_apk_sha256=="fd0d0d357a4e8c316d0e732783d795f0e100a675d0c7a0fcf252873deb84f538"||runtime_apk_sha256=="c9919b7799637455095738a57987255c79fb2669cdc81568f2138b4fcc50c362"
      // ... and the launcher's variant that leaves audio on (--sound).
      ||runtime_apk_sha256=="3eeada0a6e29b0833af7e8a3de5143dd8db83c68fad2741115a8125e50847377"
#ifdef REINCOMP_EMBEDDED_PLAYER
      // The desktop player reads its own configured globalgamemanagers.
      ||runtime_apk_sha256=="9ded0797ee607b6892b531ba3a02f4c4510774f5fe098ccada78ccff881c9209"
#endif
      );
    if(Sha256(library)!=kUnitySha256||Sha256(il2cpp_library)!=il2cpp_image::kIl2cppSha256||(runtime_apk_sha256!=kApkSha256&&!windows_presentation_apk))throw std::runtime_error("runtime original Unity/IL2CPP/APK SHA256 differs from verified translation/metadata");
    if(collector_delivery)Require(std::string(il2cpp_image::kIl2cppSha256)=="ff5c66ad2fdaa957fa005f1957988b3277331abefcdedcdb0c82e59ba2e80522"&&il2cpp_image::kIl2cppBase==0x80000000,"original collector profile byte/base pins differ");
    if(kExploratory)std::fprintf(stderr,"Unity bootstrap: EXPLORATORY DIAGNOSTIC ONLY: %zu unsupported translated traces remain; every real constructor will still execute\n",kUnsupportedTraces);
    else if(kUnsupportedTraces)throw std::runtime_error("strict bootstrap refuses unsupported translated traces");
    auto storage=fs::absolute(fs::u8path(argv[3]));fs::create_directories(storage);
    GameAutomaticEnvironments automatic_environments;game_environments=&automatic_environments;
    automatic_environments.original=__real_sub_825b0544;
    GameFontCmapEnvironments font_environments;game_font_environments=&font_environments;
    font_environments.original=__real_sub_708d905c;
    GameUnityDecodeEnvironments decode_environments;game_unity_decode_environments=&decode_environments;
    decode_environments.original_outer=__real_sub_70a8a5c4;decode_environments.original_inner=__real_sub_70a8aa08;
    std::vector<recomp::Trace> selected(std::begin(kTraces),std::end(kTraces));bool original_helper_present=false;for(auto &trace:selected)if(trace.address==0x825b0544){original_helper_present=true;trace.entry=GameOriginalSaveHelper;}
    Require(original_helper_present,"current IL2CPP automatic save helper is absent from union");
    bool font_helper_present=false;for(auto &trace:selected)if(trace.address==0x708d905c){font_helper_present=true;trace.entry=GameOriginalFontCmapHelper;}
    Require(font_helper_present,"current Unity font cmap helper is absent from union");
    bool decode_outer_present=false,decode_inner_present=false;
    for(auto &trace:selected){if(trace.address==0x70a8a5c4){decode_outer_present=true;trace.entry=GameOriginalUnityDecodeOuter;}if(trace.address==0x70a8aa08){decode_inner_present=true;trace.entry=GameOriginalUnityDecodeInner;}}
    Require(decode_outer_present&&decode_inner_present,"current Unity decode automatic owner helpers are absent from union");
    bool original_raise_present=false;
    if(first_frame&&!GameQuietTrace())for(auto &trace:selected)if(trace.address==0x826263cc){
      GameExceptionDiagnostic::ConfigureRaise(trace.entry,true);
      trace.entry=GameExceptionDiagnostic::Raise;original_raise_present=true;
    }
    Require(!first_frame||GameQuietTrace()||original_raise_present,"original Raise observation entry is absent from union");
    recomp::Runtime runtime;runtime.AddTraces(selected.data(),selected.size());
    if(first_frame)runtime.AddImport(0,[](State &s){
      GameExceptionDiagnostic::ReportNull(s);
      std::fprintf(stderr,"recomp: unresolved indirect context target=0 StatePC=%#llx LR=%#llx SP=%#llx\n",(unsigned long long)s.gpr.pc.qword,(unsigned long long)s.gpr.x30.qword,(unsigned long long)s.gpr.sp.qword);
      std::fputs("recomp: unresolved indirect target at 0\n",stderr);
      std::fflush(stderr);std::abort();
    });
    FileSystem files;
    // Network descriptors share the file namespace. Both providers outlive
    // Pthreads/Drain, so guest workers finish before resources are revoked.
    SocketTransport sockets(files);
    files.EnableGrowdownMappings();
    std::fprintf(stderr,"Game bootstrap: explicit owned Windows MAP_GROWSDOWN profile enabled\n");
    files.MountFile("/data/app/bunker/base.apk",apk);
    files.MountDirectory("/data/app/bunker/lib/arm64",library.parent_path());
    files.MountDirectory("/data/user/0/bunker",storage,true);
    if(!std::getenv("BUNKER_NO_CPU_TOPOLOGY")){
      const auto [cpu_dir,cpuinfo]=bunker::windows::cpu_topology::Build(std::filesystem::path(storage).parent_path()/"android-cpu");
      files.MountDirectory("/sys/devices/system/cpu",cpu_dir);files.MountFile("/proc/cpuinfo",cpuinfo);
      std::fprintf(stderr,"Game bootstrap: /sys/devices/system/cpu and /proc/cpuinfo describe %u cores\n",bunker::windows::cpu_topology::Count());
    }
    // Guest function overrides: lift gaps always, client changes with --server-profile.
    bunker::windows::lift_gaps::AddOverrides();
    bunker::windows::absent_sdks::AddOverrides();
    if(GameServerProfile())bunker::windows::server_profile::InstallEntryOverrides();
    bunker::windows::guest_probes::AddFromEnvironment();
    // --render-size: the game picks its own render resolution through
    // UnityEngine.Screen.SetResolution (e.g. 1280x720). Scale each request so
    // its height is the configured render height, keeping the game's aspect;
    // Unity then sizes its fixed buffers and the presenter scales to the window.
    if(render_height){
      for(const addr_t entry:{addr_t(0x84d7801c),addr_t(0x84d78084),addr_t(0x84d780f4)})  // SetResolution(w,h,mode,rate)/(w,h,bool,rate)/(w,h,bool)
        recomp::AddGuestProbe(entry,[render_width,render_height](State &s){
          const int32_t w=int32_t(s.gpr.x0.dword),h=int32_t(s.gpr.x1.dword);
          if(w<=0||h<=0||h==render_height)return;
          const int32_t sw=int32_t((int64_t(w)*render_height+h/2)/h),sh=render_height;
          std::fprintf(stderr,"Game bootstrap: Screen.SetResolution %dx%d -> %dx%d (render size %dx%d)\n",w,h,sw,sh,render_width,render_height);
          s.gpr.x0.qword=uint32_t(sw);s.gpr.x1.qword=uint32_t(sh);});
    }
    // Automatic-environment owners. --wrap redirects cross-object references
    // only; same-shard direct calls reach these through the guest-call
    // protocol's override filter (previously a --wrap of __bunker_guest_call).
    SceneRoute scene_route;
    bunker::windows::frame_rate::AddOverrides(GameFrameCap());
    // Unity's core topology (libunity 42CAB4) returns {count A | count B<<32,
    // mask A | mask B<<32}; its job-worker pool and the threaded-GfxDevice
    // choice both use A+B. Under the host it counts one core (its per-CPU
    // loop is bounded by Android cpufeatures, which reports none here), so
    // only one job worker ran and rendering stayed on UnityMain. Report the
    // host's logical processors as one cluster instead.
    if(!std::getenv("BUNKER_NO_CPU_TOPOLOGY"))
      recomp::AddGuestDirectEntry(0x7042cab4,+[](State &s,addr_t pc,Memory *memory)->Memory *{
        memory=sub_7042cab4(s,pc,memory);
        const uint32_t counted=s.gpr.x0.dword+uint32_t(s.gpr.x0.qword>>32),n=bunker::windows::cpu_topology::Count();
        if(counted<n){s.gpr.x0.qword=n;s.gpr.x1.qword=(n>=32?0xffffffffu:(1u<<n)-1u);}
        static std::atomic<bool> logged{false};
        if(!logged.exchange(true))std::fprintf(stderr,"Game bootstrap: Unity core topology %u -> %u cores\n",counted,uint32_t(s.gpr.x0.dword));
        return memory;});
    // GfxDevice threading mode setter (libunity 5E9F68; 0 direct, 2 threaded
    // render thread): logged so a run shows which client Unity chose.
    recomp::AddGuestProbe(0x705e9f68,[](State &s){
      std::fprintf(stderr,"Game bootstrap: GfxDevice threading mode %u\n",s.gpr.x0.dword);});
    recomp::AddGuestDirectEntry(0x825b0544,GameOriginalSaveHelper);
    recomp::AddGuestDirectEntry(0x708d905c,GameOriginalFontCmapHelper);
    recomp::AddGuestDirectEntry(0x70a8a5c4,GameOriginalUnityDecodeOuter);
    recomp::AddGuestDirectEntry(0x70a8aa08,GameOriginalUnityDecodeInner);
    recomp::PublishGuestOverrides();
    if(GameServerProfile()){
      namespace sp=bunker::windows::server_profile;
      // Unity extracts global-metadata.dat from the APK into files/il2cpp; patch
      // that copy when il2cpp opens it for reading (idempotent).
      files.SetOpenObserver([storage](const char *path,int flags){
        static const std::string prefix="/data/user/0/bunker/",suffix="/il2cpp/Metadata/global-metadata.dat";
        const std::string guest=path;
        if((flags&3)||guest.size()<prefix.size()+suffix.size()||guest.compare(0,prefix.size(),prefix)||guest.compare(guest.size()-suffix.size(),suffix.size(),suffix))return;
        const auto host=storage/fs::u8path(guest.substr(prefix.size()));
        if(!fs::exists(host))return;
        try{const int n=sp::PatchMetadataStrings(host,sp::MetadataReplacements({}));
          std::fprintf(stderr,"Server profile: metadata %s: %d string literal(s) patched now\n",guest.c_str(),n);}
        catch(const std::exception &error){std::fprintf(stderr,"Server profile: metadata patch failed: %s\n",error.what());std::fflush(stderr);std::abort();}
      });
    }
    files.SetWorkingDirectory("/data/user/0/bunker");
    auto error=[&](int value){files.SetError(value);};
    Pthreads *runner=nullptr;
    Logging logging;logging.SetErrorSink(error);
    // The game's own logcat (Unity Debug.Log, il2cpp and plugin messages).
    // Without a sink liblog reports a filtered write and the text is lost.
    // BUNKER_ANDROID_LOG=0 disables it.
    if(const char*v=std::getenv("BUNKER_ANDROID_LOG");!v||std::strcmp(v,"0"))
      logging.SetLog([](int priority,std::string_view tag,std::string_view text){
        static const char letters[]="??VDIWEFS";const char level=priority>=0&&priority<=8?letters[priority]:'?';
        while(!text.empty()&&(text.back()=='\n'||text.back()=='\r'))text.remove_suffix(1);
        std::fprintf(stderr,"Android log %c/%.*s: %.*s\n",level,int(tag.size()),tag.data(),int(text.size()),text.data());
        return true;});
    logging.SetStream([&](addr_t stream,std::string_view text){return files.FWrite(text.data(),1,text.size(),stream);});
    bunker::jni::Host java(Translated);
    DefineUnityJavaCatalog(java);
    ReflectionLaunchEpoch reflection_launch_epoch(java);
    DefineGameAndroidApiProfile(java);
    JavaTlsTrust::Config tls_config;
    tls_config.observer=[](const JavaTlsTrust::Event &event){
      GameWorkerObservation::NativeEnvironment native;
      std::fprintf(stderr,"Game TLS trust: operation=%s nativeReaderUsed=%d publicCertificates=%zu; original UnityTLS retains peer verification\n",event.operation.c_str(),int(event.reader_used),event.certificate_count);
    };
    // The service and Host outlive Pthreads/Drain and every native TLS callback.
    JavaTlsTrust tls_trust(java,std::move(tls_config));
    // Octo asset downloads (jp.qualiarts.octo.lib.Http): native HTTP into the
    // writable app storage. Paths outside /data/user/0/bunker/ are refused.
    bunker::windows::octo_http::Service octo_http(java,[storage](const std::string &guest)->std::optional<fs::path>{
      static const std::string prefix="/data/user/0/bunker/";
      if(guest.compare(0,prefix.size(),prefix)||guest.find("..")!=std::string::npos)return std::nullopt;
      return storage/fs::u8path(guest.substr(prefix.size()));});
    AndroidBuildManufacturerProfile manufacturer_profile(java,AndroidBuildManufacturerProfile::PropertyPolicy::AbsentAndroidProductProperties);
    if(first_frame)std::fputs("First frame: manufacturer=unknown; explicit absent Android product properties, AOSP fallback; no Windows vendor inference\n",stderr);
    std::unique_ptr<JavaNativeLibraries> native_catalog;
    bunker::jni::Host::ObjectValue application_loader;
    {LocalFrame locals(java);auto call=[&](size_t slot,addr_t a=0,addr_t b=0,addr_t c=0){State s{};s.gpr.x0.qword=java.EnvAddress();s.gpr.x1.qword=a;s.gpr.x2.qword=b;s.gpr.x3.qword=c;java.InvokeEnv(slot,s);Require(java.PendingException().empty(),"native catalog setup raised a Java exception");return s.gpr.x0.qword;};
      auto app=call(6,reinterpret_cast<addr_t>("com/unity3d/player/UnityPlayer")),clazz=call(6,reinterpret_cast<addr_t>("java/lang/Class"));
      auto method=call(33,clazz,reinterpret_cast<addr_t>("getClassLoader"),reinterpret_cast<addr_t>("()Ljava/lang/ClassLoader;"));auto loader=call(34,app,method);
      application_loader=java.CaptureValue(loader);
      JavaNativeLibraries::Config catalog_config{loader,app,{{u"unity",u"/data/app/bunker/lib/arm64/libunity.so"},{u"il2cpp",u"/data/app/bunker/lib/arm64/libil2cpp.so"}}};
      if(!firebase_dll.empty())catalog_config.libraries.push_back({u"FirebaseCppApp-9_6_0",u"/data/app/bunker/lib/arm64/libFirebaseCppApp-9_6_0.so"});
      native_catalog=std::make_unique<JavaNativeLibraries>(java,catalog_config);
    }
    if(frame_gate)AudioVolumeSession::DeclareFields(java);
    AndroidContextConfig context_config;
    context_config.package_name=kPackageName;context_config.target_sdk=kTargetSdk;context_config.activity_class=kLaunchActivity;
    ApplyUnityPackageMetadata(context_config);
    context_config.prepare_internal_directory=MakeInternalDirectoryPreparer(files,context_config.data_dir);
    // This closed mount profile exposes APK/native/internal data only. Its
    // external-volume catalog is independent of APK expansion-file inventory.
    context_config.external_obb_volumes=std::vector<ExternalObbVolume>{};
    context_config.read_android_id=MakeWindowsAndroidIdReader(context_config.package_name);
    context_config.apk_assets=std::make_shared<ApkAssets>(apk);
    stage="Android Context setup";AndroidContext context(java,context_config);
    std::atomic<uint64_t> choreographer_refresh_ratio{0};
    WindowsDisplayService displays(java,[&](const WindowsDisplayService::DisplayInfo &info){
      if(info.id==0&&info.refresh_numerator&&info.refresh_denominator)
        choreographer_refresh_ratio.store((uint64_t(info.refresh_numerator)<<32)|info.refresh_denominator,std::memory_order_relaxed);
      if(first_frame)std::fprintf(stderr,"First frame: Windows display id=%d pixels=%dx%d rotation=%d densityDpi=%d refresh=%u/%u appOffsetNs=%lld nominalPresentationBudgetNs=%lld; native-port scheduling policy, SDR window capability\n",info.id,info.width,info.height,info.rotation,info.density_dpi,info.refresh_numerator,info.refresh_denominator,(long long)info.app_vsync_offset_ns,(long long)info.nominal_presentation_budget_ns);
    });
    stage="SharedPreferences setup";
    auto preferences_backend=MakeWindowsSharedPreferencesBackend(context_config.package_name);
    auto read_preferences=std::move(preferences_backend.read);
    preferences_backend.read=[read=std::move(read_preferences),package=context_config.package_name](const std::u16string &name){
      auto snapshot=read(name);
      std::fprintf(stderr,"Game bootstrap: original preferences read store=%s observed=%s entries=%zu\n",name==package?"legacy":"v2",snapshot?"present":"absent",snapshot?snapshot->size():0);
      return snapshot;
    };
    auto write_preferences=std::move(preferences_backend.write);
    preferences_backend.write=[write=std::move(write_preferences),package=context_config.package_name](const std::u16string &name,const SharedPreferencesSnapshot &snapshot){
      auto status=write(name,snapshot);auto version=snapshot.find(u"__UNITY_PLAYERPREFS_VERSION__");
      std::fprintf(stderr,"Game bootstrap: original preferences write store=%s entries=%zu version_kind=%u version_bits=%#llx status=%u\n",name==package?"legacy":"v2",snapshot.size(),version==snapshot.end()?0:unsigned(version->second.kind),(unsigned long long)(version==snapshot.end()?0:version->second.bits),status);
      return status;
    };
    std::unique_ptr<AndroidSharedPreferences> preferences;
    {LocalFrame locals(java);preferences=std::make_unique<AndroidSharedPreferences>(java,context.Activity(),context_config.package_name,std::move(preferences_backend));}
    std::unique_ptr<JniInterfaceProxy> interface_proxies;
    std::unique_ptr<ReflectionHelper> reflection_helper;
    // The input APK was SHA256-verified above. No extra split/JAR/DEX sources or
    // dynamic Java code loader are exposed by this diagnostic's closed catalog.
    // Downloaded movies live in app storage. Chapter titles use StreamingAssets
    // URLs inside the APK, whose contents the launcher extracts to apk-assets.
    const auto movie_assets = "jar:file://" + Utf8(context_config.source_dir) + "!/assets/";
    const auto movie_storage = Utf8(context_config.data_dir) + "/";
#ifdef REINCOMP_EMBEDDED_PLAYER
    // The embedded Android mixer is muted because the Windows player owns
    // gameplay audio. Movie audio follows the desktop launcher's sound setting.
    const bool movie_sound=!std::getenv("REINCOMP_MUTE_AUDIO");
#else
    const bool movie_sound=GameSound() || !mute_audio;
#endif
    bunker::windows::avpro_video::Service avpro_video(java,[storage,movie_assets,movie_storage](const std::string &guest)->std::optional<fs::path>{
      const bool packaged = guest.compare(0,movie_assets.size(),movie_assets)==0;
      if(!packaged && guest.compare(0,movie_storage.size(),movie_storage)!=0)return std::nullopt;
      const auto tail = guest.substr(packaged ? movie_assets.size() : movie_storage.size());
      const auto relative = fs::u8path(tail);
      if(relative.empty() || relative.has_root_path() || tail.find_first_of("\\:")!=std::string::npos)return std::nullopt;
      for(const auto &part:relative)if(part=="..")return std::nullopt;
      return (packaged ? storage.parent_path()/"apk-assets" : storage)/relative;
    },movie_sound);
    stage="asset-pack wrapper setup";AndroidAssetPackWrapper asset_packs(java,{runtime_apk_sha256,"b2be67185a866f3489146d9864ca40d1de5c2668037f026609890afe12940689",true});
    stage="native providers setup";
    // Real policy/semaphores for explicitly bound threads. These cohorts are
    // without a delivery binding: unknown contexts fail rather than queueing
    // a request that cannot receive a real same-worker acknowledgment.
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
    GameCollectorDelivery *collector_notify=nullptr;
    GameUnityMainCollector *main_collector_notify=nullptr;
    GameUnityWorkerCollector *worker_collector_notify=nullptr;
#endif
    SignalHost process_signals([&](uint64_t token){
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
      if(main_collector_notify&&main_collector_notify->Owns(token)){main_collector_notify->Notify(token);return;}
      if(worker_collector_notify&&worker_collector_notify->Owns(token)){worker_collector_notify->Notify(token);return;}
      if(collector_notify){collector_notify->Notify(token);return;}
#endif
      std::fprintf(stderr,"Game bootstrap: unsupported unbound delivery target=%#llx\n",(unsigned long long)token);std::abort();},0x7ff600100000ULL,
      [&files](addr_t address,size_t size)->std::optional<SignalHost::StackRegion>{
        auto mapping=files.MappingRegion(address,size,true);
        if(!mapping)return {};
        return SignalHost::StackRegion{mapping->base,mapping->base+mapping->size,mapping->generation};
      },
      [&files](addr_t address,size_t size,uint64_t generation)->std::optional<SignalHost::StackLease>{
        auto use=files.AcquireMappingUse(address,size,true);
        if(!use||use->region.generation!=generation)return {};
        return SignalHost::StackLease{{use->region.base,use->region.base+use->region.size,use->region.generation},std::move(use->lease)};
      });process_signals.SetErrorSink(error);
    Semaphores semaphores(process_signals);semaphores.SetErrorSink(error);
    SensorService sensors(SensorService::Profile::Unavailable);
    std::unique_ptr<NonlocalJumps> jumps;
    recomp::Runtime window_bridges;
    recomp::Runtime egl_bridges,gl_bridges;
    std::mutex window_call_mutex;std::map<std::string,uint64_t> window_calls;
    std::unique_ptr<NativeWindows> native_windows;
    std::unique_ptr<EglHost> egl;
    std::unique_ptr<GlHost> graphics;
    std::atomic<uint64_t> observed_egl_calls{0},observed_presentations{0};
    // Above refresh the guest's swap interval is disabled. Pace successful
    // window draws on their render thread; keep request pacing for frames
    // that only load assets or draw offscreen.
    const bool pace_window_swaps=frame_gate&&GameFrameCap()>DisplayRefreshFps();
    struct WindowPresent {uint64_t ticket;addr_t display,surface;bool window;};
    std::mutex present_mutex;std::map<DWORD,WindowPresent> pending_window_presentations;
    // ModuleRegistry owns 0x7ff400000000; keep this combined provider disjoint.
    if(surface_gate)native_windows=std::make_unique<NativeWindows>(java,0x7ff440000000ULL);

    if(frame_gate){
      auto dlls=fs::canonical(fs::u8path(argv[7]));
      egl=std::make_unique<EglHost>(*native_windows,dlls.wstring());
      if(pace_window_swaps)egl->UseHostFramePacing();
      graphics=std::make_unique<GlHost>(*egl,dlls.wstring());
      if(first_frame){
        if(capture_frame)graphics->EnableFrameDiagnostics((storage/L"first-frame.png").wstring());
        egl->SetDiagnosticObserver([&](const std::string &name,const State &state,bool completed){
          if(!completed){++observed_egl_calls;if(!GameQuietTrace())std::fprintf(stderr,"First frame: EGL ENTER %s native_tid=%lu guestLR=%#llx\n",name.c_str(),GetCurrentThreadId(),(unsigned long long)state.gpr.x30.qword);
            if(name=="eglSwapBuffers"){
              WindowPresent pending{0,state.gpr.x0.qword,state.gpr.x1.qword,false};
              pending.window=egl->IsCurrentDrawWindow(pending.display,pending.surface);
              pending.ticket=graphics->BeginWindowPresent(pending.display,pending.surface);
              if(pending.window&&pace_window_swaps){
                static thread_local GameFrameLimiter swaps(GameFrameCap());
                swaps.WaitForSwap();
              }
              std::lock_guard<std::mutex> lock(present_mutex);
              if(!pending_window_presentations.emplace(GetCurrentThreadId(),pending).second)throw std::logic_error("nested original window presentation observation");
            }
          }else{
            if(name=="eglSwapBuffers"){
              WindowPresent pending{};
              {std::lock_guard<std::mutex> lock(present_mutex);auto found=pending_window_presentations.find(GetCurrentThreadId());
                if(found!=pending_window_presentations.end()){pending=found->second;pending_window_presentations.erase(found);}}
              graphics->FinishWindowPresent(pending.ticket,pending.display,pending.surface,state.gpr.x0.dword);
              if(pending.window&&state.gpr.x0.dword==1){
                ++observed_presentations;
                const auto now=std::chrono::steady_clock::now();
                static thread_local auto window=now,previous=now;
                static thread_local std::vector<double> intervals;
                if(now!=previous)intervals.push_back(std::chrono::duration<double,std::milli>(now-previous).count());
                previous=now;
                const double seconds=std::chrono::duration<double>(now-window).count();
                if(seconds>=5){
                  std::sort(intervals.begin(),intervals.end());
                  const size_t n=intervals.size();
                  const auto within_budget=std::upper_bound(intervals.begin(),intervals.end(),1000.0/120.0);
                  const size_t over_budget=static_cast<size_t>(intervals.end()-within_budget);
                  const auto first_miss=std::lower_bound(intervals.begin(),intervals.end(),8.30);
                  const size_t misses=static_cast<size_t>(intervals.end()-first_miss);
                  std::fprintf(stderr,"Present rate: %.1f fps (%zu presents in %.1fs) frame ms p50=%.2f p95=%.2f p99=%.2f max=%.2f over_120hz_budget=%zu/%zu at_or_above_8_30ms=%zu/%zu\n",
                    n/seconds,n,seconds,intervals[(n-1)/2],intervals[n-1-n/20],intervals[n-1-n/100],intervals.back(),over_budget,n,misses,n);
                  intervals.clear();
                  window=now;
                }
              }
            }
            if(!GameQuietTrace())std::fprintf(stderr,"First frame: EGL RETURN %s x0=%#llx successfulPresentations=%llu\n",name.c_str(),(unsigned long long)state.gpr.x0.qword,(unsigned long long)observed_presentations.load());
          }
        });
        if(capture_frame)std::fputs("First frame: diagnostics enabled; original EGL/GLES only, window readback before original presentation; initial EGL/draw/presentation counts=0\n",stderr);
      }
      egl->SetProcedureResolver([&](const std::string &name){
        std::fprintf(stderr,"Unity bootstrap: requested typed GL procedure %s\n",name.c_str());return FindTypedGlesProcedure(*graphics,name);
      });
    }
    GameModuleLedger unity_ledger{"Unity",{std::begin(unity_image::kConstructors),std::end(unity_image::kConstructors)},{std::begin(unity_image::kUnityFinalizers),std::end(unity_image::kUnityFinalizers)}};
    GameModuleLedger il2cpp_ledger{"IL2CPP",{std::begin(il2cpp_image::kConstructors),std::end(il2cpp_image::kConstructors)},{std::begin(il2cpp_image::kFinalizers),std::end(il2cpp_image::kFinalizers)}};
    ModuleRegistry modules([&](addr_t entry){try{Require(runner,"module lifecycle runner is unavailable");auto *owner=unity_ledger.Owns(entry)?&unity_ledger:il2cpp_ledger.Owns(entry)?&il2cpp_ledger:nullptr;Require(owner,"module lifecycle entry has no verified owner");owner->Run(entry,*runner);constructor_index=unity_ledger.constructed;}catch(const std::exception &e){std::fprintf(stderr,"Game bootstrap: module lifecycle failed:%s entry=%#llx\n",e.what(),(unsigned long long)entry);std::abort();}});
    LibC libc([&](addr_t entry,const std::vector<addr_t> &args){return runner->Run(entry,args);});
    {
      // il2cpp's Boehm GC reads GC_* tuning from getenv at GC_init. Each
      // collection stops the world through emulated signals (slow here), and a
      // scene load ran 40+ a second while the heap grew from its small default.
      // Start with a large heap and grow rather than collect; host GC_*
      // variables override (and are passed through).
      std::map<std::string,std::string> gc{{"GC_INITIAL_HEAP_SIZE","1073741824"},{"GC_FREE_SPACE_DIVISOR","1"}};
#ifdef _WIN32
      if(LPWCH block=GetEnvironmentStringsW()){
        for(LPWCH at=block;*at;at+=wcslen(at)+1){const std::string entry=fs::path(at).u8string();
          if(entry.rfind("GC_",0)==0&&entry.find('=')!=std::string::npos)gc[entry.substr(0,entry.find('='))]=entry.substr(entry.find('=')+1);}
        FreeEnvironmentStringsW(block);}
#else
      for(char **at=environ;at&&*at;++at){const std::string entry=*at;
        if(entry.rfind("GC_",0)==0&&entry.find('=')!=std::string::npos)gc[entry.substr(0,entry.find('='))]=entry.substr(entry.find('=')+1);}
#endif
      for(const auto &[name,value]:gc){libc.Setenv(name.c_str(),value.c_str(),1);std::fprintf(stderr,"Game bootstrap: guest %s=%s\n",name.c_str(),value.c_str());}
    }
    // The native SDK service and its retained trampolines outlive guest drain.
    std::unique_ptr<FirebaseAppService> firebase;
    std::unique_ptr<bunker::windows::grpc_native::Service> grpc_service;
    logging.SetAllocator([&](uint64_t size){return libc.Malloc(size);});
    libc.SetErrorSink(error);libc.SetEnvironment({});
    // Import callbacks can reference this audit until every worker has joined.
    GameTlsAudit tls_audit;tls_audit.free_callback=libc.Marker("free");
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
    std::unique_ptr<GameUnityWorkerCollector> worker_collector;
    std::unique_ptr<GameCollectorDelivery> collector;
    std::unique_ptr<GameUnityMainCollector> main_collector;
    std::unique_ptr<GameSocketWaitCollector> socket_wait_collector;
#endif
    // Destroy/join guest workers before the providers they may still call.
    Pthreads threads(runtime,Translated,0x7ff900000000ULL,
      [&libc](addr_t entry){return Translated(entry)||entry==libc.Marker("free");});threads.SetErrorSink(error);runner=&threads;
    bunker::windows::absent_sdks::Call()=[&threads](addr_t entry,const std::vector<addr_t> &args){return threads.Run(entry,args);};
    bunker::windows::frame_rate::Call()=[&threads](addr_t entry,const std::vector<addr_t> &args,const float *in,float *out){return threads.RunFloat(entry,args,in,out);};
    // Declared before Drain so all guest workers stop before this service is revoked.
    std::unique_ptr<JavaNativeLoader> native_loader;
    std::unique_ptr<UnityNetworkProxy> network_proxy;
    std::unique_ptr<AndroidChoreographer> choreographer;
    modules.SetProgramHeaderRunner([&](addr_t callback,addr_t info,uint64_t size,addr_t data){
      Require(Translated(callback),"program-header callback is not translated");
      Require(size==sizeof(AndroidDlPhdrInfo),"program-header callback ABI extent differs from Android13");
      if(!GameQuietTrace()){
        GameWorkerObservation::NativeEnvironment native;
        const auto entry=Read<AndroidDlPhdrInfo>(info);
        const auto name=entry.dlpi_name?recomp::ReadGuestString(entry.dlpi_name):std::string();
        std::fprintf(stderr,"Game bootstrap: original phdr callback=%#llx info=%#llx size=%llu data=%#llx module=%s bias=%#llx phdr=%#llx phnum=%u adds=%llu subs=%llu; actual active guest ELF metadata\n",(unsigned long long)callback,(unsigned long long)info,(unsigned long long)size,(unsigned long long)data,name.c_str(),(unsigned long long)entry.dlpi_addr,(unsigned long long)entry.dlpi_phdr,unsigned(entry.dlpi_phnum),(unsigned long long)entry.dlpi_adds,(unsigned long long)entry.dlpi_subs);
      }
      const auto result=static_cast<int32_t>(threads.Run(callback,{info,size,data}));
      if(!GameQuietTrace()){GameWorkerObservation::NativeEnvironment native;std::fprintf(stderr,"Game bootstrap: original phdr callback=%#llx returned=%d\n",(unsigned long long)callback,result);}
      return result;
    });
    // Existing profiles retain observation-only hooks and the nonzero guard.
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
    if(collector_delivery){
      worker_collector=std::make_unique<GameUnityWorkerCollector>(threads,process_signals,semaphores,runtime);worker_collector_notify=worker_collector.get();
      GameCollectorDelivery::WorkerExtension extension;
#ifdef REINCOMP_EMBEDDED_PLAYER
      extension.sender_parent_roots=NativePlayerSenderRoots;
#endif
      extension.prepare=[&](const auto&info,auto&lifecycle){worker_collector->Prepare(info,lifecycle);};
      extension.enter=[&](const auto&info){worker_collector->Enter(info);};
      extension.before_tls=[&](const auto&info){worker_collector->BeforeTls(info);};
      extension.retire=[&](const auto&info,auto reason){worker_collector->Retire(info,reason);};
      extension.owns=[&](uint64_t token){return worker_collector->Owns(token);};
      extension.select_mutex=[&](const auto&context){return worker_collector->SelectMutex(context);};
      extension.checkpoint=[&](const auto&context){worker_collector->Checkpoint(context);};
      extension.main_owns=[&](uint64_t token){return main_collector&&main_collector->Owns(token);};
      extension.main_select=[&](const auto&context){return main_collector->SelectMutex(context);};
      extension.main_checkpoint=[&](const auto&context){main_collector->MutexCheckpoint(context);};
      collector=std::make_unique<GameCollectorDelivery>(threads,process_signals,semaphores,runtime,std::move(extension));collector_notify=collector.get();
      main_collector=std::make_unique<GameUnityMainCollector>(threads,process_signals,semaphores,runtime);main_collector_notify=main_collector.get();
      socket_wait_collector=std::make_unique<GameSocketWaitCollector>(threads,*collector,*main_collector,*worker_collector);
      tls_audit.create_boundary=[&](State &s,const auto &invoke){worker_collector->WithCreate(s,[&](State&t){collector->WithCreate(t,invoke);});};
    }
    else
#endif
    threads.SetWorkerLifecycleHooks({
      [](const Pthreads::WorkerInfo &info){GameWorkerObservation::Lifecycle("prepare",info);return Pthreads::WorkerLease{};},
      [](const Pthreads::WorkerInfo &info,const Pthreads::WorkerLease &){GameWorkerObservation::Enter(info);},
      [](const Pthreads::WorkerInfo &info,const Pthreads::WorkerLease &){GameWorkerObservation::Lifecycle("before_tls",info);},
      [](const Pthreads::WorkerInfo &info,const Pthreads::WorkerLease &,Pthreads::WorkerRetirement reason){GameWorkerObservation::Retire(info,reason);}
    });
    const auto parent_token=threads.Self();Require(!process_signals.RegisterThread(parent_token)&&!process_signals.BindCurrent(parent_token),"cannot bind actual UI virtual signal context");
    if(first_frame){GameWorkerObservation::NativeEnvironment native;
      std::fprintf(stderr,"Game bootstrap: UI virtual thread registered token=%#llx native_tid=%lu; observation only\n",(unsigned long long)parent_token,GetCurrentThreadId());std::fflush(stderr);}
    auto current_signal_thread=[&]{auto token=threads.Self();Require(bool(process_signals.Lookup(token)),"guest-created worker lacks proved SignalHost lifecycle hooks");return token;};
    auto current_jump_thread=[&](){
      auto token=threads.Self();AndroidThreadAttr attr{};
      if(!process_signals.Lookup(token))throw std::runtime_error("setjmp thread has no registered virtual signal-mask lifetime");
      if(threads.GetAttr(token,&attr)||!attr.stack_base||!attr.stack_size||attr.stack_size>UINT64_MAX-attr.stack_base)throw std::runtime_error("setjmp thread has no owned guest stack");
      return NonlocalJumps::Thread{token,token,attr.stack_base,attr.stack_base+attr.stack_size};
    };
    automatic_environments.current=current_jump_thread;
    font_environments.current=current_jump_thread;
    decode_environments.current=current_jump_thread;
    if(surface_gate)jumps=std::make_unique<NonlocalJumps>(current_jump_thread,[&files,current_jump_thread](addr_t environment,size_t size)->std::optional<NonlocalJumps::Region>{
      if(auto mapping=files.MappingRegion(environment,size,true)){
        if(!GameQuietTrace())std::fprintf(stderr,"Unity bootstrap: jmp_buf=%#llx owned mmap=[%#llx,%#llx) generation=%llu\n",static_cast<unsigned long long>(environment),static_cast<unsigned long long>(mapping->base),static_cast<unsigned long long>(mapping->base+mapping->size),static_cast<unsigned long long>(mapping->generation));
        return NonlocalJumps::Region{mapping->base,mapping->base+mapping->size,mapping->generation};
      }
      auto current=current_jump_thread();
      if(auto owned=game_environments->Find(environment,size,current.token))return owned;
      if(auto owned=game_font_environments->Find(environment,size,current.token))return owned;
      if(auto owned=game_unity_decode_environments->Find(environment,size,current.token))return owned;
      std::fprintf(stderr,"Unity bootstrap: unknown jmp_buf allocation=%#llx bytes=%zu; owned guest stack=[%#llx,%#llx); no mmap/stack lifetime; heap/subobject provenance is not invented\n",static_cast<unsigned long long>(environment),size,static_cast<unsigned long long>(current.stack_low),static_cast<unsigned long long>(current.stack_high));
      return {};
    },[&](){uint64_t mask=0;auto token=threads.Self();if(process_signals.Mask(token,0,nullptr,&mask))throw std::runtime_error("setjmp signal mask is not registered for this virtual thread");return mask;},Executable);
    // Every referenced dispatcher outlives worker drain, including exception
    // unwinding, guest TLS, and Zlib's genuine guest allocator cleanup calls.
    recomp::Runtime module_bridges,signal_bridges,jump_bridges;
    // Timed delays publish no coordinator/finalizer roots. Keep an eligible
    // signal pending and fail before consuming it at this provider boundary.
    // These providers and their callbacks must outlive Drain's worker shutdown.
    auto sleep_no_delivery=[](const SignalHost::Delivery&)->std::optional<uint64_t>{throw std::runtime_error("timed-delay signal roots are not qualified");};
    SignalSleeps sleeps(process_signals,current_signal_thread,sleep_no_delivery,
                        [](uint64_t){return uint64_t{0};});
    sleeps.SetErrorSink(error);
    SignalHost::Boundary nanosleep_boundary=sleep_no_delivery;
    SignalNanoSleeps::Admission nanosleep_admission=[](uint64_t){return uint64_t{0};};
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
    if(main_collector){
      nanosleep_boundary=[&](const auto &delivery){return main_collector->Deliver(delivery);};
      nanosleep_admission=[&](uint64_t token){return main_collector->Owns(token)?main_collector->NanoSleepAdmission(token):uint64_t{0};};
    }
    if(collector){
      nanosleep_admission=[&,main_admission=nanosleep_admission](uint64_t token){return collector->BackoffOwner(token)?collector->NanoSleepAdmission(token):main_admission(token);};
    }
#endif
    SignalNanoSleeps nanosleeps(process_signals,current_signal_thread,nanosleep_boundary,nanosleep_admission);
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
    if(main_collector||collector)nanosleeps.SetDispatcher([&](uint64_t token,uint64_t allowed,const SignalHost::Boundary&boundary){
      if(main_collector&&main_collector->Owns(token))return main_collector->DispatchNanoSleep(token,allowed,boundary);
      if(collector&&collector->BackoffOwner(token))return collector->DispatchNanoSleep(token,allowed,boundary);
      // Unity workers take a pending stop at their next compiled poll site;
      // the backoff sleep itself leaves it pending.
      if(worker_collector&&worker_collector->Owns(token))return process_signals.DispatchOneAdmittedSubset(token,SignalHost::Bit(30)|SignalHost::Bit(24),0,{});
      return process_signals.DispatchOneAdmitted(token,allowed,boundary);});
#endif
    nanosleeps.SetErrorSink(error);
    nanosleeps.SetRequestObserver([](const SignalNanoSleeps::Observation &observation){
      if(GameQuietTrace())return;
      GameWorkerObservation::NativeEnvironment native;
      std::fprintf(stderr,"Game bootstrap: original nanosleep COPIED caller_native_tid=%lu request=%#llx remaining=%#llx copied=%d seconds=%lld nanoseconds=%lld; immutable input snapshot, no blocking claim\n",GetCurrentThreadId(),(unsigned long long)observation.request,(unsigned long long)observation.remaining,int(observation.copied),(long long)observation.seconds,(long long)observation.nanoseconds);
      std::fflush(stderr);
    });
    recomp::Runtime sleep_bridges;
    auto futex_no_delivery=[](const SignalHost::Delivery&)->std::optional<uint64_t>{throw std::runtime_error("private-futex signal roots are not qualified");};
    SignalHost::Boundary futex_boundary=futex_no_delivery;
    SignalFutexes::Admission futex_admission=[](uint64_t){return uint64_t{0};};
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
    if(worker_collector){
      futex_boundary=[&](const auto&delivery){return main_collector&&main_collector->Owns(delivery.token)?main_collector->Deliver(delivery):worker_collector->Deliver(delivery);};
      futex_admission=[&](uint64_t token){return main_collector&&main_collector->Owns(token)?main_collector->FutexAdmission(token):worker_collector->Owns(token)?worker_collector->FutexAdmission(token):uint64_t{0};};
    }
#endif
    SignalFutexes futexes(process_signals,current_signal_thread,futex_boundary,futex_admission);
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
    if(worker_collector)futexes.SetDispatcher([&](uint64_t token,uint64_t allowed,const SignalHost::Boundary&boundary){if(main_collector&&main_collector->Owns(token))return main_collector->DispatchFutex(token,allowed,boundary);return worker_collector->Owns(token)?worker_collector->DispatchFutex(token,allowed,boundary):process_signals.DispatchOneAdmitted(token,allowed,boundary);});
#endif
    futexes.SetErrorSink(error);
    futexes.SetWaitObserver([&](const SignalFutexes::WaitObservation &observation){
      GameWorkerObservation::NativeEnvironment native;
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
      if(worker_collector)worker_collector->ObserveFutex(observation);
      if(main_collector)main_collector->ObserveFutex(observation);
#endif
      // Per-wait diagnostics: several lines plus a flush on every timed futex
      // (hundreds per frame) cost measurable frame time, so --quiet-trace drops them.
      if(GameQuietTrace())return;
      if(observation.phase==SignalFutexes::WaitPhase::Copied){
        std::fprintf(stderr,"Game bootstrap: original timed futex COPIED caller_native_tid=%lu timeout=%#llx copied=%d seconds=%lld nanoseconds=%lld deadline_valid=%d deadline_ns=%llu; fixed active-monotonic snapshot, no blocking claim\n",GetCurrentThreadId(),(unsigned long long)observation.timeout_pointer,int(observation.copied),(long long)observation.seconds,(long long)observation.nanoseconds,int(observation.deadline_valid),(unsigned long long)observation.deadline_nanoseconds);
      }else{
        std::fprintf(stderr,"Game bootstrap: original timed futex RESULT caller_native_tid=%lu timeout=%#llx copied=%d seconds=%lld nanoseconds=%lld deadline_valid=%d deadline_ns=%llu result=%lld operation_error=%d\n",GetCurrentThreadId(),(unsigned long long)observation.timeout_pointer,int(observation.copied),(long long)observation.seconds,(long long)observation.nanoseconds,int(observation.deadline_valid),(unsigned long long)observation.deadline_nanoseconds,(long long)observation.result,observation.error);
      }
      std::fflush(stderr);
    });
    recomp::Runtime futex_bridges;
    ThreadPriorities priorities(threads,ThreadPriorities::Profile::UnityNormalClass);
    priorities.SetErrorSink(error);
    recomp::Runtime priority_bridges;
    AndroidProcess android_process(java,priorities,[](const AndroidProcess::Observation &observation){
      GameWorkerObservation::NativeEnvironment native;
      std::fprintf(stderr,"Game bootstrap: original Process.setThreadPriority %s caller_native_tid=%lu target_tid=%u nice=%d error=%d; explicit Windows Unity4/NORMAL translation\n",observation.phase==AndroidProcess::Observation::Phase::Enter?"ENTER":"RETURN",GetCurrentThreadId(),observation.tid,observation.nice,observation.error);
      std::fflush(stderr);
    });
    ThreadAffinities affinities(threads,ThreadAffinities::Profile::WindowsSingleGroupNativeIndices,0x7ff600600000ULL,[](const ThreadAffinities::Observation &observation){
      GameWorkerObservation::NativeEnvironment native;
      if(observation.phase==ThreadAffinities::Observation::Phase::MaskCopied){
        std::fprintf(stderr,"Game bootstrap: original sched_setaffinity MASK-COPIED caller_native_tid=%lu pid=%d size=%u pointer=%#llx copied=%d requested=%#x; exact provider copy, no return claim\n",GetCurrentThreadId(),observation.pid,observation.size,(unsigned long long)observation.user_mask,int(observation.mask_copied),observation.requested_mask);
      }else{
        std::fprintf(stderr,"Game bootstrap: original sched_setaffinity RESULT caller_native_tid=%lu pid=%d resolved_tid=%u copied=%d target=%d topology=%d native_set=%d readback_valid=%d requested=%#x process=%#llx system=%#llx effective=%#llx previous=%#llx readback=%#llx result=%lld operation_error=%d\n",GetCurrentThreadId(),observation.pid,observation.resolved_tid,int(observation.mask_copied),int(observation.target_resolved),int(observation.topology_valid),int(observation.native_set_called),int(observation.readback_valid),observation.requested_mask,(unsigned long long)observation.process_mask,(unsigned long long)observation.system_mask,(unsigned long long)observation.effective_mask,(unsigned long long)observation.previous_mask,(unsigned long long)observation.readback_mask,(long long)observation.result,observation.error);
      }
      std::fflush(stderr);
    });
    affinities.SetErrorSink(error);
    recomp::Runtime affinity_bridges;
    Zlib zlib([&](addr_t entry,const std::vector<addr_t> &args){return threads.Run(entry,args);});
    struct Drain {Pthreads &threads;std::function<void()> before,close;~Drain(){if(before)before();if(threads.Shutdown())std::abort();if(close)close();}} drain{threads,{},{}};
    drain.before=[&]{if(choreographer)try{choreographer->Stop();}catch(const std::exception &e){std::fprintf(stderr,"Game Choreographer shutdown failed:%s\n",e.what());std::abort();}};
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
    if(collector)drain.close=[&]{collector->CloseAfterShutdown();main_collector->Close();worker_collector->Close();};
#endif
    DescriptorImportInterceptor network_invocation=[&](const char *name,State &state,const std::function<void()> &invoke){
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
      if(socket_wait_collector){socket_wait_collector->WithSocketWait(name,state,invoke);return;}
#endif
      (void)name;(void)state;invoke();
    };
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
    if(socket_wait_collector)sockets.SetWaitObserver([&](const SocketTransport::WaitObservation &observation){return socket_wait_collector->Observe(observation);});
#endif
    sockets.SetBrokenPipeNotification([&](State *state){
      GameSocketSigpipe::Notify(process_signals,threads.Self(),state);
    });
    files.Install(runtime,[network_invocation](const char *name,State &state,const std::function<void()> &invoke){
      if(!std::strcmp(name,"read")||!std::strcmp(name,"write")){network_invocation(name,state,invoke);return;}
      invoke();
    });
    sockets.Install(runtime,network_invocation);threads.Install();libc.Install(runtime);logging.Install(runtime);
    sensors.Install(runtime);
    std::fputs("Game bootstrap: Android sensor service unavailable in this port; original Unity NULL-manager branch; no hardware inventory or sensor samples claimed\n",stderr);
    constexpr addr_t mmap_audit_marker=0x7ffc68000000ULL;
    auto mmap_bridge=recomp::MakeBridge<addr_t(addr_t,uint64_t,int32_t,int32_t,int32_t,int64_t)>([&files](auto address,auto length,auto protection,auto flags,auto descriptor,auto offset){return files.Mmap(address,length,protection,flags,descriptor,offset);});
    runtime.AddImport(mmap_audit_marker,[&,mmap_bridge](State &s){
      const auto length=s.gpr.x1.qword,caller=s.gpr.x30.qword;const auto flags=s.gpr.x3.dword;
      try{
        mmap_bridge(s);
        if(!GameQuietTrace())std::fprintf(stderr,"Game bootstrap: original mmap length=%llu flags=%#x LR=%#llx result=%#llx errno=%d\n",(unsigned long long)length,flags,(unsigned long long)caller,(unsigned long long)s.gpr.x0.qword,files.Error());
      }
      catch(const std::exception &e){std::fprintf(stderr,"Game bootstrap: original mmap length=%llu flags=%#x LR=%#llx failed:%s\n",(unsigned long long)length,flags,(unsigned long long)caller,e.what());std::abort();}
    });
    constexpr addr_t sscanf_audit_marker=0x7ffc68000008ULL;
    auto sscanf_bridge=recomp::MakeBridge<int32_t(const char*,const char*,addr_t,addr_t,addr_t,addr_t)>([&libc](auto input,auto format,auto a,auto b,auto c,auto d){return libc.Scan(input,format,a,b,c,d);});
    runtime.AddImport(sscanf_audit_marker,[&,sscanf_bridge](State &s){
      GameWorkerObservation::NativeEnvironment native;
      const auto caller=s.gpr.x30.qword,output=s.gpr.x2.qword;
      std::string format_label="<unread>";
      try{
        const auto format=recomp::ReadGuestString(s.gpr.x1.qword);
        format_label.clear();
        for(char ch:format){if(ch=='\n')format_label+="\\n";else if(ch=='\r')format_label+="\\r";else if(ch=='\\')format_label+="\\\\";else format_label+=ch;}
        sscanf_bridge(s);const auto count=static_cast<int32_t>(s.gpr.x0.dword);
        if(format=="%llu"){
          const auto value=count==1?Read<uint64_t>(output):0;
          std::fprintf(stderr,"Game bootstrap: original sscanf format=%s LR=%#llx result=%d assigned=%d value=%llu errno=%d\n",format_label.c_str(),(unsigned long long)caller,count,int(count==1),(unsigned long long)value,files.Error());
        }else{
          std::fprintf(stderr,"Game bootstrap: original sscanf format=%s LR=%#llx result=%d assigned=%d characterProfile=1; input/output contents not logged\n",format_label.c_str(),(unsigned long long)caller,count,std::max(count,0));
        }
      }catch(const std::exception &e){std::fprintf(stderr,"Game bootstrap: original sscanf format=%s LR=%#llx failed:%s\n",format_label.c_str(),(unsigned long long)caller,e.what());std::abort();}
      catch(...){std::fprintf(stderr,"Game bootstrap: original sscanf format=%s LR=%#llx failed:unknown host exception\n",format_label.c_str(),(unsigned long long)caller);std::abort();}
    });
    constexpr addr_t setlocale_audit_marker=0x7ffc68000010ULL;
    auto setlocale_bridge=recomp::MakeBridge<char*(int32_t,const char*)>([&libc](auto category,auto name){return libc.SetLocale(category,name);});
    runtime.AddImport(setlocale_audit_marker,[&,setlocale_bridge](State &s){
      const auto caller=s.gpr.x30.qword,name=s.gpr.x1.qword;const auto category=static_cast<int32_t>(s.gpr.x0.dword);
      try{
        // Keep the real API's category-before-name validation, including bad pointers.
        const bool name_read=name&&category>=0&&category<=12;
        const auto requested=name_read?recomp::ReadGuestString(name):std::string();
        setlocale_bridge(s);const auto returned=s.gpr.x0.qword?recomp::ReadGuestString(s.gpr.x0.qword):std::string();
        std::fprintf(stderr,"Game bootstrap: original setlocale category=%d name_null=%d name_read=%d requested=[%s] result_null=%d returned=[%s] LR=%#llx errno=%d\n",category,int(!name),int(name_read),requested.c_str(),int(!s.gpr.x0.qword),returned.c_str(),(unsigned long long)caller,files.Error());
      }catch(const std::exception &e){std::fprintf(stderr,"Game bootstrap: original setlocale LR=%#llx failed:%s\n",(unsigned long long)caller,e.what());std::abort();}
      catch(...){std::fprintf(stderr,"Game bootstrap: original setlocale LR=%#llx failed:unknown host exception\n",(unsigned long long)caller);std::abort();}
    });
    constexpr addr_t dup_audit_marker=0x7ffc68000018ULL;
    auto dup_bridge=recomp::MakeBridge<int32_t(int32_t)>([&files](auto descriptor){return files.Dup(descriptor);});
    runtime.AddImport(dup_audit_marker,[&,dup_bridge](State &s){
      const auto caller=s.gpr.x30.qword;const auto source=static_cast<int32_t>(s.gpr.x0.dword);
      try{
        dup_bridge(s);
        std::fprintf(stderr,"Game bootstrap: original dup source=%d result=%d LR=%#llx errno=%d\n",source,static_cast<int32_t>(s.gpr.x0.dword),(unsigned long long)caller,files.Error());
      }catch(const std::exception &e){std::fprintf(stderr,"Game bootstrap: original dup LR=%#llx failed:%s\n",(unsigned long long)caller,e.what());std::abort();}
      catch(...){std::fprintf(stderr,"Game bootstrap: original dup LR=%#llx failed:unknown host exception\n",(unsigned long long)caller);std::abort();}
    });
    tls_audit.Install(runtime,threads);
    if(!firebase_dll.empty()){
      FirebaseAppService::Config config;
      config.dll_path=firebase_dll.wstring();
      // The native desktop SDK cannot open the guest's APK jar URI. Stage the
      // original packaged desktop configuration without changing its bytes.
      const auto firebase_config_root=storage/"firebase-config";
      const auto firebase_config_file=firebase_config_root/"google-services-desktop.json";
      const auto firebase_config_bytes=context_config.apk_assets->Read("google-services-desktop.json");
      Require(!firebase_config_bytes.empty()&&firebase_config_bytes.size()<=1024*1024,"packaged Firebase desktop config has an unsupported size");
      fs::create_directories(firebase_config_root);
      {
        std::ofstream output(firebase_config_file,std::ios::binary|std::ios::trunc);
        Require(bool(output),"cannot open owned Firebase desktop config file");
        output.write(reinterpret_cast<const char*>(firebase_config_bytes.data()),static_cast<std::streamsize>(firebase_config_bytes.size()));
        output.close();Require(bool(output),"cannot write complete packaged Firebase desktop config");
      }
      {
        std::ifstream input(firebase_config_file,std::ios::binary);
        const std::vector<uint8_t> check((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
        Require(!input.bad()&&check==firebase_config_bytes,"staged Firebase desktop config differs from APK member");
      }
      config.resolve_config_path=[guest_apk=Utf8(context_config.source_dir),host_apk=apk.generic_u8string(),root=firebase_config_root.generic_u8string()](const std::string &path){
        if(path=="jar:file://"+guest_apk+"!/assets"||path=="jar:file:"+guest_apk+"!/assets"||
           path=="jar:file:///"+host_apk+"!/assets"||path=="jar:file://"+host_apk+"!/assets"||path=="jar:file:"+host_apk+"!/assets"){
          std::fputs("Windows Firebase SDK: resolving qualified APK streaming-assets directory to staged original desktop config\n",stderr);
          return root;
        }
        return path;
      };
      std::fprintf(stderr,"Windows Firebase SDK: staged original APK desktop config bytes=%zu; contents omitted\n",firebase_config_bytes.size());
      config.verify_library=[](const std::wstring &path,const std::string &expected){Require(Sha256(fs::path(path))==expected,"Windows Firebase DLL differs from the exact official SDK pin");};
      config.translated_entry=Translated;
      config.admitted_thread=[&]{return recomp::CurrentExecutionState()&&bool(process_signals.Lookup(threads.Self()))&&modules.ReferenceCount("libil2cpp.so")&&!il2cpp_ledger.finalized;};
      config.runner=[&](addr_t entry,const std::vector<addr_t>&arguments){
        Require(recomp::CurrentExecutionState()&&Translated(entry)&&bool(process_signals.Lookup(threads.Self()))&&modules.ReferenceCount("libil2cpp.so")&&!il2cpp_ledger.finalized,"Firebase reverse P/Invoke has no admitted live translated owner");
        return threads.Run(entry,arguments);
      };
      stage="genuine Windows Firebase SDK setup";firebase=std::make_unique<FirebaseAppService>(std::move(config));
      for(const auto &name:FirebaseAppService::Commands())runtime.AddImport(firebase->Marker(name),[&,name](State&s){
        const auto caller=s.gpr.x30.qword;
        firebase->Invoke(name,s);
        GameWorkerObservation::NativeEnvironment native;
        if(!GameQuietTrace()||name!="Firebase_App_CSharp_PollCallbacks")std::fprintf(stderr,"Windows Firebase SDK: original P/Invoke %s returned=%#llx LR=%#llx; genuine desktop SDK result, no Android JNI_OnLoad\n",name.c_str(),(unsigned long long)s.gpr.x0.qword,(unsigned long long)caller);
      });
      modules.Add(firebase->MakeModule());
      std::fputs("Windows Firebase SDK: exact official 9.6.0 DLL admitted as a typed platform service; original Android code and scene flow preserved\n",stderr);
    }
    if(!GameGrpcNative().empty()){
      bunker::windows::grpc_native::Service::Config grpc_config;
      grpc_config.library_path=fs::absolute(fs::u8path(GameGrpcNative())).u8string();
      if(GameServerProfile()){
        auto target="127.0.0.1:"+std::to_string(8003+bunker::ports::windowsOffset());
        grpc_config.target_rewrites["127.0.0.1:443"]=target;
        grpc_config.target_rewrites["127.0.0.1:38003"]=target;
      }
      if(const char*v=std::getenv("BUNKER_GRPC_TRACE");v&&std::strcmp(v,"0"))grpc_config.trace=true;
      stage="gRPC native extension setup";grpc_service=std::make_unique<bunker::windows::grpc_native::Service>(std::move(grpc_config));
      grpc_service->Install(runtime);modules.Add(grpc_service->MakeModule());
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
      // Grpc.Core handler threads park in bounded completion-queue slices;
      // each slice is a socket-style collector checkpoint.
      if(socket_wait_collector)grpc_service->SetWaitHooks(
        [&](const char *name,State &state,const std::function<void()> &invoke){socket_wait_collector->WithSocketWait(name,state,invoke);},
        [&](const bunker::windows::grpc_native::Service::WaitObservation &o){
          using P=bunker::windows::grpc_native::Service::WaitPhase;using D=GameSocketWaitContext::Phase;
          const D phase=o.phase==P::Copied?D::Copied:o.phase==P::BeforeNativeWait?D::BeforeNativeWait:o.phase==P::AfterNativeWait?D::AfterNativeWait:D::Returned;
          socket_wait_collector->Observe(GameSocketWaitContext::Observation{o.state,0,GameSocketWaitContext::Operation::GrpcQueue,phase,o.identity,o.retained,false,0});});
#endif
    }
    // FastAES.dll P/Invoke (master data decryption): libFastAES.so is not in
    // the translated catalog, so dlopen("FastAES") resolves to a host service.
    bunker::windows::fastaes_native::Service fastaes_service;
    fastaes_service.Install(runtime);modules.Add(fastaes_service.MakeModule());
    avpro_video.Install(runtime);modules.Add(avpro_video.MakeModule());
    bunker::windows::opensles::Service sound([&](addr_t entry,const std::vector<addr_t> &args){return threads.Run(entry,args);},[&]{threads.ReleaseCurrent();});
    sound.Install(runtime);modules.Add(sound.MakeModule());
    // setpriority: the audio threads ask for a higher priority. Accepted, not applied.
    const addr_t setpriority_marker=0x7fe900000000ULL;runtime.AddImport(setpriority_marker,[](State &s){s.gpr.x0.qword=0;});
    // __system_property_get(name, value): no Android system properties exist here; every one reads as empty.
    const addr_t system_property_marker=0x7fe900000008ULL;runtime.AddImport(system_property_marker,[](State &s){const char empty=0;if(s.gpr.x1.qword)recomp::WriteGuestMemory(s.gpr.x1.qword,&empty,1);s.gpr.x0.qword=0;});
    // strerror_r(error, buffer, size), the XSI form bionic exports: writes "error N" and returns 0.
    const addr_t strerror_marker=0x7fe900000010ULL;runtime.AddImport(strerror_marker,[](State &s){
      char text[32];const int n=std::snprintf(text,sizeof text,"error %d",int(s.gpr.x0.dword));const size_t room=s.gpr.x2.qword;
      if(s.gpr.x1.qword&&room){const size_t copy=std::min<size_t>(size_t(n),room-1);recomp::WriteGuestMemory(s.gpr.x1.qword,text,copy);const char end=0;recomp::WriteGuestMemory(s.gpr.x1.qword+copy,&end,1);}
      s.gpr.x0.qword=0;});
    bunker::windows::fmod_audio::Service fmod_audio(java,[&](addr_t entry,const std::vector<addr_t> &args){return threads.Run(entry,args);},[&]{threads.ReleaseCurrent();});
    modules.Install(module_bridges);
    for(const char *name:{"dlopen","dlsym","dlclose","dlerror","dladdr","dl_iterate_phdr"}){auto marker=modules.Marker(name);runtime.AddImport(marker,[&,name,marker](State &s){try{
      const auto argument=s.gpr.x0.qword,output=s.gpr.x1.qword,caller=s.gpr.x30.qword;
      std::string text;
      if(!GameQuietTrace()){
        if(!std::strcmp(name,"dlopen"))text=argument?recomp::ReadGuestString(argument):"<null>";
        if(!std::strcmp(name,"dlsym"))text=output?recomp::ReadGuestString(output):"<null>";
      }
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
      GameCollectorDelivery::Opaque opaque{bool(collector)};
      GameUnityMainCollector::Opaque main_opaque{bool(main_collector)};
      GameUnityWorkerCollector::Opaque worker_opaque{bool(worker_collector)};
#endif
      module_bridges.Dispatch(s,marker,nullptr);
      if(GameQuietTrace())return;
      std::fprintf(stderr,"Game bootstrap: original %s argument=%#llx name=%s result=%#llx LR=%#llx Unity refs=%zu IL2CPP refs=%zu\n",name,(unsigned long long)argument,text.c_str(),(unsigned long long)s.gpr.x0.qword,(unsigned long long)caller,modules.ReferenceCount("libunity.so"),modules.ReferenceCount("libil2cpp.so"));
      if(!std::strcmp(name,"dladdr")&&s.gpr.x0.dword){const auto info=Read<AndroidDlInfo>(output);const auto file=info.filename?recomp::ReadGuestString(info.filename):std::string();const auto symbol=info.symbol_name?recomp::ReadGuestString(info.symbol_name):std::string();std::fprintf(stderr,"Game bootstrap: original dladdr info filename=%s base=%#llx symbol=%s address=%#llx; registered ELF metadata, not native PE ownership\n",file.c_str(),(unsigned long long)info.base,symbol.c_str(),(unsigned long long)info.symbol_address);}
    }catch(const std::exception &e){std::fprintf(stderr,"Game bootstrap: module bridge %s failed:%s\n",name,e.what());std::abort();}});}
    if(init_jni)InstallDiagnosticJni(java,runtime);else java.Install(runtime);
#ifdef REINCOMP_EMBEDDED_PLAYER
    RegisterPlayerJni(&java);
#endif
    zlib.Install(runtime);
    AndroidChoreographer::Config choreographer_config;
    choreographer_config.prepare_worker=[&](const std::u16string&)->AndroidChoreographer::Config::WorkerLease{
      GameWorkerObservation::NativeEnvironment native;
      auto parent=std::make_shared<GameChoreographerParent>();parent->token=threads.Self();
      Require(bool(process_signals.Lookup(parent->token)),"Choreographer creator has no real signal lifetime");
      Require(!process_signals.Mask(parent->token,0,nullptr,&parent->mask),"cannot capture Choreographer creator signal mask");return parent;
    };
    auto leave_choreographer_worker=[&](const AndroidChoreographer::Config::WorkerLease &lease){
      GameWorkerObservation::NativeEnvironment native;
      auto &worker=game_choreographer_worker;
      Require(worker.token&&worker.parent==lease.get(),"Choreographer worker lease/lifetime mismatch");
      const auto token=worker.token;std::exception_ptr failure;
      try{
        if(tls_audit.PendingModuleCallbacks(threads))Require(!unity_ledger.finalized&&!il2cpp_ledger.finalized,"Choreographer TLS callback outlived its original module");
        Require(!threads.ReleaseCurrent(),"Choreographer translated stack/TLS retirement failed");
      }catch(...){failure=std::current_exception();}
      try{if(java.EnvAddress()){State state{};state.gpr.x0.qword=java.VmAddress();java.InvokeVm(5,state);Require(int32_t(state.gpr.x0.dword)==bunker::jni::kOk,"Choreographer VM detach failed");}}catch(...){if(!failure)failure=std::current_exception();}
      if(jumps)jumps->DropThread(token,token);
      if(worker.registered&&process_signals.DropThread(token)&&!failure)failure=std::make_exception_ptr(std::runtime_error("Choreographer signal lifetime retirement failed"));
      worker={};
      std::fprintf(stderr,"Game Choreographer worker: retired token=%#llx native_tid=%lu; original TLS before VM detach\n",(unsigned long long)token,GetCurrentThreadId());
      if(failure)std::rethrow_exception(failure);
    };
    choreographer_config.enter_worker=[&,leave_choreographer_worker](const std::u16string&,const AndroidChoreographer::Config::WorkerLease &lease){
      GameWorkerObservation::NativeEnvironment native;
      auto parent=std::static_pointer_cast<GameChoreographerParent>(lease);
      Require(bool(parent)&&!game_choreographer_worker.token,"invalid or repeated Choreographer worker entry");
      auto &worker=game_choreographer_worker;worker.token=threads.Self();worker.parent=lease.get();
      try{
        Require(!process_signals.RegisterThread(worker.token,parent->token),"Choreographer signal registration failed");worker.registered=true;
        Require(!process_signals.BindCurrent(worker.token)&&!process_signals.Mask(worker.token,2,&parent->mask,nullptr),"Choreographer signal inheritance failed");
        addr_t environment=0;State state{};state.gpr.x0.qword=java.VmAddress();state.gpr.x1.qword=reinterpret_cast<addr_t>(&environment);java.InvokeVm(4,state);
        Require(int32_t(state.gpr.x0.dword)==bunker::jni::kOk&&environment==java.EnvAddress()&&environment,"Choreographer VM attach failed");
        std::fprintf(stderr,"Game Choreographer worker: entered token=%#llx parent=%#llx native_tid=%lu mask=%#llx; actual external thread, no fabricated collector record\n",(unsigned long long)worker.token,(unsigned long long)parent->token,GetCurrentThreadId(),(unsigned long long)parent->mask);
      }catch(...){auto failure=std::current_exception();leave_choreographer_worker(lease);std::rethrow_exception(failure);}
    };
    choreographer_config.exit_worker=leave_choreographer_worker;
    choreographer_config.read_frame_clock=[&]{
      GameWorkerObservation::NativeEnvironment native;
      auto ratio=choreographer_refresh_ratio.load(std::memory_order_relaxed);
      if(!ratio){(void)displays.Displays();ratio=choreographer_refresh_ratio.load(std::memory_order_relaxed);}
      Require(ratio&&uint32_t(ratio)&&uint32_t(ratio>>32),"no observed Windows primary refresh ratio for Choreographer port clock");
      const auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
      return AndroidChoreographer::ClockSample{now,uint32_t(ratio>>32),uint32_t(ratio)};
    };
    choreographer_config.observer=[](const AndroidChoreographer::Event &event){if(GameQuietTrace())return;GameWorkerObservation::NativeEnvironment native;std::fprintf(stderr,"Game Choreographer: operation=%s native_tid=%lu what=%d frameTimeNs=%lld nameUnits=%zu; asynchronous native-port clock, not Android/DWM vsync\n",event.operation.c_str(),GetCurrentThreadId(),event.what,(long long)event.frame_time_ns,event.thread_name.size());};
    choreographer_config.failure=[](std::exception_ptr failure){try{std::rethrow_exception(failure);}catch(const std::exception&e){std::fprintf(stderr,"Game Choreographer worker failed:%s\n",e.what());}catch(...){std::fputs("Game Choreographer worker failed:unknown exception\n",stderr);}std::abort();};
    stage="asynchronous Choreographer setup";choreographer=std::make_unique<AndroidChoreographer>(java,std::move(choreographer_config));
    JniInterfaceProxy::Config proxy_config;
    proxy_config.run_native=[&](addr_t entry,const std::vector<addr_t> &arguments){Require(runner&&Translated(entry)&&modules.ReferenceCount("libunity.so")&&!unity_ledger.finalized,"proxy callback has no live original Unity native owner");return runner->Run(entry,arguments);};
    stage="interface proxy setup";interface_proxies=std::make_unique<JniInterfaceProxy>(java,std::move(proxy_config));
    ReflectionHelper::Config reflection_config;
    reflection_config.run_native=[&](addr_t entry,const std::vector<addr_t> &arguments){Require(runner&&Translated(entry)&&modules.ReferenceCount("libunity.so")&&!unity_ledger.finalized,"ReflectionHelper callback has no live original Unity native owner");return runner->Run(entry,arguments);};
    stage="ReflectionHelper setup";reflection_helper=std::make_unique<ReflectionHelper>(java,reflection_launch_epoch,std::move(reflection_config));
    auto no_delivery=[](const SignalHost::Delivery&)->std::optional<uint64_t>{throw std::runtime_error("frame profile has no qualified target signal delivery binding");};
    SignalHost::Boundary signal_boundary=no_delivery;
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
    SignalHost::SenderSnapshot sender;
    if(collector){signal_boundary=[&](const auto &delivery){return main_collector->Owns(delivery.token)?main_collector->Deliver(delivery):worker_collector->Owns(delivery.token)?worker_collector->Deliver(delivery):collector->Deliver(delivery);};sender=[&]{return SignalHost::SenderIdentity{libc.ProcessId(),GameCollectorDelivery::kVirtualRealUid};};}
    process_signals.Install(signal_bridges,current_signal_thread,signal_boundary,std::move(sender));
#else
    process_signals.Install(signal_bridges,current_signal_thread,signal_boundary);
#endif
    for(const char *name:{"sigaction","pthread_sigmask","sigprocmask","pthread_kill","sigsuspend","sigpending","sigemptyset","sigfillset","sigaddset","sigdelset","sigismember","sigaltstack"}){auto marker=process_signals.Marker(name);runtime.AddImport(marker,[&,name,marker](State &s){try{if(!std::strcmp(name,"pthread_kill")&&s.gpr.x1.dword){
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
      if(collector){GameWorkerObservation::NativeEnvironment native;const auto target=s.gpr.x0.qword;const auto signal=s.gpr.x1.dword;
        if(!GameQuietTrace()){
          std::fprintf(stderr,"Game GC: nonzero send ENTRY caller_native_tid=%lu observed_worker_token=%#llx signal=%u target=%#llx PC=%#llx SP=%#llx FP=%#llx LR=%#llx x19=%#llx x20=%#llx x21=%#llx x22=%#llx x26=%#llx x27=%#llx x28=%#llx; arguments only before ownership validation\n",GetCurrentThreadId(),(unsigned long long)GameWorkerObservation::Token(),signal,(unsigned long long)target,(unsigned long long)s.gpr.pc.qword,(unsigned long long)s.gpr.sp.qword,(unsigned long long)s.gpr.x29.qword,(unsigned long long)s.gpr.x30.qword,(unsigned long long)s.gpr.x19.qword,(unsigned long long)s.gpr.x20.qword,(unsigned long long)s.gpr.x21.qword,(unsigned long long)s.gpr.x22.qword,(unsigned long long)s.gpr.x26.qword,(unsigned long long)s.gpr.x27.qword,(unsigned long long)s.gpr.x28.qword);
          std::fflush(stderr);
        }
        if(main_collector->Owns(target))main_collector->Send(s,[&]{signal_bridges.Dispatch(s,marker,nullptr);});else if(worker_collector->Owns(target))worker_collector->Send(s,[&]{signal_bridges.Dispatch(s,marker,nullptr);});else collector->Send(s,[&]{signal_bridges.Dispatch(s,marker,nullptr);});
        if(!GameQuietTrace())std::fprintf(stderr,"Game GC: actual pthread_kill returned signal=%u target=%#llx status=%d; no queue inference from pre-call witness\n",signal,(unsigned long long)target,int32_t(s.gpr.x0.dword));
        return;}else
#endif
      {std::fprintf(stderr,"Game bootstrap: unsupported nonzero signal delivery signal=%u target=%#llx guestPC=%#llx LR=%#llx\n",s.gpr.x1.dword,(unsigned long long)s.gpr.x0.qword,(unsigned long long)s.gpr.pc.qword,(unsigned long long)s.gpr.x30.qword);std::abort();}}
      const auto input=s.gpr.x0.qword,output=s.gpr.x1.qword;
      signal_bridges.Dispatch(s,marker,nullptr);
      if(!std::strcmp(name,"sigaltstack")){
        const auto stack=process_signals.AlternateStack(current_signal_thread()).configured;
        std::fprintf(stderr,"Game bootstrap: original sigaltstack new=%#llx old=%#llx SP=%#llx LR=%#llx result=%d configured=%#llx flags=%u size=%llu\n",(unsigned long long)input,(unsigned long long)output,(unsigned long long)s.gpr.sp.qword,(unsigned long long)s.gpr.x30.qword,int32_t(s.gpr.x0.dword),(unsigned long long)stack.pointer,uint32_t(stack.flags),(unsigned long long)stack.size);
      }
    }catch(const std::exception &e){std::fprintf(stderr,"Game bootstrap: signal bridge %s failed:%s\n",name,e.what());std::abort();}});}
    semaphores.Install(runtime,current_signal_thread,signal_boundary);
    // Collector delivery remains limited to its qualified condition wait.
    sleeps.Install(sleep_bridges);
    const auto usleep_marker=sleeps.Marker("usleep");
    runtime.AddImport(usleep_marker,[&,usleep_marker](State &s){
      const auto microseconds=s.gpr.x0.dword;const auto caller=s.gpr.x30.qword;
      try{
        sleep_bridges.Dispatch(s,usleep_marker,nullptr);
        if(!GameQuietTrace())std::fprintf(stderr,"Game bootstrap: original usleep microseconds=%u LR=%#llx result=%d errno=%d\n",microseconds,(unsigned long long)caller,int32_t(s.gpr.x0.dword),files.Error());
      }catch(const std::exception &e){std::fprintf(stderr,"Game bootstrap: original usleep LR=%#llx failed:%s\n",(unsigned long long)caller,e.what());std::abort();}
      catch(...){std::fprintf(stderr,"Game bootstrap: original usleep LR=%#llx failed:unknown host exception\n",(unsigned long long)caller);std::abort();}
    });
    nanosleeps.Install(sleep_bridges);
    const auto nanosleep_marker=nanosleeps.Marker("nanosleep");
    runtime.AddImport(nanosleep_marker,[&,nanosleep_marker](State &s){
      const auto request=s.gpr.x0.qword,remaining=s.gpr.x1.qword,caller=s.gpr.x30.qword;
      try{
        if(!GameQuietTrace()){GameWorkerObservation::NativeEnvironment native;
          std::fprintf(stderr,"Game bootstrap: original nanosleep ENTER caller_native_tid=%lu request=%#llx remaining=%#llx LR=%#llx\n",GetCurrentThreadId(),(unsigned long long)request,(unsigned long long)remaining,(unsigned long long)caller);}
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
        if(main_collector&&main_collector->Owns(threads.Self())&&GameUnityMainCollector::NanoSleepSite(s))main_collector->WithNanoSleep(s,[&]{sleep_bridges.Dispatch(s,nanosleep_marker,nullptr);});else
        if(collector&&GameCollectorDelivery::BackoffSleepSite(s)&&collector->BackoffOwner(threads.Self()))collector->WithBackoffSleep(s,[&]{sleep_bridges.Dispatch(s,nanosleep_marker,nullptr);});else
#endif
        sleep_bridges.Dispatch(s,nanosleep_marker,nullptr);
        if(!GameQuietTrace()){GameWorkerObservation::NativeEnvironment native;
          std::fprintf(stderr,"Game bootstrap: original nanosleep RETURN caller_native_tid=%lu request=%#llx remaining=%#llx LR=%#llx result=%d errno=%d\n",GetCurrentThreadId(),(unsigned long long)request,(unsigned long long)remaining,(unsigned long long)caller,int32_t(s.gpr.x0.dword),files.Error());}
      }catch(const std::exception &e){std::fprintf(stderr,"Game bootstrap: original nanosleep LR=%#llx failed:%s\n",(unsigned long long)caller,e.what());std::abort();}
      catch(...){std::fprintf(stderr,"Game bootstrap: original nanosleep LR=%#llx failed:unknown host exception\n",(unsigned long long)caller);std::abort();}
    });
    // The forwarding import below owns the complete original-call witness.
    futexes.Install(futex_bridges,[](const State&,uint64_t){});
    affinities.Install(affinity_bridges);
    const auto getaffinity_marker=affinities.Marker("sched_getaffinity");
    runtime.AddImport(getaffinity_marker,[&,getaffinity_marker](State &s){
      const auto caller=s.gpr.x30.qword,pointer=s.gpr.x2.qword,size=s.gpr.x1.qword;
      const auto pid=int32_t(s.gpr.x0.dword);
      try{
        {GameWorkerObservation::NativeEnvironment native;
          std::fprintf(stderr,"Game bootstrap: original sched_getaffinity ENTER caller_native_tid=%lu pid=%d size=%llu pointer=%#llx LR=%#llx\n",GetCurrentThreadId(),pid,(unsigned long long)size,(unsigned long long)pointer,(unsigned long long)caller);
          std::fflush(stderr);
        }
        affinity_bridges.Dispatch(s,getaffinity_marker,nullptr);
        {GameWorkerObservation::NativeEnvironment native;
          std::fprintf(stderr,"Game bootstrap: original sched_getaffinity RETURN pid=%d LR=%#llx result=%d errno=%d\n",pid,(unsigned long long)caller,int32_t(s.gpr.x0.dword),files.Error());
          std::fflush(stderr);
        }
      }catch(const std::exception &e){std::fprintf(stderr,"Game bootstrap: original sched_getaffinity failed:%s\n",e.what());std::abort();}
      catch(...){std::fprintf(stderr,"Game bootstrap: original sched_getaffinity failed:unknown host exception\n");std::abort();}
    });
    const auto syscall_marker=futexes.Marker("syscall");
    const auto setaffinity_marker=affinities.Marker("sched_setaffinity");
    runtime.AddImport(setaffinity_marker,[&,setaffinity_marker](State &s){
      const auto caller=s.gpr.x30.qword,pointer=s.gpr.x2.qword,size=s.gpr.x1.qword;
      const auto pid=int32_t(s.gpr.x0.dword);
      try{
        {GameWorkerObservation::NativeEnvironment native;
          std::fprintf(stderr,"Game bootstrap: original sched_setaffinity ENTER caller_native_tid=%lu pid=%d size=%llu pointer=%#llx LR=%#llx\n",GetCurrentThreadId(),pid,(unsigned long long)size,(unsigned long long)pointer,(unsigned long long)caller);
          std::fflush(stderr);
        }
        affinity_bridges.Dispatch(s,setaffinity_marker,nullptr);
        {GameWorkerObservation::NativeEnvironment native;
          std::fprintf(stderr,"Game bootstrap: original sched_setaffinity RETURN pid=%d LR=%#llx result=%d errno=%d\n",pid,(unsigned long long)caller,int32_t(s.gpr.x0.dword),files.Error());
          std::fflush(stderr);
        }
      }catch(const std::exception &e){std::fprintf(stderr,"Game bootstrap: original sched_setaffinity failed:%s\n",e.what());std::abort();}
      catch(...){std::fprintf(stderr,"Game bootstrap: original sched_setaffinity failed:unknown host exception\n");std::abort();}
    });
    runtime.AddImport(syscall_marker,[&,syscall_marker](State &s){
      try{
        uint64_t token=0;const auto caller=s.gpr.x30.qword,sp=s.gpr.sp.qword;
        const std::array<uint64_t,7> args={s.gpr.x0.qword,s.gpr.x1.qword,s.gpr.x2.qword,s.gpr.x3.qword,s.gpr.x4.qword,s.gpr.x5.qword,s.gpr.x6.qword};
        {GameWorkerObservation::NativeEnvironment native;
        token=current_signal_thread();
        if(!GameQuietTrace())std::fprintf(stderr,"Game bootstrap: original syscall ENTER token=%#llx native_tid=%lu PC=%#llx SP=%#llx LR=%#llx x0=%#llx x1=%#llx x2=%#llx x3=%#llx x4=%#llx x5=%#llx x6=%#llx; arguments only, not a blocked-state record\n",(unsigned long long)token,GetCurrentThreadId(),(unsigned long long)s.gpr.pc.qword,(unsigned long long)sp,(unsigned long long)caller,(unsigned long long)args[0],(unsigned long long)args[1],(unsigned long long)args[2],(unsigned long long)args[3],(unsigned long long)args[4],(unsigned long long)args[5],(unsigned long long)args[6]);
        if(!GameQuietTrace()&&caller==0x702ad510&&args[0]==98&&uint32_t(args[2])==0x80){
          AndroidThreadAttr attr{};const auto status=threads.GetAttr(token,&attr);
          const bool owned=!status&&attr.stack_base&&attr.stack_size<=UINT64_MAX-attr.stack_base;
          const auto high=owned?attr.stack_base+attr.stack_size:0;
          const bool readable=owned&&!(sp&15)&&sp>=attr.stack_base&&sp<=high&&high-sp>=32;
          const auto saved_caller=readable?Read<addr_t>(sp+24):0;
          std::fprintf(stderr,"Game bootstrap: original syscall callsite token=%#llx attr_status=%d stack_low=%#llx stack_high=%#llx saved_caller_read=%d saved_caller=%#llx; exact original 32-byte frame only\n",(unsigned long long)token,status,(unsigned long long)attr.stack_base,(unsigned long long)high,int(readable),(unsigned long long)saved_caller);
        }
        if(!GameQuietTrace())std::fflush(stderr);
        }
        if(uint32_t(args[0])==122)affinity_bridges.Dispatch(s,affinities.Marker("syscall"),nullptr);
        else {
          const bool profile_wait=WaitProfileEnabled();std::string profile_key;std::chrono::steady_clock::time_point profile_start;
          if(profile_wait){profile_key=WaitProfileKey("syscall"+std::to_string(uint32_t(args[0]))+"/op"+std::to_string(uint32_t(args[2])&0x7f),s);profile_start=std::chrono::steady_clock::now();}
          struct ProfileScope{bool on;std::string &key;std::chrono::steady_clock::time_point &start;~ProfileScope(){if(on)WaitProfileRecord(std::move(key),uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count()));}}profile_scope{profile_wait,profile_key,profile_start};
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
          if(worker_collector&&worker_collector->Owns(token))worker_collector->WithSyscall(s,[&]{futex_bridges.Dispatch(s,syscall_marker,nullptr);});else
          if(main_collector&&main_collector->Owns(token))main_collector->WithSyscall(s,[&]{futex_bridges.Dispatch(s,syscall_marker,nullptr);});else
#endif
          futex_bridges.Dispatch(s,syscall_marker,nullptr);
        }
        if(!GameQuietTrace()){GameWorkerObservation::NativeEnvironment native;
        std::fprintf(stderr,"Game bootstrap: original syscall RETURN token=%#llx LR=%#llx result=%lld errno=%d\n",(unsigned long long)token,(unsigned long long)caller,(long long)int64_t(s.gpr.x0.qword),files.Error());
        }
      }catch(const std::exception &e){std::fprintf(stderr,"Game bootstrap: original syscall failed:%s\n",e.what());std::abort();}
      catch(...){std::fprintf(stderr,"Game bootstrap: original syscall failed:unknown host exception\n");std::abort();}
    });
    priorities.Install(priority_bridges);
    const auto getpriority_marker=priorities.Marker("getpriority");
    runtime.AddImport(getpriority_marker,[&,getpriority_marker](State &s){
      const auto which=s.gpr.x0.dword,who=s.gpr.x1.dword;const auto caller=s.gpr.x30.qword;
      try{
        {GameWorkerObservation::NativeEnvironment native;
          std::fprintf(stderr,"Game bootstrap: original getpriority ENTER caller_native_tid=%lu which=%u who=%u LR=%#llx\n",GetCurrentThreadId(),which,who,(unsigned long long)caller);
          std::fflush(stderr);
        }
        priority_bridges.Dispatch(s,getpriority_marker,nullptr);
        {GameWorkerObservation::NativeEnvironment native;
          std::fprintf(stderr,"Game bootstrap: original getpriority RETURN who=%u LR=%#llx result=%d errno=%d\n",who,(unsigned long long)caller,int32_t(s.gpr.x0.dword),files.Error());
        }
      }catch(const std::exception &e){std::fprintf(stderr,"Game bootstrap: original getpriority failed:%s\n",e.what());std::abort();}
      catch(...){std::fprintf(stderr,"Game bootstrap: original getpriority failed:unknown host exception\n");std::abort();}
    });
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
    constexpr addr_t ack_audit=0x7ffc60000100ULL;
    if(collector)runtime.AddImport(ack_audit,[&](State &s){try{GameWorkerObservation::NativeEnvironment native;if(main_collector->Owns(threads.Self()))main_collector->BeforePost(s);else if(worker_collector->Owns(threads.Self()))worker_collector->BeforePost(s);else collector->BeforePost(s);runtime.Dispatch(s,semaphores.Marker("sem_post"),nullptr);if(main_collector->Owns(threads.Self()))main_collector->AfterPost(s);else if(worker_collector->Owns(threads.Self()))worker_collector->AfterPost(s);else collector->AfterPost(s);}catch(const std::exception&e){std::fprintf(stderr,"Game GC: original post observation failed:%s\n",e.what());std::abort();}catch(...){std::abort();}});
#endif
    for(const char *name:{"pthread_cond_wait","pthread_cond_timedwait","pthread_mutex_lock","pthread_rwlock_rdlock","pthread_rwlock_wrlock","pthread_once","pthread_join","sem_wait","sem_timedwait","sigsuspend"}){
      const std::string identity=name;
      const auto actual=identity=="sigsuspend"?process_signals.Marker(identity):identity=="sem_wait"||identity=="sem_timedwait"?semaphores.Marker(identity):threads.Marker(identity);
      const auto observed=tls_audit.Marker(identity);
      runtime.AddImport(observed,[&,identity,actual](State &s){
        bool record_wait=!GameQuietTrace();
        if(first_frame && record_wait){
          GameWorkerObservation::NativeEnvironment native;
          // Per-thread diagnostic counters contain no guest addresses. Bound
          // repeated fast-lock output while retaining complete sampled pairs.
          static thread_local std::array<uint64_t,4> calls{};
          const size_t slot=identity=="pthread_mutex_lock"?0:identity=="pthread_rwlock_rdlock"?1:identity=="pthread_rwlock_wrlock"?2:identity=="pthread_once"?3:4;
          if(slot<calls.size()&&GameWorkerObservation::Token()){
            auto &seen=calls[slot];if(seen!=UINT64_MAX)++seen;
            record_wait=seen<=64||seen%4096==0;
            if(record_wait)std::fprintf(stderr,"First frame: sampled worker wait import=%s native_tid=%lu calls=%llu; first64 then every4096, observations only\n",identity.c_str(),GetCurrentThreadId(),(unsigned long long)seen);
          }
        }
        // UnityMain is an external Pthreads lifetime, not a prepared worker.
        // Its own diagnostic must not rely on GameWorkerObservation::Token.
        const bool observe_main=first_frame&&!GameQuietTrace()&&ObserveFourthFrameThread();
        static thread_local uint64_t main_wait_sequence=0;
        const auto main_sequence=observe_main?++main_wait_sequence:0;
        if(observe_main)ObserveFourthFrameWait("ENTRY",identity.c_str(),main_sequence,s);
        const auto token=record_wait?GameWorkerObservation::Wait("ENTER",identity.c_str(),s):0;
        const bool profile_wait=WaitProfileEnabled();std::string profile_key;std::chrono::steady_clock::time_point profile_start;
        if(profile_wait){profile_key=WaitProfileKey(identity,s);profile_start=std::chrono::steady_clock::now();}
        struct ProfileScope{bool on;std::string &key;std::chrono::steady_clock::time_point &start;~ProfileScope(){if(on)WaitProfileRecord(std::move(key),uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count()));}}profile_scope{profile_wait,profile_key,profile_start};
        // Dispatch the existing checked provider. No nested Runtime::Run or
        // new guest State is introduced; original arguments/results survive.
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
        if(main_collector&&main_collector->OwnsCurrentThread())main_collector->WithWait(identity,s,[&]{runtime.Dispatch(s,actual,nullptr);});else
        if(worker_collector&&worker_collector->OwnsCurrentThread())worker_collector->WithWait(identity,s,[&]{runtime.Dispatch(s,actual,nullptr);});else
        if(collector)collector->WithWait(identity,s,[&]{runtime.Dispatch(s,actual,nullptr);});else
#endif
        runtime.Dispatch(s,actual,nullptr);
        if(token)GameWorkerObservation::Wait("RETURN",identity.c_str(),s);
        if(observe_main)ObserveFourthFrameWait("RETURN",identity.c_str(),main_sequence,s);
      });
    }
    if(jumps){automatic_environments.jumps=jumps.get();font_environments.jumps=jumps.get();decode_environments.jumps=jumps.get();jumps->Install(jump_bridges);for(const char *name:{"setjmp","_setjmp","sigsetjmp","longjmp","_longjmp","siglongjmp"}){auto marker=jumps->Marker(name);runtime.AddImport(marker,[&,name,marker](State &s){try{
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
      if(collector)collector->BeforeNonlocal();
      if(main_collector)main_collector->BeforeNonlocal();
      if(worker_collector)worker_collector->BeforeNonlocal();
#endif
      if(!std::strcmp(name,"setjmp")){const auto token=current_signal_thread();automatic_environments.BeforeSetjmp(s,token);font_environments.BeforeSetjmp(s,token);decode_environments.BeforeSetjmp(s,token);}jump_bridges.Dispatch(s,marker,nullptr);}catch(const std::exception &e){std::fprintf(stderr,"Game bootstrap: nonlocal bridge %s failed:%s\n",name,e.what());std::abort();}});}}
    if(graphics && GameQuietTrace()){
      graphics->Install(runtime);
    }else if(graphics){
      graphics->Install(gl_bridges);
      for(const auto &name:GlHost::Commands()){
        auto marker=graphics->Marker(name);runtime.AddImport(marker,[&,marker,name](State &s){
          if(!GameQuietTrace())std::fprintf(stderr,"Unity bootstrap: guest %s x0=%#llx x1=%#llx return=%#llx\n",name.c_str(),static_cast<unsigned long long>(s.gpr.x0.qword),static_cast<unsigned long long>(s.gpr.x1.qword),static_cast<unsigned long long>(s.gpr.x30.qword));gl_bridges.Dispatch(s,marker,nullptr);
        });
      }
    }
    if(egl)egl->Install(egl_bridges);
    if(graphics)modules.Add(MakeGlesServiceModule(*graphics));
    if(native_windows){
      // Forward the original checked provider bridges without adding a nested
      // Runtime::Run/ExecutionContext. Observe actual guest imports, not our
      // separate host calls used to verify the HWND's initial geometry.
      native_windows->Install(window_bridges);
      for(const char *name:{"ANativeWindow_fromSurface","ANativeWindow_acquire","ANativeWindow_release","ANativeWindow_getWidth","ANativeWindow_getHeight","ANativeWindow_setBuffersGeometry"}){
        const auto marker=native_windows->Marker(name);
        runtime.AddImport(marker,[&,marker,name](State &s){
          {std::lock_guard<std::mutex> lock(window_call_mutex);++window_calls[name];}
          std::fprintf(stderr,"Unity bootstrap: guest %s x0=%#llx x1=%#llx return=%#llx\n",name,static_cast<unsigned long long>(s.gpr.x0.qword),static_cast<unsigned long long>(s.gpr.x1.qword),static_cast<unsigned long long>(s.gpr.x30.qword));
          window_bridges.Dispatch(s,marker,nullptr);
          std::fprintf(stderr,"Unity bootstrap: guest %s completed x0=%#llx\n",name,static_cast<unsigned long long>(s.gpr.x0.qword));
        });
      }
    }
    size_t missing=0;std::set<addr_t> forwarded_egl;
    auto load=[&](const char *module_name,const auto &imports,const fs::path &path,addr_t base,size_t span,elf_image &target){
      resolved.clear();size_t provided=0,weak=0;
      for(const auto &import:imports){
        std::string name=import.name;addr_t address=tls_audit.Marker(name);
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
        if(collector&&name=="sem_post")address=ack_audit;
#endif
        if(!address&&name=="setpriority")address=setpriority_marker;if(!address&&name=="__system_property_get")address=system_property_marker;if(!address&&name=="strerror_r")address=strerror_marker;if(!address&&name=="mmap")address=mmap_audit_marker;if(!address&&name=="sscanf")address=sscanf_audit_marker;if(!address&&name=="setlocale")address=setlocale_audit_marker;if(!address&&name=="dup")address=dup_audit_marker;if(!address&&name=="sched_getaffinity")address=getaffinity_marker;if(!address&&name=="sched_setaffinity")address=setaffinity_marker;if(!address)address=Try(process_signals,name);if(!address)address=Try(semaphores,name);
        if(!address)for(addr_t candidate:{Try(files,name),Try(sockets,name),Try(threads,name),Try(libc,name),Try(sleeps,name),Try(nanosleeps,name),Try(futexes,name),Try(priorities,name),Try(logging,name),Try(modules,name),Try(zlib,name),Try(sensors,name),native_windows?Try(*native_windows,name):0,jumps?Try(*jumps,name):0,egl?Try(*egl,name):0,graphics?TryGlImport(*graphics,name):0}){
          if(candidate){if(address&&address!=candidate)throw std::runtime_error("ambiguous import provider: "+name);address=candidate;}
        }
        if(address){++provided;if(egl&&Try(*egl,name)==address&&forwarded_egl.insert(address).second)runtime.AddImport(address,[&,address,name](State &s){if(!GameQuietTrace())std::fprintf(stderr,"Game bootstrap: guest %s x0=%#llx x1=%#llx x2=%#llx LR=%#llx\n",name.c_str(),(unsigned long long)s.gpr.x0.qword,(unsigned long long)s.gpr.x1.qword,(unsigned long long)s.gpr.x2.qword,(unsigned long long)s.gpr.x30.qword);egl_bridges.Dispatch(s,address,nullptr);});}
        else if(import.weak){++weak;}
        else{Require(!import.object,"missing strong data import; fabricated storage is forbidden");address=0x7ffc00000000ULL+missing++*8;runtime.AddImport(address,[name,module_name](State &s){std::fprintf(stderr,"Game bootstrap: unsupported import module=%s name=%s PC=%#llx LR=%#llx x0=%#llx x1=%#llx stage=%s\n",module_name,name.c_str(),(unsigned long long)s.gpr.pc.qword,(unsigned long long)s.gpr.x30.qword,(unsigned long long)s.gpr.x0.qword,(unsigned long long)s.gpr.x1.qword,stage);std::fflush(stderr);std::abort();});}
        Require(resolved.emplace(name,address).second,"duplicate import inventory entry");
      }
      void *reservation=base==kUnityBase ? unityReservation : il2cppReservation;
      auto name=path.u8string();
      int result=reservation ? elf_load_bionic_reserved(name.c_str(),reservation,span,Resolve,&target)
                             : elf_load_bionic(name.c_str(),base,Resolve,&target);
      Require(!result&&target.span==span,"verified Bionic module data-image/span load failed");
      std::fprintf(stderr,"Game bootstrap: mapped %s imports=%zu real/%zu weak-zero; cumulative failure markers=%zu; constructors remain lazy\n",module_name,provided,weak,missing);
    };
    elf_image image{},il2cpp_image_data{};
    load("libunity.so",unity_image::kGameImports,library,kUnityBase,kUnityImageSpan,image);
    load("libil2cpp.so",il2cpp_image::kGameImports,il2cpp_library,il2cpp_image::kIl2cppBase,il2cpp_image::kIl2cppSpan,il2cpp_image_data);
    Module unity;unity.name="libunity.so";unity.aliases={"/data/app/bunker/lib/arm64/libunity.so"};
    const auto unity_file_bytes=fs::file_size(library),il2cpp_file_bytes=fs::file_size(il2cpp_library);
    Require(unity_file_bytes<=SIZE_MAX&&il2cpp_file_bytes<=SIZE_MAX,"verified ELF byte length exceeds native size_t");
    unity.address_image=DescribeModuleElf(image.file,static_cast<size_t>(unity_file_bytes),kUnityBase,unity.aliases.front());
    unity.export_policy=Module::ExportPolicy::BionicRawSymbols;
    // Keep the raw ELF integrity check separate from public Bionic dlsym.
    for(auto &symbol:kUnityExports){auto actual=reinterpret_cast<addr_t>(elf_symbol(&image,symbol.name));if(actual!=symbol.address)throw std::runtime_error(std::string("loaded export differs from translation: ")+symbol.name);unity.exports.emplace(symbol.name,actual);}
    unity.initializers.assign(std::begin(kInitializers),std::end(kInitializers));
    unity.finalizers.assign(std::begin(kUnityFinalizers),std::end(kUnityFinalizers));
    modules.Add(std::move(unity));
    Module il2cpp;il2cpp.name="libil2cpp.so";il2cpp.aliases={"/data/app/bunker/lib/arm64/libil2cpp.so"};
    il2cpp.address_image=DescribeModuleElf(il2cpp_image_data.file,static_cast<size_t>(il2cpp_file_bytes),il2cpp_image::kIl2cppBase,il2cpp.aliases.front());
    il2cpp.export_policy=Module::ExportPolicy::BionicRawSymbols;
    for(auto &symbol:il2cpp_image::kExports){auto actual=reinterpret_cast<addr_t>(elf_symbol(&il2cpp_image_data,symbol.name));Require(actual==symbol.address,"IL2CPP export differs from verified ELF");il2cpp.exports.emplace(symbol.name,actual);}
    il2cpp.initializers=il2cpp_ledger.constructors;il2cpp.finalizers=il2cpp_ledger.finalizers;modules.Add(std::move(il2cpp));
    Require(modules.ReferenceCount("libil2cpp.so")==0&&!il2cpp_ledger.constructed,"IL2CPP constructor state was made eager");
    stage="constructors";auto module=modules.Open("libunity.so",2);
    if(!module||constructor_index!=kExpectedConstructors)throw std::runtime_error("not all real Unity constructors completed");
    std::fprintf(stderr,"Unity bootstrap: ALL %zu real constructors completed in order\n",constructor_index);
    stage="JNI_OnLoad";auto onload=modules.Symbol(module,"JNI_OnLoad");if(!onload||!Translated(onload))throw std::runtime_error("real JNI_OnLoad export is not translated");
    auto version=threads.Run(onload,{java.VmAddress(),0});
    std::fprintf(stderr,"Unity bootstrap: real JNI_OnLoad returned %#llx, pending Java exception: %s\n",static_cast<unsigned long long>(version),java.PendingException().c_str());
    if(version!=bunker::jni::kVersion16||!java.PendingException().empty())return 5;
    for(auto &binding:kUnityRegistrations)if(java.NativeEntry(binding.class_name,binding.name,binding.signature)!=binding.entry)throw std::runtime_error(std::string("actual JNI_OnLoad native registration mismatch: ")+binding.class_name+"."+binding.name);
    {
      LocalFrame locals(java);
      State app_state{};app_state.gpr.x0.qword=java.EnvAddress();app_state.gpr.x1.qword=reinterpret_cast<addr_t>("com/unity3d/player/UnityPlayer");java.InvokeEnv(6,app_state);
      Require(java.PendingException().empty()&&app_state.gpr.x0.qword,"native loader application class is unavailable");
      JavaNativeLoader::Config config;
      config.application_class=app_state.gpr.x0.qword;config.application_loader=java.LocalValue(application_loader);
      config.libraries={{u"unity","/data/app/bunker/lib/arm64/libunity.so",onload},{u"il2cpp","/data/app/bunker/lib/arm64/libil2cpp.so",0}};
      if(firebase)config.libraries.push_back({u"FirebaseCppApp-9_6_0","/data/app/bunker/lib/arm64/libFirebaseCppApp-9_6_0.so",0});
      config.runner=[&threads](addr_t entry,const std::vector<addr_t>&args){
        Require(Translated(entry),"Java JNI_OnLoad target is not translated");
        auto result=threads.Run(entry,args);
        {GameWorkerObservation::NativeEnvironment native;std::fprintf(stderr,"Game Java native loader: original JNI_OnLoad entry=%#llx returned=%#llx\n",static_cast<unsigned long long>(entry),static_cast<unsigned long long>(result));}
        return result;
      };
      config.observer=[](const JavaNativeLoader::Event &event){
        GameWorkerObservation::NativeEnvironment native;
        try{
          auto name=Utf8(event.name);
          std::fprintf(stderr,"Game Java native loader: name=%s path=%s success=%d cached=%d recursive=%d adopted=%d handle=%#llx onload=%#llx detail=%s\n",name.c_str(),event.path.c_str(),int(event.success),int(event.cached),int(event.recursive),int(event.adopted),static_cast<unsigned long long>(event.handle),static_cast<unsigned long long>(event.onload),event.detail.c_str());
        }catch(...){std::fputs("Game Java native loader: diagnostic text could not be represented\n",stderr);}
      };
      config.diagnostic=[](int priority,const std::u16string &message){
        GameWorkerObservation::NativeEnvironment native;
        try{auto text=Utf8(message);std::fprintf(stderr,"Game Java native loader: priority=%d %s\n",priority,text.c_str());}
        catch(...){std::fputs("Game Java native loader: failure text could not be represented\n",stderr);}
      };
      native_loader=std::make_unique<JavaNativeLoader>(java,modules,config);
      // This real call already completed above. Adopt its handle/result without
      // another dlopen, constructor cycle or JNI_OnLoad execution.
      native_loader->Adopt(u"unity",module,static_cast<int32_t>(version));
      native_loader->BindUnityPlayer();
      UnityNetworkProxy::Config proxy_config;
      proxy_config.observer=[](const UnityNetworkProxy::Event &event){
        GameWorkerObservation::NativeEnvironment native;
        const auto *properties=event.manual_properties?&*event.manual_properties:nullptr;
        int host_present=-1,automatic=-1,pac_configured=-1;
        if(properties){
          const auto &host=event.scheme=="https"?properties->httpsHost:properties->httpHost;
          host_present=int(host&&!host->empty());automatic=int(properties->autoDetect);
          pac_configured=int(properties->autoConfigUrl&&!properties->autoConfigUrl->empty());
        }
        std::fprintf(stderr,"Game network proxy: scheme=%s readerUsed=%d manualHostPresent=%d resultPresent=%d autoDetect=%d pacConfigured=%d; manual properties only, automatic resolution not performed\n",event.scheme.c_str(),int(event.reader_used),host_present,int(event.result.has_value()),automatic,pac_configured);
      };
      network_proxy=std::make_unique<UnityNetworkProxy>(java,std::move(proxy_config));
    }
    std::fprintf(stderr,"Unity bootstrap: all %zu evidenced native bindings match original ELF/DEX\n",std::size(kUnityRegistrations));
    for(auto name:kUnityJavaClasses){auto count=java.NativeCount(name);if(count)std::fprintf(stderr,"Unity bootstrap: registered %zu natives on %s\n",count,name);}
    // Reaching this line proves constructors and JNI registration only. Surface,
    // graphics/audio initialization and the game lifecycle are separate gates.
    std::fprintf(stderr,"Unity bootstrap: %sconstructor/JNI-only gate passed; %zu unsupported traces remain; the game is not running\n",kExploratory?"EXPLORATORY ":"",kUnsupportedTraces);
    if(init_jni){
      stage="initJni(Context)";LocalFrame frame(java);
      auto entry=java.NativeEntry("com/unity3d/player/UnityPlayer","initJni","(Landroid/content/Context;)V");
      if(!entry||!Translated(entry))throw std::runtime_error("verified instance-native initJni(Context) entry is unavailable");
      auto player=java.NewInstance("com/unity3d/player/UnityPlayer"),activity=context.Activity();
      if(!player||!activity||!java.PendingException().empty())throw std::runtime_error("typed initJni receiver/context creation failed");
      context.BindUnityPlayer(player);
      std::fprintf(stderr,"Unity bootstrap: %sisolated instance-native initJni(Context)V gate entry=%#llx; host-typed UnityPlayer/Activity objects; Java <clinit>/constructors are not executed or marked initialized\n",kExploratory?"EXPLORATORY ":"",static_cast<unsigned long long>(entry));
      init_java_errors=0;init_java_diagnostic_active=true;
      threads.Run(entry,{java.EnvAddress(),player,activity});
      init_java_diagnostic_active=false;
      auto exception=java.PendingException();
      std::fprintf(stderr,"Unity bootstrap: initJni native entry returned; pending Java exception: %s\n",exception.c_str());
      if(!exception.empty())throw std::runtime_error("initJni stopped with pending Java exception: "+exception);
      if(init_java_errors.load())throw std::runtime_error("initJni encountered "+std::to_string(init_java_errors.load())+" Java exception(s); inspect the original JNI diagnostics even when native code cleared them");
      std::fprintf(stderr,"Unity bootstrap: isolated initJni native-entry gate passed; Activity/UnityPlayer Java lifecycle, surface/render/resume and game startup remain unexecuted\n");
      if(surface_gate){
        auto recreate=java.NativeEntry("com/unity3d/player/UnityPlayer","nativeRecreateGfxState","(ILandroid/view/Surface;)V");
        auto changed=java.NativeEntry("com/unity3d/player/UnityPlayer","nativeSendSurfaceChangedEvent","()V");
        if(!Translated(recreate)||!Translated(changed))throw std::runtime_error("verified surface natives are not translated");
        auto window=native_windows->Create(width,height,frame_gate?u"Bunker":u"Bunker native surface diagnostic",frame_gate);
        if(frame_gate&&start_fullscreen)native_windows->SetFullscreen(window,true);
        if(frame_gate&&!bunker_view_loader.empty()){
          auto view=std::make_shared<bunker::windows::BunkerView>(bunker_view_loader,bunker_view_state,bunker_view_data);
          native_windows->SetMessageHook(window,[view](void *hwnd,uint32_t message,uint64_t wparam,int64_t lparam){return view->Message(hwnd,message,wparam,lparam);});
        }
        auto surface=native_windows->NewSurface(window);
        {LocalFrame locals(java);displays.BindActivityWindow(context.Activity(),native_windows->NativeHandleReader(window));}
        auto token=native_windows->FromSurface(surface);
        auto actual_width=native_windows->Width(token),actual_height=native_windows->Height(token);native_windows->Release(token);
        if(actual_width!=width||actual_height!=height)throw std::runtime_error("actual HWND client geometry differs from configured landscape pixels");
        std::fprintf(stderr,"Unity bootstrap: %ssurface-only host lifecycle port; actual HWND client=%dx%d; APK sensorLandscape; resumed=false; no Java class-init or render/resume claim\n",kExploratory?"EXPLORATORY ":"",actual_width,actual_height);
        uint64_t worker_token=0;bool worker_registered=false,unity_looper_prepared=false;
        auto looper_dispatcher=std::make_shared<GameUnityLooperDispatcher>();
        std::unique_ptr<UnitySoftInputSession> soft_input;
        if(frame_gate){
          soft_input=std::make_unique<UnitySoftInputSession>(java,player,[&](addr_t target,const std::vector<addr_t>&args){
            Require(Translated(target),"soft-input native entry is not translated");
            std::fprintf(stderr,"Game soft input: original native=%#llx argc=%zu UnityMain=%lu\n",(unsigned long long)target,args.size(),GetCurrentThreadId());
            return threads.Run(target,args);
          });
          // The original Unity module has completed JNI_OnLoad and initJni;
          // its registered entries remain live until after UnityMain stops.
          Require(modules.ReferenceCount("libunity.so")!=0,"soft-input native-loaded state requires the real Unity module");
          soft_input->SetNativeLoaded(true);
          // With sound on, Unity asks to hear about phone calls so it can pause audio. There are none.
          java.BindVoidMethod("com/unity3d/player/UnityPlayer","addPhoneCallListener","()V",[](addr_t,const bunker::jni::Host::MethodArguments&){});
        }
        const auto actual_render=frame_gate?java.NativeEntry("com/unity3d/player/UnityPlayer","nativeRender","()Z"):0;
        const auto actual_resume=mute_audio?java.NativeEntry("com/unity3d/player/UnityPlayer","nativeResume","()V"):0;
        const auto actual_mute=mute_audio?java.NativeEntry("com/unity3d/player/UnityPlayer","nativeMuteMasterAudio","(Z)V"):0;
        if(mute_audio&&(!Translated(actual_resume)||!Translated(actual_mute)))throw std::runtime_error("original resume/master-mute entries are not translated");
        uint64_t render_observation_calls=0;
        UnitySurfaceLifecycle lifecycle(java,[&](addr_t target,const std::vector<addr_t>&args){
          if(first_frame&&actual_render&&target==actual_render&&++render_observation_calls==4){
            fourth_frame_observation_token.store(worker_token,std::memory_order_relaxed);
            fourth_frame_observation_thread.store(GetCurrentThreadId(),std::memory_order_relaxed);
            FourthFrameNativeEnvironment environment;
            std::fprintf(stderr,"Fourth-frame observation: UnityMain=%lu; JNI phases and all existing synchronization ENTER/RETURN observations enabled; no guest control\n",GetCurrentThreadId());
          }
          if(actual_render&&target==actual_render&&!GameQuietTrace())std::fprintf(stderr,"Game bootstrap: before original nativeRender IL2CPP constructors=%zu refs=%zu\n",il2cpp_ledger.constructed,modules.ReferenceCount("libil2cpp.so"));
          if(!GameQuietTrace())std::fprintf(stderr,"Unity bootstrap: UnityMain host-thread=%lu native=%#llx argc=%zu env=%#llx receiver=%#llx display=%#llx surface=%#llx\n",GetCurrentThreadId(),static_cast<unsigned long long>(target),args.size(),static_cast<unsigned long long>(args.at(0)),static_cast<unsigned long long>(args.at(1)),static_cast<unsigned long long>(args.size()>2?args[2]:0),static_cast<unsigned long long>(args.size()>3?args[3]:0));
          auto result=[&] {
            if(!actual_render || target!=actual_render)return threads.Run(target,args);
            bunker::windows::ComponentTypeQueryFrame queries;
            return threads.Run(target,args);
          }();
          if(firebase)firebase->CheckHealthy();
          if(first_frame&&actual_render&&target==actual_render&&render_observation_calls==4&&ObserveFourthFrameThread()){
            FourthFrameNativeEnvironment environment;fourth_frame_observation_thread.store(0,std::memory_order_relaxed);
            std::fputs("Fourth-frame observation: original render returned; exhaustive JNI/synchronization observation completed\n",stderr);
          }
          if(!GameQuietTrace())std::fprintf(stderr,"Unity bootstrap: UnityMain native=%#llx returned\n",static_cast<unsigned long long>(target));
          if(mute_audio&&!GameSound()&&target==actual_resume){
            // An explicit user control after resume, using the worker's actual
            // JNI environment/receiver. It is not a synthetic volume event.
            if(!java.PendingException().empty())throw std::runtime_error("original nativeResume raised a Java exception before master mute");
            threads.Run(actual_mute,{args.at(0),args.at(1),1});
            if(!java.PendingException().empty())throw std::runtime_error("original master-mute request raised a Java exception");
            std::fprintf(stderr,"Game bootstrap: explicit muted diagnostic; original nativeMuteMasterAudio(true) returned; hardware-volume observer disabled; no endpoint/volume or audio-output qualification\n");
          }
          return result;
        },{recreate,changed},player,[&,looper_dispatcher]{
          {std::lock_guard lock(looper_dispatcher->mutex);looper_dispatcher->owner=nullptr;}
          if(ObserveFourthFrameThread())fourth_frame_observation_thread.store(0,std::memory_order_relaxed);
          const auto token=worker_token;
          if(!token){if(unity_looper_prepared){choreographer->ReleaseCurrentLooper();unity_looper_prepared=false;}return;}
          int released=0;std::exception_ptr release_error;
          try{if(egl)egl->ReleaseCurrentThread();}catch(...){release_error=std::current_exception();}
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
          if(main_collector&&main_collector->Owns(token))main_collector->DetachCurrent();
#endif
          try{if(tls_audit.PendingModuleCallbacks(threads))Require(!unity_ledger.finalized&&!il2cpp_ledger.finalized,"real UnityMain TLS survives an already finalized module; safe callback lifetime is unproved");released=threads.ReleaseCurrent();}catch(...){if(!release_error)release_error=std::current_exception();}
          // Original TLS callbacks still have the actual prepared Looper/JNI
          // owner. Retire its queue after those callbacks, before VM detach.
          try{if(unity_looper_prepared){choreographer->ReleaseCurrentLooper();unity_looper_prepared=false;}}catch(...){if(!release_error)release_error=std::current_exception();}
          jumps->DropThread(token,token);
          const auto dropped=worker_registered?process_signals.DropThread(token):0;
          if(release_error)std::rethrow_exception(release_error);
          if(released||dropped)throw std::runtime_error("UnityMain runner/signal lifetime cleanup failed");
        });
        try {
        if(frame_gate){
          auto native=[&](const char *name,const char *signature){auto address=java.NativeEntry("com/unity3d/player/UnityPlayer",name,signature);if(!Translated(address))throw std::runtime_error(std::string("required frame native is not translated: ")+name);return address;};
          lifecycle.ConfigureOneFrame({native("nativeResume","()V"),native("nativeFocusChanged","(Z)V"),native("nativeRender","()Z"),native("nativePause","()Z"),native("nativeDone","()Z")},[&]{soft_input->DrainGlThreadJobs();});
        }
        lifecycle.AfterInitJni();
        // A real model mask is captured at Start, then inherited by the child.
        // Full policy imports are installed, with delivery explicitly rejected
        // because these translated cohorts do not poll for suspension.
        Require(threads.Self()==parent_token&&bool(process_signals.Lookup(parent_token)),"actual UI virtual signal lifetime changed before UnityMain start");
        uint64_t parent_mask=0;if(process_signals.Mask(parent_token,0,nullptr,&parent_mask))throw std::runtime_error("cannot snapshot parent virtual signal mask");
        {std::lock_guard lock(looper_dispatcher->mutex);looper_dispatcher->owner=&lifecycle;}
        lifecycle.Start([&,parent_token,parent_mask,looper_dispatcher]{
          worker_token=threads.Self();
          if(soft_input)soft_input->SetUnityMainCurrent();
          if(process_signals.RegisterThread(worker_token,parent_token))throw std::runtime_error("cannot register UnityMain virtual signal-mask lifetime");
          worker_registered=true;
          if(process_signals.BindCurrent(worker_token)||process_signals.Mask(worker_token,2,&parent_mask,nullptr))throw std::runtime_error("cannot inherit UnityMain signal mask");
          choreographer->PrepareCurrentLooper(u"UnityMain",[&,looper_dispatcher]{
            std::lock_guard lock(looper_dispatcher->mutex);
            return looper_dispatcher->owner&&looper_dispatcher->owner->Post([&]{choreographer->PumpCurrentLooper();});
          });unity_looper_prepared=true;
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
          if(main_collector)main_collector->AttachCurrent(worker_token,parent_token);
#endif
          std::fprintf(stderr,"Unity bootstrap: UnityMain virtual-thread=%llu generation=%llu inherited mask=%#llx; alternate stack disabled\n",static_cast<unsigned long long>(worker_token),static_cast<unsigned long long>(worker_token),static_cast<unsigned long long>(parent_mask));
        });init_java_errors=0;init_java_diagnostic_active=true;
        stage="surfaceCreated/recreate";lifecycle.SurfaceCreated(surface);lifecycle.WaitIdle();
        stage="surfaceChanged/recreate+notify";lifecycle.SurfaceChanged(surface);lifecycle.WaitIdle();
        if(frame_gate){
          stage="one-frame resume/focus/render/pause/done";
          auto indicator=java.NativeEntry("com/unity3d/player/UnityPlayer","nativeRestartActivityIndicator","()V");
          if(!Translated(indicator))throw std::runtime_error("actual UI resume activity-indicator entry is not translated");
          std::fprintf(stderr,"Unity bootstrap: UI resume prerequisite nativeRestartActivityIndicator entry=%#llx\n",static_cast<unsigned long long>(indicator));
          threads.Run(indicator,{java.EnvAddress(),player});
          if(!java.PendingException().empty()||init_java_errors.load())throw std::runtime_error("UI resume prerequisite raised an original Java exception");
          std::unique_ptr<AudioVolumeSession> volume;
          if(mute_audio){
            std::fprintf(stderr,"Game bootstrap: explicit muted diagnostic requested; hardware-volume observer omitted; original master mute requested after nativeResume; later engine audio initialization remains unqualified\n");
          }else{
            volume=std::make_unique<AudioVolumeSession>(java,[&](addr_t address,const std::vector<addr_t>&args){return threads.Run(address,args);},activity,player);
            auto audio=volume->InitialSnapshot();
            std::fprintf(stderr,"Unity bootstrap: real read-only music-volume observer ready scalar=%g muted=%d profile-index=%d; no initial native callback; no playback/system mutation\n",double(audio.scalar),audio.muted,AudioVolumeSession::MusicIndex(audio));
          }
          auto poll_volume=[&]{if(soft_input)soft_input->DrainUiJobs();if(volume)volume->Poll();};
          auto surface_resized=[&](const NativeWindows::Event &event){
            if(event.kind!=NativeWindows::Event::Resized||
               (event.width==actual_width&&event.height==actual_height))return;
            if(event.width<=0||event.height<=0)throw std::runtime_error("native surface resize has nonpositive client dimensions");
            std::fprintf(stderr,"Unity bootstrap: real client resize %dx%d -> %dx%d; queuing original surfaceChanged on UnityMain\n",actual_width,actual_height,event.width,event.height);
            lifecycle.SurfaceChanged(surface);
            actual_width=event.width;actual_height=event.height;
          };
          poll_volume();lifecycle.ActivityResumed();
          auto lease=native_windows->FromSurface(surface);auto hwnd=static_cast<HWND>(native_windows->NativeHandle(lease));native_windows->Release(lease);
          if(GetForegroundWindow()!=hwnd)std::fprintf(stderr,"Unity bootstrap: real foreground activation requested, accepted=%d\n",int(SetForegroundWindow(hwnd)!=0));
          bool focused=false;auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(first_frame?60:3);
          while(!focused&&std::chrono::steady_clock::now()<deadline){
            if(soft_input)soft_input->DrainUiJobs();
            for(const auto &event:native_windows->DrainEvents(window)){
              if(event.kind==NativeWindows::Event::Focus){lifecycle.WindowFocusChanged(event.focused);focused=event.focused;}
              if(event.kind==NativeWindows::Event::CloseRequested)throw std::runtime_error("window close requested before first native frame");
              surface_resized(event);
            }
            if(!focused)std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }
          if(!focused)throw std::runtime_error("no real HWND focus event; native frame prerequisite remains unmet");
          if(first_frame)std::fputs("First frame: waiting for original nativeRender; no manual initialization or skipped guest calls\n",stderr);
          // Left mouse button -> single-finger touch through nativeInjectEvent.
          bunker::windows::touch_input::Injector touch(java,[&](addr_t target,const std::vector<addr_t>&args){return threads.Run(target,args);},player,[]{const char*v=std::getenv("BUNKER_TOUCH_TRACE");return v&&std::strcmp(v,"0");}());
          float last_touch_x=0,last_touch_y=0;
          // Keyboard stick (WASD/arrows) owns the single touch pointer while a
          // direction is held; mouse input waits until it is released.
          bool stick_active=false;
          auto pointer_event=[&](const NativeWindows::Event &event){
            using T=bunker::windows::touch_input::Injector;
            if(event.kind==NativeWindows::Event::Focus){if(!event.focused)touch.Inject(T::Cancel,last_touch_x,last_touch_y);return;}
            if(!event.mouse.client_position_valid||stick_active)return;
            // Unity reads touches in its surface buffer's pixels. When the game's
            // buffers are a fixed size (Screen.SetResolution, --render-size), the
            // presenter scales them to the client, so scale the pointer the same way.
            float scale_x=1,scale_y=1;
            {auto lease=native_windows->FromSurface(surface);
             if(lease){auto g=native_windows->Describe(lease);native_windows->Release(lease);
               if(g.fixed_size&&g.client_width>0&&g.client_height>0){scale_x=float(g.width)/float(g.client_width);scale_y=float(g.height)/float(g.client_height);}}}
            const float x=float(event.mouse.x)*scale_x,y=float(event.mouse.y)*scale_y;last_touch_x=x;last_touch_y=y;
            if(event.kind==NativeWindows::Event::MouseButtonDown&&event.mouse.button==NativeWindows::MouseData::Button::Left)touch.Inject(T::Down,x,y);
            else if(event.kind==NativeWindows::Event::MouseButtonUp&&event.mouse.button==NativeWindows::MouseData::Button::Left)touch.Inject(T::Up,x,y);
            else if(event.kind==NativeWindows::Event::MouseMove)touch.Inject(T::Move,x,y);
          };
          // Window keyboard -> host soft-input text while Unity has a keyboard open,
          // otherwise android.view.KeyEvents (Esc = Back) through nativeInjectEvent.
          // Virtual-key codes (Win32 VK_*); a Linux window backend maps to the same.
          // F11/Alt+Enter (fullscreen) are consumed by the window and never arrive.
          bool shift_down=false,ctrl_down=false,alt_down=false;
          struct Held {int32_t repeat=0;int64_t down_ms=0;};std::map<int32_t,Held> held_keys;
          bunker::windows::keyboard_touch::Stick keyboard_stick;
          bool confirm_probe=false;int confirm_tap=0;int32_t confirm_scan=0;float confirm_x=0,confirm_y=0;std::chrono::steady_clock::time_point confirm_at;
          auto key_event=[&](const NativeWindows::Event &event){
            using bunker::windows::touch_input::AndroidKeyCode;using bunker::windows::touch_input::AndroidMetaState;
            using bunker::windows::touch_input::AndroidUnicode;using T=bunker::windows::touch_input::Injector;
            if(event.kind==NativeWindows::Event::Focus){
              if(!event.focused){for(auto &[code,h]:held_keys)touch.InjectKey(false,code,0,0,0,0,h.down_ms);held_keys.clear();shift_down=ctrl_down=alt_down=false;
                keyboard_stick=bunker::windows::keyboard_touch::Stick{};stick_active=false;}  // pointer_event cancels the touch
              return;}
            if(event.kind!=NativeWindows::Event::KeyDown&&event.kind!=NativeWindows::Event::KeyUp)return;
            const auto vk=event.key.virtual_key;const bool down=event.kind==NativeWindows::Event::KeyDown;
            if(vk==0x10||vk==0xA0||vk==0xA1)shift_down=down;
            if(vk==0x11||vk==0xA2||vk==0xA3)ctrl_down=down;
            if(vk==0x12||vk==0xA4||vk==0xA5)alt_down=down;
            if(soft_input&&soft_input->KeyboardActive()){
              if(!down)return;
              char16_t c=0;
              if(vk>=0x41&&vk<=0x5A)c=char16_t(shift_down?vk:vk+0x20);
              else if(vk>=0x30&&vk<=0x39)c=char16_t(vk);
              else if(vk>=0x60&&vk<=0x69)c=char16_t(u'0'+(vk-0x60));
              else if(vk==0x20)c=u' ';else if(vk==0x08)c=u'\b';else if(vk==0x0D)c=u'\r';else if(vk==0x1B)c=0x1b;
              else if(vk==0xBD)c=shift_down?u'_':u'-';else if(vk==0xBE)c=u'.';
              if(!c)return;
              for(unsigned i=0;i<(event.key.repeat_count?event.key.repeat_count:1u);++i)soft_input->TypeCharacter(c);
              return;
            }
            // Touch-only game controls on the keyboard. The game moves the
            // character by dragging anywhere on screen, so WASD/arrows hold a
            // drag from the screen centre toward the held direction; Space taps
            // the centre (advance dialogue, "tap to start").
            if(bunker::windows::keyboard_touch::Direction(vk)||vk==0x20){
              using T=bunker::windows::touch_input::Injector;
              float w=0,h=0;
              {auto lease=native_windows->FromSurface(surface);
               if(lease){auto g=native_windows->Describe(lease);native_windows->Release(lease);w=float(g.width);h=float(g.height);}}
              if(w<=0||h<=0)return;
              if(vk==0x20){  // press and release with the key, like a finger
                if(stick_active)return;
                if(down&&!event.key.previous_down){touch.Inject(T::Down,w*0.5f,h*0.5f);last_touch_x=w*0.5f;last_touch_y=h*0.5f;}
                else if(!down)touch.Inject(T::Up,w*0.5f,h*0.5f);
                return;
              }
              keyboard_stick.Set(vk,down);  // applied once per frame by update_stick
              return;
            }
            // Enter confirms a dialog (taps its OK/Yes plate, found by sampling
            // the next presented frame in update_confirm); otherwise it reaches
            // the game as KEYCODE_ENTER there.
            if(vk==0x0D&&!ctrl_down&&!alt_down){
              if(graphics&&down&&!event.key.previous_down&&!confirm_probe&&!confirm_tap){
                std::vector<std::pair<float,float>> points;
                for(const auto &p:bunker::windows::keyboard_confirm::SamplePoints())points.emplace_back(p.x,p.y);
                graphics->RequestPixelProbe(std::move(points));confirm_probe=true;confirm_scan=event.key.scan_code;confirm_at=std::chrono::steady_clock::now();
              }
              return;
            }
            const int32_t code=AndroidKeyCode(vk);if(!code)return;
            const int32_t meta=AndroidMetaState(shift_down,ctrl_down,alt_down),scan=event.key.scan_code;
            if(down){
              auto [it,fresh]=held_keys.try_emplace(code);
              if(fresh)it->second.down_ms=T::Now();else ++it->second.repeat;
              touch.InjectKey(true,code,it->second.repeat,meta,AndroidUnicode(code,shift_down),scan,it->second.down_ms);
            }else{
              auto it=held_keys.find(code);if(it==held_keys.end())return;
              touch.InjectKey(false,code,0,meta,AndroidUnicode(code,shift_down),scan,it->second.down_ms);held_keys.erase(it);
            }
          };
          // Once per frame: the touch goes down at the centre on one frame and
          // moves toward the held direction on later ones, so Unity sees a drag
          // (a down and move delivered before the same frame read as a tap).
          auto update_stick=[&]{
            using T=bunker::windows::touch_input::Injector;
            if(!keyboard_stick.Any()&&!stick_active)return;
            float w=0,h=0;
            {auto lease=native_windows->FromSurface(surface);
             if(lease){auto g=native_windows->Describe(lease);native_windows->Release(lease);w=float(g.width);h=float(g.height);}}
            if(w<=0||h<=0)return;
            const auto target=keyboard_stick.Target(w,h);
            if(target.active&&!stick_active){
              touch.Inject(T::Cancel,last_touch_x,last_touch_y);
              touch.Inject(T::Down,target.origin_x,target.origin_y);stick_active=true;
              last_touch_x=target.origin_x;last_touch_y=target.origin_y;
            }else if(target.active){
              // Glide toward the target like a finger (the game ignores a drag
              // that jumps its full length in one frame).
              const auto step=bunker::windows::keyboard_touch::Step(last_touch_x,last_touch_y,target.x,target.y,h*0.02f);
              touch.Inject(T::Move,step.first,step.second);last_touch_x=step.first;last_touch_y=step.second;
            }else if(stick_active){touch.Inject(T::Up,last_touch_x,last_touch_y);stick_active=false;}
          };
          // Once per frame: finish an Enter press. A found confirm button gets a
          // finger tap (down this frame, up the next); no button sends Enter.
          auto update_confirm=[&]{
            using T=bunker::windows::touch_input::Injector;
            if(confirm_tap){touch.Inject(T::Up,confirm_x,confirm_y);confirm_tap=0;return;}
            if(!confirm_probe)return;
            auto colours=graphics->TakePixelProbe();
            if(!colours&&std::chrono::steady_clock::now()-confirm_at<std::chrono::milliseconds(500))return;
            confirm_probe=false;if(!colours)colours.emplace();  // no frame presented: send Enter
            std::vector<bunker::windows::keyboard_confirm::Rgba> samples(colours->begin(),colours->end());
            const auto button=bunker::windows::keyboard_confirm::Choose(samples);
            // A dialog still fading in has no full-bright plate yet: sample the
            // next frames for up to 0.4 s before sending Enter to the game.
            if(!button&&!samples.empty()&&std::chrono::steady_clock::now()-confirm_at<std::chrono::milliseconds(400)){
              std::vector<std::pair<float,float>> points;
              for(const auto &p:bunker::windows::keyboard_confirm::SamplePoints())points.emplace_back(p.x,p.y);
              graphics->RequestPixelProbe(std::move(points));confirm_probe=true;return;
            }
            float w=0,h=0;
            {auto lease=native_windows->FromSurface(surface);
             if(lease){auto g=native_windows->Describe(lease);native_windows->Release(lease);w=float(g.width);h=float(g.height);}}
            if(button&&w>0&&h>0&&!stick_active){
              confirm_x=button->x*w;confirm_y=button->y*h;
              std::fprintf(stderr,"Keyboard confirm: Enter taps button at %.0f,%.0f\n",double(confirm_x),double(confirm_y));
              touch.Inject(T::Down,confirm_x,confirm_y);last_touch_x=confirm_x;last_touch_y=confirm_y;confirm_tap=1;
            }else{
              const auto now=T::Now();
              touch.InjectKey(true,66,0,0,'\n',confirm_scan,now);touch.InjectKey(false,66,0,0,'\n',confirm_scan,now);
            }
          };
          bool keep_running=false,render_returned=false;
          if(first_frame){
            bool next_frame=false,close=false;uint64_t completed_frames=0,later_snapshot_request=0;
            // Frame timing: request->return (inside the original frame) versus
            // return->request (host continuation latency), averaged per 60 frames.
            auto requested_at=std::chrono::steady_clock::now(),returned_at=requested_at;double render_ms=0,gap_ms=0;
            GameFrameLimiter limiter(GameFrameCap());
            uint64_t presents_at_request=0;
            bool frame_presented=false;
            while(!close){
              for(const auto &event:native_windows->DrainEvents(window)){
                if(event.kind==NativeWindows::Event::Focus)lifecycle.WindowFocusChanged(event.focused);
                if(event.kind==NativeWindows::Event::CloseRequested)close=true;
                surface_resized(event);pointer_event(event);key_event(event);
              }
              update_stick();update_confirm();
              poll_volume();
              if(close)break;
              if(!next_frame){
                if(auto result=lifecycle.PollFrame()){
                  keep_running=*result;render_returned=true;frame_continues=keep_running;++completed_frames;
                  frame_presented=pace_window_swaps&&observed_presentations.load(std::memory_order_relaxed)!=presents_at_request;
                  returned_at=std::chrono::steady_clock::now();render_ms+=std::chrono::duration<double,std::milli>(returned_at-requested_at).count();
                  auto diagnostic=graphics->DescribeFrameDiagnostics();
                  const bool witness=diagnostic.frame_png&&diagnostic.nonuniform&&diagnostic.draw_submissions&&diagnostic.presented_capture_serial;
                  if(!GameQuietTrace())std::fprintf(stderr,"First frame: original nativeRender returned frame=%llu continue=%d presentedCandidate=%d draws=%llu\n",(unsigned long long)completed_frames,int(keep_running),int(witness),(unsigned long long)diagnostic.draw_submissions);
                  if(!keep_running||frame_run.StopAfterReturnedFrame(completed_frames,witness))break;
                  if(frame_run.SnapshotBeforeNextFrame(completed_frames)){
                    later_snapshot_request=graphics->RequestFrameSnapshot((storage/L"later-frame.png").wstring());
                    std::fprintf(stderr,"Frame continuation: snapshot request=%llu after returnedFrame=%llu; next genuine successful swap only\n",(unsigned long long)later_snapshot_request,(unsigned long long)completed_frames);
                  }
                  scene_route.AfterFrame([&](bool down){
                    auto lease=native_windows->FromSurface(surface);
                    if(!lease)throw std::runtime_error("scene route lost its surface");
                    auto geometry=native_windows->Describe(lease);
                    native_windows->Release(lease);
                    using T=bunker::windows::touch_input::Injector;
                    touch.Inject(down?T::Down:T::Up,geometry.width*0.5f,geometry.height*0.90f);
                  },[&](const std::wstring &path){graphics->RequestFrameSnapshot(path,true);});
                  next_frame=true;
                }
              }

              if(next_frame&&(frame_presented||limiter.Ready(std::chrono::steady_clock::now()))){
                ApplyGameTargetFps();
                presents_at_request=observed_presentations.load(std::memory_order_relaxed);
                if(!lifecycle.RequestNextFrame()){FramePollSleep();continue;}
                next_frame=false;requested_at=std::chrono::steady_clock::now();limiter.Started(requested_at);gap_ms+=std::chrono::duration<double,std::milli>(requested_at-returned_at).count();
                if(completed_frames%60==0){std::fprintf(stderr,"Frame timing: frames=%llu avg_render_ms=%.2f avg_host_gap_ms=%.2f (last 60)\n",(unsigned long long)completed_frames,render_ms/60,gap_ms/60);render_ms=gap_ms=0;}
                if(completed_frames<=8||completed_frames%60==0)std::fprintf(stderr,"First frame: requesting next original nativeRender after frame=%llu; same UnityMain/window, no repeated nativeResume\n",(unsigned long long)completed_frames);
              }
              // A frame in flight: wake the moment it returns (the next request
              // goes out without a poll-tick delay); otherwise tick for the cap.
              if(!next_frame)lifecycle.AwaitFrame(std::chrono::milliseconds(2));else FramePollSleep(limiter.PollDelay());
            }
            if(frame_run.Continuing())std::fprintf(stderr,"Frame continuation: stopped returnedFrames=%llu reason=%s; same original UnityMain/window, no repeated nativeResume\n",(unsigned long long)completed_frames,close?"real window close":!keep_running?"original nativeRender returned false":"requested returned-frame limit");
            if(later_snapshot_request){
              auto snapshot=graphics->DescribeFrameSnapshot();Require(snapshot.request_serial==later_snapshot_request,"later-frame snapshot request serial changed");
              std::fprintf(stderr,"Frame continuation: snapshot request=%llu completed=%d pending=%d presentedSerial=%llu attempts=%llu nonuniform=%d size=%dx%d failure=%s\n",(unsigned long long)snapshot.request_serial,int(snapshot.completed),int(snapshot.pending),(unsigned long long)snapshot.presented_capture_serial,(unsigned long long)snapshot.capture_attempts,int(snapshot.nonuniform),snapshot.width,snapshot.height,snapshot.failure.c_str());
            }
          }else{
            while(true){try{keep_running=lifecycle.WaitForFrame();render_returned=true;break;}catch(const std::runtime_error &e){if(std::string(e.what())!="one-frame diagnostic timed out before nativeRender completed")throw;std::fprintf(stderr,"Game bootstrap: first nativeRender observation window expired; retaining same live worker/window and waiting, not restarting\n");poll_volume();}}
          }
          lifecycle.WaitIdle();poll_volume();
          if(first_frame){if(auto result=lifecycle.PollFrame()){keep_running=*result;render_returned=true;}}
          frame_continues=keep_running;
          if(render_returned)std::fprintf(stderr,"Unity bootstrap: first nativeRender returned continue=%d; requesting finishing PAUSE at diagnostic frame boundary\n",int(keep_running));
          else std::fputs("Unity bootstrap: window closed before nativeRender returned; requesting finishing PAUSE\n",stderr);
          if(first_frame){
            auto diagnostic=graphics->DescribeFrameDiagnostics();const bool witness=render_returned&&diagnostic.frame_png&&diagnostic.nonuniform&&diagnostic.draw_submissions&&diagnostic.presented_capture_serial;
            std::fprintf(stderr,"First frame: witness=%d nativeRenderReturned=%d EGLcalls=%llu successfulPresentations=%llu GLcalls=%llu drawSubmissions=%llu clears=%llu PNG=%d nonuniform=%d size=%dx%d captureFailure=%s\n",int(witness),int(render_returned),(unsigned long long)observed_egl_calls.load(),(unsigned long long)observed_presentations.load(),(unsigned long long)diagnostic.calls,(unsigned long long)diagnostic.draw_submissions,(unsigned long long)diagnostic.clear_submissions,int(diagnostic.frame_png),int(diagnostic.nonuniform),diagnostic.width,diagnostic.height,diagnostic.failure.c_str());
            if(witness&&!frame_run.Continuing()){
              std::fputs("First frame: retaining original surface/window for ten seconds before normal diagnostic pause\n",stderr);
              const auto hold_until=std::chrono::steady_clock::now()+std::chrono::seconds(10);bool close=false;
              while(!close&&std::chrono::steady_clock::now()<hold_until){for(const auto &event:native_windows->DrainEvents(window)){if(event.kind==NativeWindows::Event::Focus)lifecycle.WindowFocusChanged(event.focused);if(event.kind==NativeWindows::Event::CloseRequested)close=true;}poll_volume();std::this_thread::sleep_for(std::chrono::milliseconds(10));}
            }
          }
          if(volume)volume->Pause();soft_input->SetFinishing(true);
          lifecycle.BeginPauseNative(true);
          auto finishing_observation=std::chrono::steady_clock::now()+std::chrono::seconds(4);
          while(!lifecycle.PollNativePause()){
            // Retain the actual worker, providers and HWND through nativeDone.
            // Physical events may still arrive, but finishing schedules no new
            // guest focus/surface jobs behind the outstanding nativeDone call.
            soft_input->DrainUiJobs();native_windows->DrainEvents(window);poll_volume();
            if(std::chrono::steady_clock::now()>=finishing_observation){
              std::fputs("Unity bootstrap: original finishing acknowledgment still pending; retaining same UnityMain/window/providers and pumping UI jobs\n",stderr);
              finishing_observation=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }
        }
        stage="surfaceDestroyed/recreate(null)";lifecycle.SurfaceDestroyed();lifecycle.Stop();
        if(soft_input){soft_input->SetNativeLoaded(false);soft_input->CheckIdle();}
        if(egl){egl->SetDiagnosticObserver({});egl->SetProcedureResolver({});}
        init_java_diagnostic_active=false;
        // A playable session raises many original Java exceptions that Unity
        // checks and clears itself (absent optional Android APIs). Each one is
        // already logged where it is raised; report the total, don't fail.
        if(init_java_errors.load())std::fprintf(stderr,"Unity bootstrap: live session observed %zu original Java exception(s), each cleared by the guest; see the per-exception diagnostics\n",init_java_errors.load());
        auto state=lifecycle.Describe();if(state.resumed||state.surface||state.started)throw std::runtime_error("surface lifecycle did not finish paused and detached");
        {std::lock_guard<std::mutex> lock(window_call_mutex);
          const auto from=window_calls["ANativeWindow_fromSurface"],acquire=window_calls["ANativeWindow_acquire"],release=window_calls["ANativeWindow_release"];
          std::fprintf(stderr,"Unity bootstrap: observed guest native-window calls fromSurface=%llu acquire=%llu release=%llu\n",static_cast<unsigned long long>(from),static_cast<unsigned long long>(acquire),static_cast<unsigned long long>(release));
          if(from<2)throw std::runtime_error("real native-window handoff calls missing");
          // A long session may end with the guest still holding a window
          // reference (teardown order); process exit releases it.
          if(release!=from+acquire)std::fprintf(stderr,"Unity bootstrap: guest native-window references unbalanced at exit (held=%lld); released with the process\n",static_cast<long long>(from+acquire)-static_cast<long long>(release));
        }
        native_windows->Close(window);java.DeleteLocalReference(surface);
        if(!frame_gate)std::fprintf(stderr,"Unity bootstrap: isolated surface handoff/detach gate passed on UnityMain; HWND closed after native detach acknowledgment; no nativeRender/nativeResume executed\n");
        else std::fprintf(stderr,"Unity bootstrap: bounded native frame diagnostic completed: rendered=%llu continue=%d nativeDone=%d process-kill-requested=%d; Java class-init/constructor completion, a rendered image and a playable game are not claimed\n",static_cast<unsigned long long>(state.rendered),int(frame_continues),int(state.native_done),int(state.process_kill_requested));
        } catch(const std::exception &error) {
          // Report before lifecycle/provider destruction can wait for workers.
          std::fprintf(stderr,"Unity bootstrap: live surface lifecycle failure: %s; stage=%s\n",error.what(),stage);
          throw;
        }
      }
    }
    if(choreographer){choreographer->Stop();choreographer->CheckHealthy();}
    Require(!threads.Shutdown(),"actual guest workers did not drain before JNI/module teardown");
#if defined(BUNKER_GAME_COLLECTOR_DELIVERY) && BUNKER_GAME_COLLECTOR_DELIVERY
    if(collector)collector->CloseAfterShutdown();
    if(main_collector)main_collector->Close();
    if(worker_collector)worker_collector->Close();
#endif
    preferences->CheckPersistence();
    if(auto pending=tls_audit.PendingModuleCallbacks(threads)){
      // A real callback must run before JNI/module globals are retired. If
      // natural guest code already retired an owner, do not invoke stale code.
      Require(!unity_ledger.finalized&&!il2cpp_ledger.finalized,"real UI TLS survives an already finalized module; safe callback lifetime is unproved");
      Require(!threads.ReleaseCurrent(),"actual UI TLS retirement failed");preferences->CheckPersistence();if(jumps)jumps->DropThread(parent_token,parent_token);Require(!process_signals.DropThread(parent_token),"actual UI signal retirement failed");native_catalog->Revoke();
      std::fprintf(stderr,"Game bootstrap: reached teardown boundary: %zu real UI TLS destructor(s) retired before JNI/global teardown; modules retained until process exit, Unity refs=%zu IL2CPP refs=%zu; no forced finalizers/reuse claim\n",pending,modules.ReferenceCount("libunity.so"),modules.ReferenceCount("libil2cpp.so"));return 7;
    }
    stage="JNI_OnUnload";auto onunload=modules.Symbol(module,"JNI_OnUnload");if(onunload)threads.Run(onunload,{java.VmAddress(),0});
    preferences->CheckPersistence();
    Require(!tls_audit.PendingModuleCallbacks(threads),"JNI_OnUnload created UI TLS requiring an unproved teardown order");
    stage="interface proxy ownership before module finalizers";
    try{interface_proxies->CheckNoEnabledProxies();}catch(const std::exception &error){
      std::fprintf(stderr,"Game bootstrap: retained-module proxy ownership boundary: %s; Unity refs=%zu IL2CPP refs=%zu; no native delete/finalizer forced\n",error.what(),modules.ReferenceCount("libunity.so"),modules.ReferenceCount("libil2cpp.so"));return 8;
    }
    // A live play session ends like an Android app process: it is killed
    // after the activity finishes, so guest static finalizers never run
    // while engine threads/state from the session are still in place.
    if(frame_gate){
      preferences->CheckPersistence();
      std::fprintf(stderr,"Game bootstrap: live session ended; process exits without guest static finalizers, as Android kills the finished app process\n");
      std::fflush(nullptr);std::_Exit(frame_continues?0:6);
    }
    stage="finalizers";if(modules.Close(module))throw std::runtime_error("Unity module finalization failed");
    preferences->CheckPersistence();
    interface_proxies->CheckNoEnabledProxies();
    std::fprintf(stderr,"Game bootstrap: actual lazy IL2CPP lifecycle constructors=%zu finalizers=%zu remaining refs=%zu; no host-forced guest handle close\n",il2cpp_ledger.constructed,il2cpp_ledger.finalized,modules.ReferenceCount("libil2cpp.so"));
    Require(!tls_audit.PendingModuleCallbacks(threads),"actual module finalizers created surviving UI TLS; callback lifetime is unproved");
    if(modules.ReferenceCount("libunity.so")||modules.ReferenceCount("libil2cpp.so")){
      Require(!tls_audit.PendingModuleCallbacks(threads),"module finalization created UI TLS requiring an unproved teardown order");
      Require(!threads.ReleaseCurrent(),"actual UI retirement with retained modules failed");preferences->CheckPersistence();if(jumps)jumps->DropThread(parent_token,parent_token);Require(!process_signals.DropThread(parent_token),"actual UI signal retirement with retained modules failed");native_catalog->Revoke();
      std::fprintf(stderr,"Game bootstrap: reached teardown boundary: actual module references remain Unity=%zu IL2CPP=%zu; globals retained until process exit, no host-forced finalizers\n",modules.ReferenceCount("libunity.so"),modules.ReferenceCount("libil2cpp.so"));return 7;
    }
    libc.Finalize();interface_proxies->CheckNoEnabledProxies();Require(!tls_audit.PendingModuleCallbacks(threads),"actual global finalizers created surviving UI TLS; callback lifetime is unproved");const auto current_token=parent_token;auto released=threads.ReleaseCurrent();
    preferences->CheckPersistence();
    if(jumps)jumps->DropThread(current_token,current_token);
    if(process_signals.Lookup(current_token)&&process_signals.DropThread(current_token))throw std::runtime_error("parent signal-thread cleanup failed");
    native_catalog->Revoke();
    if(released)throw std::runtime_error("main runner TLS/stack cleanup failed");
    if(frame_gate&&!frame_continues){std::fprintf(stderr,"Unity bootstrap: nativeRender returned false/requested Activity finish; one-frame startup was not successful\n");return 6;}
    std::fprintf(stderr,"Game bootstrap: normal profile completion=%s; original constructors/JNI, lifecycle and ownership checks complete\n",frame_gate?(collector_delivery?"frame-muted-gc":mute_audio?"frame-muted":"frame"):surface_gate?"surface":init_jni?"init":"default");return 0;
  }catch(const std::exception &error){std::fprintf(stderr,"Unity bootstrap: %s; stage=%s completed constructors=%zu\n",error.what(),stage,constructor_index);return 1;}
}
