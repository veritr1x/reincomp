// Compile with g++ -std=c++17 bunker-events.cpp -o bunker-events.exe
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string_view>

using addr_t=uintptr_t;
using ULONGLONG=unsigned long long;
struct Register {addr_t qword=0;};
struct State {struct {Register x0,x1,x30,pc;} gpr;};
struct Memory {};
static unsigned owner=1;
static unsigned GetCurrentThreadId(){return owner;}
static ULONGLONG now=20000;
static ULONGLONG GetTickCount64(){return now;}
static void *GetModuleHandleW(const wchar_t *){return nullptr;}
static void *GetProcAddress(void *,const char *){return nullptr;}
namespace field_metadata {
template<class T> T read(addr_t address){T result;std::memcpy(&result,reinterpret_cast<void *>(address),sizeof(T));return result;}
}
namespace recomp {
static void ReadGuestMemory(void *dest,addr_t from,size_t size){std::memcpy(dest,reinterpret_cast<void *>(from),size);}
static void WriteGuestMemory(addr_t to,const void *src,size_t size){std::memcpy(reinterpret_cast<void *>(to),src,size);}
using Entry=Memory *(*)(State &,addr_t,Memory *);
static std::map<addr_t,Entry> directEntries;
static void AddGuestDirectEntry(addr_t pc,Entry handler){directEntries[pc]=handler;}
template<class F> void AddGuestProbe(addr_t,F){}
}
static addr_t handle;
struct Module {
    addr_t Symbol(addr_t,std::string_view name){
        if(name=="il2cpp_gchandle_new_weakref")return 1;
        if(name=="il2cpp_gchandle_free")return 2;
        assert(name=="il2cpp_gchandle_get_target");return 3;
    }
} module,*modules=&module;
struct Runtime {
    std::map<addr_t,addr_t> refs;
    addr_t next=0;
    unsigned triggered=0;
    bool available=false,visible=false;
    addr_t Run(addr_t pc,std::initializer_list<addr_t> args){
        const auto a=args.begin();
        switch(pc){
        case 1: refs[++next]=a[0];return next;
        case 2: refs.erase(a[0]);return 0;
        case 3: return refs.at(a[0]);
        case 0x82984420: ++triggered;*reinterpret_cast<uint8_t *>(a[0]+0x59)=1;return 0;
        case 0x829420e8: return available;
        case 0x84918de8: return a[0];
        case 0x84921f20: visible=a[1];return 0;
        default: assert(false);return 0;
        }
    }
} vm,*threads=&vm;
#include "../bunker-events.h"
static bool resumedForceIdle=false,resumedRandomAnimation=false,completedQueuedTask=false;
extern "C" Memory *sub_8281dbe0(State &,addr_t pc,Memory *m){
    assert(pc==0x8281dbe0);resumedForceIdle=true;return m;
}
extern "C" Memory *sub_8281efb4(State &,addr_t pc,Memory *m){
    assert(pc==0x8281efb4);resumedRandomAnimation=true;return m;
}
extern "C" Memory *sub_840fce24(State &s,addr_t pc,Memory *m){
    assert(pc==0x840fce24 && s.gpr.x1.qword==0);
    completedQueuedTask=true;s.gpr.pc.qword=s.gpr.x30.qword;return m;
}
extern "C" Memory *sub_83291fb4(State &s,addr_t,Memory *m){s.gpr.pc.qword=s.gpr.x30.qword;return m;}
extern "C" Memory *sub_82b01eb0(State &s,addr_t,Memory *m){s.gpr.pc.qword=s.gpr.x30.qword;return m;}
static unsigned colliderLookups=0;
extern "C" Memory *sub_830ce360(State &s,addr_t,Memory *m){
    ++colliderLookups;
    s.gpr.x0.qword=42;
    s.gpr.pc.qword=s.gpr.x30.qword;
    return m;
}
struct Object {
    std::array<addr_t,32> words{};
    addr_t address(){return reinterpret_cast<addr_t>(words.data());}
    void put(size_t offset,addr_t value){std::memcpy(reinterpret_cast<void *>(address()+offset),&value,sizeof(value));}
};
static void checkPicturebookForceIdle(){
    Object runner,player,timeline,director;
    const auto task=runner.address()+0x10;
    runner.put(0x10+0x20,player.address());
    player.put(0x28,timeline.address());
    timeline.put(0x48,director.address());
    for(int32_t phase:{-1,0,1})for(bool active:{false,true})for(bool alive:{false,true}){
        runner.put(0x10,static_cast<uint32_t>(phase));
        runner.put(0x10+0x38,5);
        timeline.put(0x60,active);
        director.put(0x10,alive);
        State s{};
        s.gpr.x0.qword=runner.address();s.gpr.x1.qword=17;s.gpr.x30.qword=42;
        Memory memory{};
        resumedForceIdle=completedQueuedTask=false;
        assert(recomp::directEntries.at(0x8281dbe0)(s,0x8281dbe0,&memory)==&memory);
        const bool unloaded=phase>=0 && active && !alive;
        assert(completedQueuedTask==unloaded && resumedForceIdle==!unloaded);
        assert(field_metadata::read<int32_t>(task)==(unloaded?-2:phase));
        assert(field_metadata::read<int32_t>(task+0x38)==(unloaded?0:5));
        if(unloaded)assert(s.gpr.x0.qword==task+8 && s.gpr.pc.qword==42);
        else assert(s.gpr.x0.qword==runner.address() && s.gpr.x1.qword==17);
    }
}
static void checkRandomAnimationWait(){
    Object runner,player,animator;
    const auto task=runner.address()+0x10;
    runner.put(0x10+0x18,player.address());
    for(int32_t phase:{-1,0,-2})for(auto target:{addr_t(0),animator.address()})for(bool alive:{false,true}){
        runner.put(0x10,static_cast<uint32_t>(phase));
        runner.put(0x10+0x28,5);
        player.put(0x40,target);
        animator.put(0x10,alive);
        State s{};
        s.gpr.x0.qword=runner.address();s.gpr.x1.qword=17;s.gpr.x30.qword=42;
        Memory memory{};
        resumedRandomAnimation=completedQueuedTask=false;
        assert(recomp::directEntries.at(0x8281efb4)(s,0x8281efb4,&memory)==&memory);
        const bool unloaded=phase==0 && target && !alive;
        assert(completedQueuedTask==unloaded && resumedRandomAnimation==!unloaded);
        assert(field_metadata::read<int32_t>(task)==(unloaded?-2:phase));
        assert(field_metadata::read<int32_t>(task+0x28)==(unloaded?0:5));
        if(unloaded)assert(s.gpr.x0.qword==task+8 && s.gpr.pc.qword==42);
        else assert(s.gpr.x0.qword==runner.address() && s.gpr.x1.qword==17);
    }
}
int main(){
    bunker_events::install();
    checkPicturebookForceIdle();
    checkRandomAnimationWait();
    using namespace bunker_events;
    Object stay,enter,stayType,enterType,state,list,items,zone,collider;
    stayType.put(0x10,reinterpret_cast<addr_t>("EventTriggerOnStayCollision"));
    enterType.put(0x10,reinterpret_cast<addr_t>("EventTriggerOnEnterCollision"));
    stay.put(0,stayType.address());enter.put(0,enterType.address());
    stay.put(0x50,zone.address());enter.put(0x58,zone.address());collider.put(0x10,1);
    state.put(0x48,list.address());list.put(0x10,items.address());list.put(0x18,2);
    items.put(0x20,stay.address());items.put(0x28,enter.address());
    State s{};
    Memory memory{};
    Object liveActor,destroyedActor;
    liveActor.put(0x10,1);
    for(auto actor:{liveActor.address(),destroyedActor.address(),addr_t(0)}){
        for(auto caller:{0x82983a50u,0x8298449cu,0x82983a54u}){
            s.gpr.x0.qword=actor;
            s.gpr.x30.qword=caller;
            s.gpr.pc.qword=0x830ce360;
            colliderLookups=0;
            assert(actorCollider(s,0x830ce360,&memory)==&memory);
            const bool collisionCallback=caller==0x82983a50u || caller==0x8298449cu;
            const bool expired=actor==destroyedActor.address() && collisionCallback;
            assert(colliderLookups==(expired?0u:1u));
            assert(s.gpr.x0.qword==(expired?0u:42u) && s.gpr.pc.qword==caller);
        }
    }
    auto cross=[&]{s.gpr.x0.qword=zone.address();s.gpr.x1.qword=collider.address();entered(s);};
    auto arm=[&]{s.gpr.x1.qword=state.address();changingState(s);};
    auto update=[&]{s.gpr.x0.qword=stay.address();updateStay(s);};
    cross();now+=1500;arm();update();update();
    assert(vm.triggered==1 && activeFallbacks==0);
    stay.put(0x58,0);now+=11000;arm();update();assert(vm.triggered==1);
    cross();collider.put(0x10,0);arm();update();assert(vm.triggered==1);
    collider.put(0x10,1);cross();vm.refs[crossings[nextCrossing-1].collider.id]=0;
    // The older crossing must have the same destroyed-collider outcome.
    vm.refs[crossings[nextCrossing-2].collider.id]=0;
    arm();update();assert(vm.triggered==1);
    enter.put(0x58,0);arm();assert(activeFallbacks==0);
    Object view,button;view.put(0x90,button.address());s.gpr.x0.qword=view.address();
    vm.visible=true;showTimetable(s,0,nullptr);assert(!vm.visible);
    vm.available=true;showTimetable(s,0,nullptr);assert(vm.visible);
    std::puts("Picturebook and animation task completion, story zone re-entry, actor teardown, collider lifetime, and Guerrilla availability passed.");
}
