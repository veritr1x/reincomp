// Global 3.7.1 story and menu hooks. The roulette and Guerrilla hooks follow
// Bunker's display_patch.c and guerrilla_unlock.c.
#include "unity-object.h"

namespace bunker_events {
extern "C" Memory *sub_83291fb4(State &,addr_t,Memory *);
extern "C" Memory *sub_82b01eb0(State &,addr_t,Memory *);
extern "C" Memory *sub_830ce360(State &,addr_t,Memory *);

static Memory *actorCollider(State &s,addr_t pc,Memory *memory){
    // OnEnter and OnStay compare the incoming collider's GameObject with this
    // result. Either callback can run after the current actor has been unloaded.
    const auto actor=s.gpr.x0.qword;
    const auto caller=s.gpr.x30.qword;
    if((caller==0x82983a50 || caller==0x8298449c) && actor && !nativeUnityObjectAlive(reinterpret_cast<const void *>(actor))){
        s.gpr.x0.qword=0;
        s.gpr.pc.qword=caller;
        return memory;
    }
    return sub_830ce360(s,pc,memory);
}

static Memory *showRoulette(State &s,addr_t pc,Memory *memory){
    using Show=bool(*)(void *,void *);
    static auto show=reinterpret_cast<Show>(GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),"BunkerShowRoulette"));
    if(!show || !show(reinterpret_cast<void *>(s.gpr.x0.qword),reinterpret_cast<void *>(s.gpr.x1.qword)))
        return sub_83291fb4(s,pc,memory);
    s.gpr.pc.qword=s.gpr.x30.qword;
    return memory;
}

static Memory *showTimetable(State &s,addr_t pc,Memory *memory){
    const auto view=s.gpr.x0.qword,caller=s.gpr.x30.qword;
    auto result=sub_82b01eb0(s,pc,memory);
    if(s.gpr.pc.qword!=caller)return result;
    addr_t button=0;
    if(view)recomp::ReadGuestMemory(&button,view+0x90,sizeof(button));
    if(button){
        addr_t entity=0;
        const bool open=threads->Run(0x829420e8,{reinterpret_cast<addr_t>(&entity),0})!=0;
        const auto object=threads->Run(0x84918de8,{button,0});
        if(object)threads->Run(0x84921f20,{object,open,0});
    }
    return result;
}

// A story can cross its collision zone during the balloon fade between two
// states. Keep the last 32 crossings, as on Android. The desktop host keeps
// weak IL2CPP handles so scene unload or collection can invalidate a record.
struct WeakObject {
    addr_t address=0;
    uint32_t id=0;
    void set(addr_t object){
        static const auto release=modules->Symbol(handle,"il2cpp_gchandle_free");
        static const auto make=modules->Symbol(handle,"il2cpp_gchandle_new_weakref");
        if(id)threads->Run(release,{id});
        address=object;
        id=object?static_cast<uint32_t>(threads->Run(make,{object,0})):0;
    }
    addr_t get() const{
        static const auto target=modules->Symbol(handle,"il2cpp_gchandle_get_target");
        return id?threads->Run(target,{id}):0;
    }
};
struct Crossing {WeakObject zone,collider; ULONGLONG time=0;};
struct Fallback {WeakObject conductor; ULONGLONG since=0;};
static Crossing crossings[32];
static Fallback fallbacks[8];
static size_t nextCrossing=0,activeFallbacks=0;
static addr_t stayClass=0,enterClass=0;

static void entered(State &s){
    if(GetCurrentThreadId()!=owner || !s.gpr.x0.qword || !s.gpr.x1.qword)return;
    auto &cross=crossings[nextCrossing++%std::size(crossings)];
    cross.zone.set(s.gpr.x0.qword);
    cross.collider.set(s.gpr.x1.qword);
    cross.time=GetTickCount64();
}
static void changingState(State &s){
    using field_metadata::read;
    const auto state=s.gpr.x1.qword;
    if(!state || GetCurrentThreadId()!=owner)return;
    const auto list=read<addr_t>(state+0x48);
    if(!list)return;
    const auto items=read<addr_t>(list+0x10);
    if(!items)return;
    const auto count=std::min(read<int32_t>(list+0x18),8);
    addr_t stay=0,stayZone=0,enterZone=0;
    for(int i=0;i<count;++i){
        const auto conductor=read<addr_t>(items+0x20+i*sizeof(addr_t));
        if(!conductor)continue;
        const auto klass=read<addr_t>(conductor);
        if(!klass)continue;
        if(!stayClass || !enterClass){
            const auto name=read<const char *>(klass+0x10);
            if(!stayClass && name && !std::strcmp(name,"EventTriggerOnStayCollision"))stayClass=klass;
            if(!enterClass && name && !std::strcmp(name,"EventTriggerOnEnterCollision"))enterClass=klass;
        }
        if(klass==stayClass){stay=conductor;stayZone=read<addr_t>(stay+0x50);}
        if(klass==enterClass)enterZone=read<addr_t>(conductor+0x58);
    }
    if(!stayZone || stayZone!=enterZone)return;
    auto *slot=std::find_if(std::begin(fallbacks),std::end(fallbacks),[&](const Fallback &f){
        return !f.conductor.address || f.conductor.address==stay;
    });
    if(slot==std::end(fallbacks))slot=fallbacks;
    if(!slot->conductor.address)++activeFallbacks;
    slot->conductor.set(stay);
    slot->since=GetTickCount64();
}
static void updateStay(State &s){
    using field_metadata::read;
    if(GetCurrentThreadId()!=owner || !activeFallbacks)return;
    const auto self=s.gpr.x0.qword;
    if(!self)return;
    for(auto &f:fallbacks){
        if(f.conductor.address!=self)continue;
        const bool live=f.conductor.get()==self;
        f.conductor.set(0);
        --activeFallbacks;
        if(!live)continue;
        const auto zone=read<addr_t>(self+0x50);
        for(auto &cross:crossings){
            if(!zone || read<uint8_t>(self+0x59))break;
            if(cross.zone.address!=zone || cross.time+10000<f.since || cross.zone.get()!=zone)continue;
            const auto collider=cross.collider.get();
            if(collider && read<addr_t>(collider+0x10))
                threads->Run(0x82984420,{self,collider,0}); // EventTriggerOnStayCollision.OnStay
        }
    }
}
static void install(){
    recomp::AddGuestDirectEntry(0x830ce360,actorCollider);
    recomp::AddGuestDirectEntry(0x83291fb4,showRoulette);
    recomp::AddGuestDirectEntry(0x82b01eb0,showTimetable);
    recomp::AddGuestProbe(0x8363082c,entered);
    recomp::AddGuestProbe(0x83a0ecf8,changingState);
    recomp::AddGuestProbe(0x83a05c64,updateStay);
}
}
