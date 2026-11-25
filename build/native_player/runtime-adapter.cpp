#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "frame-memory.h"
#include "unity-object.h"
#include "../../runtime/fast_tls.h"
#include <map>
#include <unordered_set>
#include <string>
static std::map<std::string,void *> registry;
static void boundary(const char *name) {
    wchar_t path[32768]{};
    auto length=GetModuleFileNameW(nullptr,path,32768);
    if (!length||length==32768) std::abort();
    std::wstring log(path,length);
    log=log.substr(0,log.find_last_of(L"\\/")+1)+L"boundary.txt";
    FILE *f=_wfopen(log.c_str(),L"w");
    if(f){std::fprintf(f,"Next native Unity player call %s. Registered engine callbacks %zu.\n",name,registry.size());std::fclose(f);}
    ExitProcess(101);
}
extern "C" __declspec(dllexport) void il2cpp_add_internal_call(const char *name,void *callback) {
    registry[name]=callback;
}
extern "C" __declspec(dllexport) void VisitRegisteredCalls(void(*visitor)(const char *,void *)) {
    for(auto &entry:registry) visitor(entry.first.c_str(),entry.second);
}
extern "C" __declspec(dllexport) void UnimplementedCallback(const char *name){boundary(name);}
using Invoke=uint64_t(*)(const char *,const uint64_t *,size_t);
template<class... Args> static uint64_t invoke(const char *name,Args... args) {
    static Invoke service=[] {
        auto dll=LoadLibraryW(L"GuestRuntime.dll");
        return dll?reinterpret_cast<Invoke>(GetProcAddress(dll,"InvokeOriginal")):nullptr;
    }();
    if (!service) boundary("GuestRuntime.dll::InvokeOriginal");
    std::array<uint64_t,sizeof...(Args)> words{(uint64_t)args...};
    return service(name,words.data(),words.size());
}
#define FORWARD1(name,t1) extern "C" __declspec(dllexport) void name(t1 a) {invoke(#name,a);}
#define FORWARD3(name,t1,t2,t3) extern "C" __declspec(dllexport) void name(t1 a,t2 b,t3 c) {invoke(#name,a,b,c);}
using BoundCall=uint64_t(*)(const char *,std::atomic<uint64_t> *,const uint64_t *,size_t);
static BoundCall boundService(){
    static auto service=reinterpret_cast<BoundCall>(GetProcAddress(LoadLibraryW(L"GuestRuntime.dll"),"InvokeBound"));
    if(!service)boundary("GuestRuntime.dll::InvokeBound");
    return service;
}
template<class... Args> static uint64_t bound(const char *name,std::atomic<uint64_t> &entry,Args... args){
    std::array<uint64_t,sizeof...(Args)> words{(uint64_t)args...};
    return boundService()(name,&entry,words.data(),words.size());
}
// Each ordinary export resolves its own symbol once. Exports adapting
// callbacks, detaching threads, or writing managed fields use invoke directly.
#define R0(name,result) extern "C" __declspec(dllexport) result name() {static std::atomic<uint64_t> entry{};return (result)bound(#name,entry);}
#define R1(name,result,t1) extern "C" __declspec(dllexport) result name(t1 a) {static std::atomic<uint64_t> entry{};return (result)bound(#name,entry,a);}
#define R2(name,result,t1,t2) extern "C" __declspec(dllexport) result name(t1 a,t2 b) {static std::atomic<uint64_t> entry{};return (result)bound(#name,entry,a,b);}
#define R3(name,result,t1,t2,t3) extern "C" __declspec(dllexport) result name(t1 a,t2 b,t3 c) {static std::atomic<uint64_t> entry{};return (result)bound(#name,entry,a,b,c);}
#define R4(name,result,t1,t2,t3,t4) extern "C" __declspec(dllexport) result name(t1 a,t2 b,t3 c,t4 d) {static std::atomic<uint64_t> entry{};return (result)bound(#name,entry,a,b,c,d);}
#define R5(name,result,t1,t2,t3,t4,t5) extern "C" __declspec(dllexport) result name(t1 a,t2 b,t3 c,t4 d,t5 e) {static std::atomic<uint64_t> entry{};return (result)bound(#name,entry,a,b,c,d,e);}
using P=void *;
// The build verifies each original getter is exactly one load (or ADD) and RET.
// Each thread also checks its first result through the unmodified game API.
template<unsigned width, unsigned offset> static uintptr_t readField(P object) {
    auto address=reinterpret_cast<uintptr_t>(object)+offset;
    if constexpr(width==0)return address;
    uintptr_t result=0;
    std::memcpy(&result,reinterpret_cast<const void *>(address),width);
    return result;
}
struct LeafCheckTag; static constexpr recomp::ThreadLocalRef<uint64_t,LeafCheckTag> checkedLeaves{};
#define LEAF(name,result,width,offset,index) extern "C" __declspec(dllexport) result name(P object) { \
    constexpr auto bit=uint64_t(1)<<index; \
    auto field=readField<width,offset>(object); \
    if(!(checkedLeaves.get()&bit)){ if(field!=invoke(#name,object))boundary("Metadata accessor mismatch: " #name); checkedLeaves|=bit; } \
    return (result)field; }
using LiveObjects=void(*)(P *,int,P);
using WorldChange=void(*)();
// The pinned IL2CPP constructor uses the hint only to reserve storage. It
// allocates four slots per unit for visited objects and one for pending work.
// Growing either list mid-walk restarts the world and then stops it again.
static std::atomic<int> livenessReserve{};
static const bool referenceLivenessSize=std::getenv("REINCOMP_REFERENCE_LIVENESS")!=nullptr;
extern "C" __declspec(dllexport) P il2cpp_unity_liveness_calculation_begin(P filter,int capacity,LiveObjects objects,
                                                                           P user,WorldChange started,WorldChange stopped){
    if(!referenceLivenessSize && capacity>=0)
        capacity=std::max(capacity,livenessReserve.load(std::memory_order_relaxed));
    return reinterpret_cast<P>(invoke("il2cpp_unity_liveness_calculation_begin",filter,capacity,objects,user,started,stopped));
}
extern "C" __declspec(dllexport) void il2cpp_unity_liveness_calculation_end(P state){
    if(state && !referenceLivenessSize){
        auto visited=reinterpret_cast<P>(readField<8,0x08>(state));
        auto pending=reinterpret_cast<P>(readField<8,0x18>(state));
        auto visitedCount=readField<8,0x08>(visited);
        // The pending list is empty at completion. Its capacity records growth.
        auto pendingCapacity=readField<8,0x10>(pending) & ~(uintptr_t(1)<<63);
        constexpr uintptr_t limit=262144; // The learned hint reserves at most 10 MiB on x64.
        auto required=static_cast<int>(std::min(limit,std::max((visitedCount+3)/4,pendingCapacity)));
        auto observed=livenessReserve.load(std::memory_order_relaxed);
        while(observed<required && !livenessReserve.compare_exchange_weak(observed,required,std::memory_order_relaxed)){}
    }
    static std::atomic<uint64_t> entry{};
    bound("il2cpp_unity_liveness_calculation_end",entry,state);
}
R2(il2cpp_unity_liveness_calculation_from_root,void,P,P)
R1(il2cpp_unity_liveness_calculation_from_statics,void,P)
R0(il2cpp_class_get_userdata_offset,int32_t)
R0(il2cpp_domain_get,P)
extern "C" __declspec(dllexport) void il2cpp_shutdown(){
    frame_memory::shutdown();
    if(auto flush=reinterpret_cast<void(*)()>(GetProcAddress(GetModuleHandleW(L"EngineCalls.dll"),"FlushCheckpointPreferences")))flush();
    if(auto stop=reinterpret_cast<void(*)()>(GetProcAddress(GetModuleHandleW(L"EngineCalls.dll"),"ShutdownDesktopGamepads")))stop();
    invoke("il2cpp_shutdown");
}
R0(il2cpp_get_corlib,P)
R0(il2cpp_thread_current,P)
R0(il2cpp_object_header_size,uint32_t)
R0(il2cpp_array_object_header_size,uint32_t)
R0(il2cpp_allocation_granularity,uint32_t)
R0(il2cpp_offset_of_array_bounds_in_array_object_header,uint32_t)
R0(il2cpp_offset_of_array_length_in_array_object_header,uint32_t)
R0(il2cpp_gc_is_incremental,bool)
R0(il2cpp_gc_is_disabled,bool)
R0(il2cpp_gc_has_strict_wbarriers,bool)
R1(il2cpp_assembly_get_image,P,P)
R1(il2cpp_image_get_name,const char *,P)
R1(il2cpp_image_get_class_count,size_t,P)
R1(il2cpp_class_get_name,const char *,P)
R1(il2cpp_class_get_namespace,const char *,P)
R1(il2cpp_class_get_parent,P,P)
R1(il2cpp_class_get_type,P,P)
R1(il2cpp_class_instance_size,int32_t,P)
R1(il2cpp_class_get_element_class,P,P)
R1(il2cpp_class_get_image,P,P)
R1(il2cpp_type_get_type,int,P)
R1(il2cpp_type_get_object,P,P)
R1(il2cpp_thread_attach,P,P)
R1(il2cpp_method_get_name,const char *,P)
R1(il2cpp_method_get_param_count,uint32_t,P)
R1(il2cpp_method_get_return_type,P,P)
R2(il2cpp_domain_get_assemblies,P,P,size_t *)
R2(il2cpp_domain_assembly_open,P,P,const char *)
R2(il2cpp_class_get_methods,P,P,P *)
R2(il2cpp_class_get_fields,P,P,P *)
R2(il2cpp_class_get_field_from_name,P,P,const char *)
R2(il2cpp_class_is_assignable_from,bool,P,P)
R2(il2cpp_class_set_userdata,void,P,P)
R2(il2cpp_method_get_param,P,P,uint32_t)
R2(il2cpp_image_get_class,P,P,size_t)
R3(il2cpp_class_from_name,P,P,const char *,const char *)
R3(il2cpp_class_get_method_from_name,P,P,const char *,int)
R2(il2cpp_class_get_nested_types,P,P,P *)
R2(il2cpp_class_get_interfaces,P,P,P *)
R1(il2cpp_class_get_flags,int,P)
R1(il2cpp_class_get_type_token,uint32_t,P)
R1(il2cpp_class_get_declaring_type,P,P)
R1(il2cpp_class_is_generic,bool,P)
R1(il2cpp_class_is_inflated,bool,P)
R1(il2cpp_class_is_enum,bool,P)
R1(il2cpp_class_is_interface,bool,P)
R1(il2cpp_class_is_abstract,bool,P)
R1(il2cpp_class_is_blittable,bool,P)
R1(il2cpp_class_is_valuetype,bool,P)
R1(il2cpp_class_has_references,bool,P)
R1(il2cpp_class_from_il2cpp_type,P,P)
R1(il2cpp_class_from_type,P,P)
R1(il2cpp_class_from_system_type,P,P)
R2(il2cpp_class_value_size,int32_t,P,uint32_t *)
R2(il2cpp_class_has_attribute,bool,P,P)
R3(il2cpp_class_is_subclass_of,bool,P,P,bool)
R1(il2cpp_method_is_generic,bool,P)
R1(il2cpp_method_is_inflated,bool,P)
R1(il2cpp_method_is_instance,bool,P)
R1(il2cpp_method_get_class,P,P)
R1(il2cpp_method_get_declaring_type,P,P)
R1(il2cpp_method_get_token,uint32_t,P)
R2(il2cpp_method_get_flags,uint32_t,P,uint32_t *)
R2(il2cpp_method_has_attribute,bool,P,P)
R1(il2cpp_field_get_name,const char *,P)
R1(il2cpp_field_get_type,P,P)
R1(il2cpp_field_get_flags,int,P)
R1(il2cpp_field_get_offset,size_t,P)
R2(il2cpp_field_has_attribute,bool,P,P)
R1(il2cpp_type_get_name,char *,P)
R1(il2cpp_type_get_class_or_element_class,P,P)
R1(il2cpp_type_is_byref,bool,P)
R1(il2cpp_type_get_attrs,uint32_t,P)
R1(il2cpp_alloc,P,size_t)
R1(il2cpp_free,void,P)
R1(il2cpp_runtime_class_init,void,P)
#include "frame-cpu.h"
#include "native-frame-log.h"
#include "managed-profile.h"
#ifdef REINCOMP_BUNKER_MODULES
#include "outgame-header.h"
#endif

// The Android player draws screen-space overlays into the encoded backbuffer
// after the camera pipeline. Let Unity track the write mode so its canvas
// renderer uses the same blend space for masks, artwork, and text.
static void useOverlayColorSpace(bool overlay) {
    static auto getWrite = reinterpret_cast<uint8_t(*)()>(registry.at("UnityEngine.GL::get_sRGBWrite"));
    static auto setWrite = reinterpret_cast<void(*)(uint8_t)>(registry.at("UnityEngine.GL::set_sRGBWrite"));
    static bool restoreWrite = false;
    if (overlay) {
        if (getWrite()) {
            setWrite(false);
            restoreWrite = true;
        }
    } else if (restoreWrite) {
        setWrite(true);
        restoreWrite = false;
    }
}

extern "C" __declspec(dllexport) P il2cpp_runtime_invoke(P method,P object,P *args,P *exception) {
    static FILE *trace=[] {
        if(!GetEnvironmentVariableW(L"REINCOMP_NATIVE_TRACE_CALLS",nullptr,0))return static_cast<FILE *>(nullptr);
        wchar_t buf[32768]{};auto n=GetModuleFileNameW(nullptr,buf,32768);
        std::wstring path(buf,n);
        path=path.substr(0,path.find_last_of(L"\\/")+1)+L"managed-methods.txt";
        return _wfopen(path.c_str(),L"w");
    }();
    // IL2CPP owns these MethodInfo objects for the lifetime of the domain.
    static const bool paced=GetEnvironmentVariableW(L"REINCOMP_NATIVE_PRESENT_RATE",nullptr,0)!=0;
    static const bool walk=GetEnvironmentVariableW(L"REINCOMP_NATIVE_AUTO_WALK",nullptr,0)!=0;
    static std::atomic<P> frameMethod{},titleMethod{},pipelineMethod{},autoStart{};
    auto frameCallback=frameMethod.load(std::memory_order_relaxed);
    auto titleCallback=titleMethod.load(std::memory_order_relaxed);
    auto pipelineCallback=pipelineMethod.load(std::memory_order_relaxed);
    auto walkCallback=walk?autoStart.load(std::memory_order_relaxed):nullptr;
    if(method && (trace || !frameCallback || !titleCallback || !pipelineCallback || (walk && !walkCallback))){
      using Methods=std::unordered_set<P>;
      struct MethodsTag; static constexpr recomp::ThreadLocalRef<Methods *,MethodsTag> cached{};
      auto *known=cached.get();
      if(!known){static thread_local Methods local;known=&local;cached=known;}
      if(known->insert(method).second){
        const char *name=il2cpp_method_get_name(method);
        auto klass=il2cpp_method_get_class(method);
        std::string type=std::string(il2cpp_class_get_namespace(klass))+"."+il2cpp_class_get_name(klass);
        if(type=="Dark.View.Title.TitleScreen" && !std::strcmp(name,"Start")){
            titleCallback=method;
            titleMethod.store(method,std::memory_order_relaxed);
        }
        if(type=="UnityEngine.SendMouseEvents" && !std::strcmp(name,"DoSendMouseEvents")){
            frameCallback=method;
            frameMethod.store(method,std::memory_order_relaxed);
        }
        if(type=="UnityEngine.Rendering.RenderPipelineManager" && !std::strcmp(name,"DoRenderLoop_Internal")){
            pipelineCallback=method;
            pipelineMethod.store(method,std::memory_order_relaxed);
        }
        if(walk && type=="Dark.View.HeadUpDisplay.Menu.AutoRunButtonAction" && !std::strcmp(name,"Start")){
            walkCallback=method;
            autoStart.store(method,std::memory_order_relaxed);
        }
        if(trace){std::fprintf(trace,"%s::%s\n",type.c_str(),name);std::fflush(trace);}
      }
    }
    if(method && method==titleCallback){
#ifdef REINCOMP_BUNKER_MODULES
        prepareBunkerMenuCallbacks();
#endif
        titleStarted=std::chrono::steady_clock::now();
        if(GetEnvironmentVariableW(L"REINCOMP_ROUTE_CHECKPOINT",nullptr,0)){
            if(routeTitleHandle)invoke("il2cpp_gchandle_free",routeTitleHandle);
            routeTitleHandle=static_cast<uint32_t>(invoke("il2cpp_gchandle_new_weakref",object,false));
        }
    }
#ifdef REINCOMP_BUNKER_MODULES
    if(object && method && method==subquest_list::onDisable.load(std::memory_order_acquire))
        subquest_list::disable(object);
    if(skipUnchangedCampaignBadge(method,object)){
        if(exception)*exception=nullptr;
        return nullptr;
    }
#endif
    static const bool profile=GetEnvironmentVariableW(L"REINCOMP_PROFILE_MANAGED",nullptr,0)!=0;
    const bool cameraPass = method && method==pipelineCallback;
    if(cameraPass || (method && method==frameCallback))useOverlayColorSpace(false);
    if(method && method==frameCallback){
        observePlayerFrame();
        if(profile)managed_profile::frame();
    }
    using ManagedCall=uint64_t(*)(const uint64_t *);
    static auto managed=reinterpret_cast<ManagedCall>(GetProcAddress(LoadLibraryW(L"GuestRuntime.dll"),"InvokeManaged"));
    if(!managed)boundary("GuestRuntime.dll::InvokeManaged");
    const uint64_t arguments[]={uintptr_t(method),uintptr_t(object),uintptr_t(args),uintptr_t(exception)};
    auto call=[&]{return trace?(P)invoke("il2cpp_runtime_invoke",method,object,args,exception):reinterpret_cast<P>(managed(arguments));};
#ifdef REINCOMP_BUNKER_MODULES
    if(method && method==flyingMomUpdate.load(std::memory_order_acquire))
        return updateFlyingMom(object,call,exception);
#endif
    const bool recordRender=frame_cpu::enabled && cameraPass;
    if(recordRender)frame_cpu::beginRender();
    auto result=profile?managed_profile::measure(method,object,call):call();
#ifdef REINCOMP_BUNKER_MODULES
    if(object && method && method==subquest_list::lateUpdate.load(std::memory_order_acquire) && (!exception || !*exception))
        subquest_list::update(object);
    if(method && method==headerStart.load(std::memory_order_acquire) && (!exception || !*exception)){
        if(desktopHeaderReady)desktop_header_start(object);
        else applyBunkerSeasons();
    }
    if(method && method==headerUpdate.load(std::memory_order_acquire) && (!exception || !*exception)){
        desktop_header_update(object);
        fitOutgameHeader();
    }
    if(method && method==battleHudUpdate.load(std::memory_order_acquire) && (!exception || !*exception))
        desktop_battle_update(object);
#endif
    if(recordRender)frame_cpu::finishRender();
    if(walk && !autoDelivered && method && method==walkCallback && (!exception || !*exception)){
        if(routeAutoHandle)invoke("il2cpp_gchandle_free",routeAutoHandle);
        routeAutoHandle=static_cast<uint32_t>(invoke("il2cpp_gchandle_new_weakref",object,false));
    }
    if(cameraPass && (!exception || !*exception)){
        useOverlayColorSpace(true);
        if(paced){
            static auto finish=reinterpret_cast<void(*)()>(GetProcAddress(GetModuleHandleW(L"EngineCalls.dll"),"AfterRenderPipeline"));
            if(finish)finish();
        }
    }
    return result;
}
R5(il2cpp_runtime_invoke_convert_args,P,P,P,P *,int,P *)
R1(il2cpp_object_new,P,P)
R1(il2cpp_object_get_class,P,P)
R1(il2cpp_object_unbox,P,P)
R1(il2cpp_gchandle_get_target,P,uint32_t)
R2(il2cpp_gchandle_new,uint32_t,P,bool)
R2(il2cpp_gchandle_new_weakref,uint32_t,P,bool)
R1(il2cpp_gchandle_free,void,uint32_t)
R1(il2cpp_string_new,P,const char *)
R2(il2cpp_string_new_utf16,P,const uint16_t *,int)
R1(il2cpp_string_chars,uint16_t *,P)
R1(il2cpp_string_length,int,P)
FORWARD3(il2cpp_gc_wbarrier_set_field,P,P *,P)
R1(il2cpp_gc_collect,void,int)
R0(il2cpp_gc_collect_a_little,int)
R0(il2cpp_gc_get_heap_size,int64_t)
R0(il2cpp_gc_get_used_size,int64_t)
R0(il2cpp_gc_get_max_time_slice_ns,int64_t)
R1(il2cpp_gc_set_max_time_slice_ns,void,int64_t)
R0(il2cpp_gc_disable,void)
R0(il2cpp_gc_enable,void)
R2(il2cpp_array_new,P,P,size_t)
R2(il2cpp_array_new_specific,P,P,size_t)
R2(il2cpp_array_class_get,P,P,uint32_t)
R1(il2cpp_array_length,uint32_t,P)

// Unity 2019's GetComponentsForListInternal clears the entire unused capacity
// of the output List<T> one write barrier at a time. The UI pools these lists,
// so a large event menu makes even single-component queries expensive later.
extern "C" __declspec(dllexport) bool FillLargeComponentList(P owner, P type, P destination) {
    // These are the List<T> and one-dimensional reference array layouts used
    // directly by the pinned Windows player's component-list implementation.
    struct Array { P klass, monitor, bounds; size_t length; };
    struct List { P klass, monitor; Array *items; int32_t size; uint32_t version; };
    auto list = static_cast<List *>(destination);
    if (!list || !list->items || list->items->length <= 64 || !type || !nativeUnityObjectAlive(owner)) return false;
    static auto gameObject = reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.Component::get_gameObject"));
    static auto components = reinterpret_cast<Array *(*)(P, P, uint8_t, uint8_t, uint8_t, uint8_t, P)>(
        registry.at("UnityEngine.GameObject::GetComponentsInternal"));
    // Use the engine's own single-object search, including interface matches
    // and component ordering. Leave capacity growth to the ordinary list path.
    auto result = components(gameObject(owner), type, true, false, true, false, nullptr);
    if (!result || result->length > list->items->length) return false;
    auto slots = reinterpret_cast<P *>(list->items + 1);
    auto matches = reinterpret_cast<P *>(result + 1);
    int count = static_cast<int>(result->length);
    for (int i = 0; i < count; ++i) il2cpp_gc_wbarrier_set_field(list->items, slots + i, matches[i]);
    // Slots beyond the old Count are already empty. Clear only live references
    // displaced by the query, preserving both the list and its backing array.
    for (int i = count; i < list->size; ++i) il2cpp_gc_wbarrier_set_field(list->items, slots + i, nullptr);
    list->size = count;
    ++list->version;
    return true;
}
R1(il2cpp_array_element_size,int,P)
R2(il2cpp_value_box,P,P,P)
R1(il2cpp_object_get_size,uint32_t,P)
R3(il2cpp_field_set_value,void,P,P,P)
R3(il2cpp_field_get_value,void,P,P,P)
R2(il2cpp_field_static_set_value,void,P,P)
R2(il2cpp_field_static_get_value,void,P,P)
R2(il2cpp_method_get_object,P,P,P)
R1(il2cpp_method_get_from_reflection,P,P)
R2(il2cpp_object_get_virtual_method,P,P,P)
R2(il2cpp_runtime_object_init_exception,void,P,P *)
R1(il2cpp_runtime_object_init,void,P)
R1(il2cpp_string_new_wrapper,P,const char *)
R2(il2cpp_string_new_len,P,const char *,uint32_t)
R1(il2cpp_string_intern,P,P)
R1(il2cpp_string_is_interned,P,P)
R1(il2cpp_array_get_byte_length,uint32_t,P)
R1(il2cpp_class_num_fields,size_t,P)
R1(il2cpp_class_get_rank,int,P)
R1(il2cpp_class_get_data_size,uint32_t,P)
R1(il2cpp_class_get_assemblyname,const char *,P)
R1(il2cpp_class_get_static_field_data,P,P)
R1(il2cpp_class_enum_basetype,P,P)
R2(il2cpp_class_get_properties,P,P,P *)
R2(il2cpp_class_get_property_from_name,P,P,const char *)
R2(il2cpp_class_get_events,P,P,P *)
R2(il2cpp_class_has_parent,bool,P,P)
R1(il2cpp_field_get_parent,P,P)
R1(il2cpp_field_is_literal,bool,P)
R1(il2cpp_image_get_assembly,P,P)
R1(il2cpp_image_get_filename,const char *,P)
R1(il2cpp_image_get_entry_point,P,P)
R1(il2cpp_property_get_get_method,P,P)
R1(il2cpp_property_get_set_method,P,P)
R1(il2cpp_property_get_name,const char *,P)
R1(il2cpp_property_get_parent,P,P)
R1(il2cpp_property_get_flags,uint32_t,P)
R2(il2cpp_type_equals,bool,P,P)
R1(il2cpp_type_is_pointer_type,bool,P)
R1(il2cpp_type_is_static,bool,P)
R1(il2cpp_type_get_assembly_qualified_name,char *,P)
R1(il2cpp_is_vm_thread,bool,P)
R0(il2cpp_is_debugger_attached,bool)
FORWARD1(il2cpp_thread_detach,P)
R1(il2cpp_thread_get_all_attached_threads,P,size_t *)
R1(il2cpp_get_exception_argument_null,P,const char *)
R3(il2cpp_format_exception,void,P,char *,int)
R3(il2cpp_format_stack_trace,void,P,char *,int)
R1(il2cpp_custom_attrs_from_class,P,P)
R1(il2cpp_custom_attrs_from_method,P,P)
R1(il2cpp_custom_attrs_construct,P,P)
R1(il2cpp_custom_attrs_free,void,P)
R2(il2cpp_custom_attrs_get_attr,P,P,P)
R2(il2cpp_custom_attrs_has_attr,bool,P,P)
R3(il2cpp_array_new_full,P,P,size_t *,size_t *)
R3(il2cpp_bounded_array_class_get,P,P,uint32_t,bool)
R1(il2cpp_class_array_element_size,int,P)
R2(il2cpp_method_get_param_name,const char *,P,uint32_t)
R2(il2cpp_field_get_value_object,P,P,P)
R3(il2cpp_field_set_value_object,void,P,P,P)
R4(il2cpp_exception_from_name_msg,P,P,const char *,const char *,const char *)
R1(il2cpp_unhandled_exception,void,P)
R1(il2cpp_monitor_enter,void,P)
R1(il2cpp_monitor_exit,void,P)
R1(il2cpp_monitor_pulse,void,P)
R1(il2cpp_monitor_pulse_all,void,P)
R1(il2cpp_monitor_wait,void,P)
R2(il2cpp_monitor_try_enter,bool,P,uint32_t)
R2(il2cpp_monitor_try_wait,bool,P,uint32_t)
FORWARD1(il2cpp_runtime_unhandled_exception_policy_set,int)
FORWARD3(il2cpp_set_commandline_arguments,int,const char **,const char *)
FORWARD3(il2cpp_set_commandline_arguments_utf16,int,const uint16_t **,const char *)
FORWARD1(il2cpp_set_config_dir,const char *)
extern "C" __declspec(dllexport) void il2cpp_set_data_dir(const char *path) {
    if (!path) boundary("il2cpp_set_data_dir(null)");
    std::string source(path);
    const auto split=source.find_last_of("/\\");
    if (source.substr(split+1)!="il2cpp_data") boundary("il2cpp_set_data_dir(unrecognized source)");
    const bool checkpoint=GetEnvironmentVariableW(L"REINCOMP_NATIVE_CHECKPOINT",nullptr,0)!=0;
    invoke("il2cpp_set_data_dir",checkpoint?"/data/user/0/bunker/files/il2cpp":"/data/user/0/bunker/Managed");
}
FORWARD1(il2cpp_set_temp_dir,const char *)
FORWARD1(il2cpp_set_config,const char *)
FORWARD1(il2cpp_set_config_utf16,const uint16_t *)
FORWARD1(il2cpp_unity_install_unitytls_interface,const void *)
extern "C" __declspec(dllexport) void il2cpp_set_find_plugin_callback(void *) {
    // P/Invokes in this assembly target Android modules. The original runtime's
    // default resolver preserves the UTF-8 name and loads it via guest dlopen.
    invoke("il2cpp_set_find_plugin_callback",nullptr);
}
FORWARD1(il2cpp_register_log_callback,void *)
FORWARD1(il2cpp_debugger_set_agent_options,const char *)
#include "verify-windows-roots.h"
#include "inspect-enum-layouts.h"
#include "inspect-movie-metadata.h"
static void checkStoreCost(){
    auto compare=reinterpret_cast<void(*)(bool)>(GetProcAddress(GetModuleHandleW(L"GuestRuntime.dll"),"CompareOriginalStore"));
    if(!compare)boundary("field store comparison unavailable");
    P objects[]={il2cpp_string_new("store A"),il2cpp_string_new("store B")};
    P slots[]={objects[1],nullptr,objects[0]};
    constexpr unsigned n=40000;
    std::vector<double> original,fast;
    for(unsigned pass=0;pass<10;++pass){
        bool reference=pass%4==0||pass%4==3;
        compare(reference);
        auto start=std::chrono::steady_clock::now();
        for(unsigned i=0;i<n;++i){
            il2cpp_gc_wbarrier_set_field(nullptr,&slots[1],objects[i%2]);
            if(slots[1]!=objects[i%2])boundary("managed field store value differs");
        }
        auto elapsed=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/n;
        (reference?original:fast).push_back(elapsed);
    }
    compare(false);
    if(slots[0]!=objects[1]||slots[2]!=objects[0])boundary("field store overwrote an adjacent pointer");
    std::sort(original.begin(),original.end());std::sort(fast.begin(),fast.end());
    if(auto f=std::fopen("field-store-check.txt","w")){
        std::fprintf(f,"Same-thread median per field assignment: translated %.1f ns, direct %.1f ns, %.2fx. %u total assignments verified, adjacent sentinels intact.\n",
            original[original.size()/2],fast[fast.size()/2],original[original.size()/2]/fast[fast.size()/2],10*n);
        std::fclose(f);
    }
}
struct LiveCheck {std::vector<P> visited;std::vector<int> batches;};
static bool worldStopped=false;
static unsigned stops=0,starts=0;
static void testWorldStopped(){worldStopped=true;++stops;}
static void testWorldStarted(){worldStopped=false;++starts;}
static void inspectLiveObjects(P *objects,int count,P data){
    auto &check=*static_cast<LiveCheck *>(data);
    check.visited.insert(check.visited.end(),objects,objects+count);
    check.batches.push_back(count);
}
static void verifyLiveness(){
    auto walkCheck=reinterpret_cast<unsigned(*)()>(GetProcAddress(GetModuleHandleW(L"GuestRuntime.dll"),"CheckLivenessDescriptors"));
    const unsigned descriptors=walkCheck?walkCheck():0;
    auto markCheck=reinterpret_cast<bool(*)()>(GetProcAddress(GetModuleHandleW(L"GuestRuntime.dll"),"CheckLivenessMarks"));
    const bool matchedMarks=markCheck && markCheck();
    auto type=il2cpp_class_from_name(il2cpp_get_corlib(),"System","Object");
    constexpr size_t count=6000; // Exceeds the original 4,000-slot initial list.
    P root=il2cpp_array_new(type,count+2),unrelated=il2cpp_array_new(type,1);
    const auto header=il2cpp_array_object_header_size();
    auto slots=[header](P array){return reinterpret_cast<P *>(static_cast<char *>(array)+header);};
    for(size_t i=0;i<count;++i)il2cpp_gc_wbarrier_set_field(root,slots(root)+i,il2cpp_array_new(type,1));
    il2cpp_gc_wbarrier_set_field(root,slots(root)+count,slots(root)[0]); // duplicate followed by a null
    il2cpp_gc_wbarrier_set_field(slots(root)[0],slots(slots(root)[0]),root); // cycle
    auto referenceRoot=reinterpret_cast<void(*)(P,P)>(GetProcAddress(GetModuleHandleW(L"GuestRuntime.dll"),"ReferenceLivenessRoot"));
    if(!referenceRoot)boundary("missing original liveness walker");
    bool stoppedOnBegin=true;
    auto visit=[&](bool original){
        LiveCheck check;
        check.visited.reserve(count+2);check.batches.reserve(count+2);
        auto state=original?reinterpret_cast<P>(invoke("il2cpp_unity_liveness_calculation_begin",
            type,32,inspectLiveObjects,&check,testWorldStarted,testWorldStopped)):
            il2cpp_unity_liveness_calculation_begin(type,32,inspectLiveObjects,&check,testWorldStarted,testWorldStopped);
        stoppedOnBegin=stoppedOnBegin&&worldStopped;
        if(original)referenceRoot(root,state);
        else il2cpp_unity_liveness_calculation_from_root(root,state);
        il2cpp_unity_liveness_calculation_end(state);
        return check;
    };
    auto original=visit(true);
    const auto referenceStops=stops;
    auto actual=visit(false);
    const auto reservedStops=stops-referenceStops;
    bool ordered=actual.visited==original.visited && actual.batches==original.batches;
    std::unordered_set<P> found(actual.visited.begin(),actual.visited.end());
    bool reachable=found.count(root)&&!found.count(unrelated)&&found.size()==count+1;
    for(size_t i=0;i<count;++i)reachable=reachable&&found.count(slots(root)[i]);
    bool passed=matchedMarks&&descriptors==512&&ordered&&reachable&&stoppedOnBegin&&!worldStopped&&starts==stops&&
                reservedStops>0&&reservedStops<referenceStops;
    if(auto f=std::fopen("liveness-check.txt","w")){
        std::fprintf(f,"Matching bitmap traversals=%u mark comparisons passed=%d reachable set matched=%d callback order matched=%d objects=%zu batches=%zu stoppedOnBegin=%d starts=%u stops=%u resumed=%d\n",
                     descriptors,matchedMarks,reachable,ordered,found.size(),actual.batches.size(),stoppedOnBegin,starts,stops,!worldStopped);
        std::fprintf(f,"World stops with original reserve=%u with learned reserve=%u\n",referenceStops,reservedStops);
        std::fclose(f);
    }
    ExitProcess(passed?0:1);
}
extern "C" __declspec(dllexport) uintptr_t il2cpp_init(const char *domain) {
    auto initialized=invoke("il2cpp_init",domain);
    if(initialized&&GetEnvironmentVariableW(L"REINCOMP_VERIFY_NATIVE_ROOTS",nullptr,0)){checkStoreCost();verifyWindowsRoots();}
    if(initialized&&GetEnvironmentVariableW(L"REINCOMP_CHECK_LIVENESS",nullptr,0))verifyLiveness();
    if(initialized&&GetEnvironmentVariableW(L"REINCOMP_INSPECT_ENUMS",nullptr,0))inspectEnumLayouts();
    if(initialized&&GetEnvironmentVariableW(L"REINCOMP_INSPECT_MOVIE",nullptr,0))inspectMovieMetadata();
    return initialized;
}
extern "C" __declspec(dllexport) uintptr_t il2cpp_init_utf16(const uint16_t *domain) {
    return invoke("il2cpp_init_utf16",domain);
}
