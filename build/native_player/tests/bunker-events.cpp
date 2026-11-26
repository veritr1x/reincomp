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
template<class F> void AddGuestDirectEntry(addr_t,F){}
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
int main(){
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
    std::puts("Story zone re-entry, expiry, actor teardown, collider lifetime, and Guerrilla availability passed.");
}
