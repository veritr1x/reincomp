#include <tuple>

namespace liveness_walk {
extern "C" Memory *sub_8257dde0_liveness(State &,addr_t,Memory *);
extern "C" Memory *sub_8257de30_liveness(State &,addr_t,Memory *);
extern "C" Memory *sub_8257d710_liveness(State &,addr_t,Memory *);
extern "C" Memory *sub_8257d710(State &,addr_t,Memory *);
extern "C" Memory *sub_8257d9e8(State &,addr_t,Memory *);
extern "C" Memory *sub_8257d9e8_liveness(State &,addr_t,Memory *);

template<class T> static T read(addr_t address){
    T value;
    std::memcpy(&value,reinterpret_cast<const void *>(address),sizeof(value));
    return value;
}
struct ObjectList {addr_t *items;size_t size,capacity;};
static bool hasSpace(const ObjectList *list){
    return list->size<(list->capacity & 0x7fffffffffffffffULL);
}
static void append(ObjectList *list,addr_t object){
    auto size=list->size+1;
    recomp::WriteGuestMemory(reinterpret_cast<addr_t>(&list->size),&size,sizeof(size));
    recomp::WriteGuestMemory(reinterpret_cast<addr_t>(&list->items[size-1]),&object,sizeof(object));
}
// Called after the lifted visitor's entry poll. The original owns all growth,
// flushing and callbacks. Check both lists before modifying either of them.
extern "C" bool MarkLivenessInPlace(State &s){
    const auto object=s.gpr.x0.qword;
    const auto state=s.gpr.x1.qword;
    bool enqueue=false;
    const auto klass=object?read<addr_t>(object):0;
    if(object && !(klass&1)){
        enqueue=(read<uint16_t>(klass+0x12e)&0x20)!=0;
        auto filter=read<addr_t>(state+0x10);
        bool accepted=enqueue || !filter;
        if(!accepted){
            unsigned depth=read<uint8_t>(filter+0x128);
            if(!depth)return false;
            accepted=read<uint8_t>(klass+0x128)>=depth &&
                read<addr_t>(read<addr_t>(klass+0xc8)+(depth-1)*sizeof(addr_t))==filter;
        }
        if(accepted){
            auto *all=read<ObjectList *>(state+8);
            auto *pending=enqueue?read<ObjectList *>(state+0x18):nullptr;
            if(!hasSpace(all) || (enqueue && (pending==all || !hasSpace(pending))))return false;
            append(all,object);
            const auto marked=klass|1;
            recomp::WriteGuestMemory(object,&marked,sizeof(marked));
            if(enqueue)append(pending,object);
        }
    }
    s.gpr.x0.qword=enqueue;
    s.gpr.pc.qword=s.gpr.x30.qword;
    return true;
}

// The two public entry points are single unconditional branches in the pinned
// image. Start at their destinations with the PC the original branch supplies.
template<addr_t target, recomp::Entry body>
static Memory *enter(State &s,addr_t,Memory *memory){
    s.gpr.pc.qword=target;
    return body(s,target,memory);
}

struct DescriptorCheck {
    std::vector<addr_t> calls;
    bool original;
};
static thread_local DescriptorCheck *checking;
static thread_local bool referenceMark;
static unsigned registeredWalkers;

static recomp::Entry publicWalker(addr_t pc){
    if(pc==0x8254da4c)return enter<0x8257dde0,sub_8257dde0_liveness>;
    if(pc==0x8254da50)return enter<0x8257de30,sub_8257de30_liveness>;
    return nullptr;
}
static void installTestVisitors(){
    if(!std::getenv("REINCOMP_CHECK_LIVENESS"))return;
    recomp::AddGuestDirectEntry(0x8257d710,[](State &s,addr_t pc,Memory *mem){
        bool original=checking?checking->original:referenceMark;
        return (original?sub_8257d710:sub_8257d710_liveness)(s,pc,mem);
    });
    recomp::AddGuestDirectEntry(0x8257d9e8,[](State &s,addr_t pc,Memory *mem){
        if(!checking)return (referenceMark?sub_8257d9e8:sub_8257d9e8_liveness)(s,pc,mem);
        if(s.gpr.x1.qword!=reinterpret_cast<addr_t>(checking))std::abort();
        checking->calls.push_back(s.gpr.x0.qword);
        s.gpr.x0.qword=1;
        s.gpr.pc.qword=s.gpr.x30.qword;
        return mem;
    });
}

extern "C" __declspec(dllexport) void ReferenceLivenessRoot(addr_t root,addr_t state){
    referenceMark=true;
    threads->Run(0x8257dde0,{root,state});
    referenceMark=false;
}

extern "C" __declspec(dllexport) unsigned CheckLivenessDescriptors(){
    if(registeredWalkers!=2)return 0;
    // Run both actual lifted functions on identical objects. Substitute only
    // the visiting callback so even invalid/tagged slot values can be compared.
    std::array<addr_t,2> klass{};
    std::array<addr_t,62> slots{};
    slots[0]=reinterpret_cast<addr_t>(klass.data())|1;
    for(size_t i=1;i<slots.size();++i)slots[i]=i%8?i:0;
    DescriptorCheck check{};
    checking=&check;
    uint64_t seed=0xc986343e57bb5521ULL;
    for(unsigned i=0;i<512;++i){
        seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;
        klass[1]=i<64?uint64_t(1)<<i:i<128?~(uint64_t(1)<<(i-64)):i<132?i-128:seed;
        auto run=[&](bool original){
            check.original=original;
            check.calls.clear();
            return threads->Run(0x8257d710,{reinterpret_cast<addr_t>(slots.data()),reinterpret_cast<addr_t>(&check)});
        };
        auto result=run(true);
        auto expected=check.calls;
        if(run(false)!=result || check.calls!=expected || expected.size()!=size_t(__builtin_popcountll(klass[1]>>2))){
            checking=nullptr;
            return 0;
        }
    }
    checking=nullptr;
    return 512;
}

extern "C" __declspec(dllexport) bool CheckLivenessMarks(){
    std::array<uint64_t,40> klass{},filter{};
    std::array<addr_t,3> hierarchy{};
    auto address=[](auto *value){return reinterpret_cast<addr_t>(value);};
    auto classBytes=reinterpret_cast<uint8_t *>(klass.data());
    auto filterBytes=reinterpret_cast<uint8_t *>(filter.data());
    klass[0xc8/8]=address(hierarchy.data());
    filterBytes[0x128]=2;
    addr_t object;
    std::array<addr_t,4> allItems,pendingItems;
    ObjectList all{allItems.data(),0,4},pending{pendingItems.data(),0,4};
    std::array<addr_t,4> state{0,address(&all),0,address(&pending)};
    for(unsigned flags=0;flags<64;++flags){
        auto run=[&](bool original){
            allItems.fill(0x35);pendingItems.fill(0x67);
            referenceMark=original;
            all.size=1;pending.size=1;
            all.capacity=4 | (uint64_t(flags&1)<<63);
            state[2]=(flags&2)?address(filter.data()):0;
            classBytes[0x12e]=(flags&4)?0x20:0;
            classBytes[0x128]=(flags&8)?1:3;
            hierarchy[1]=(flags&16)?state[2]:0;
            object=address(klass.data())|((flags&32)?1:0);
            auto visit=[&](addr_t obj){return threads->Run(0x8257d9e8,{obj,address(state.data())});};
            auto nullResult=visit(0);
            auto initial=visit(address(&object));
            auto repeated=visit(address(&object));
            return std::make_tuple(nullResult,initial,repeated,object,all.size,pending.size,allItems,pendingItems);
        };
        auto original=run(true);
        if(run(false)!=original)return false;
    }
    // A full second list must leave the first list and mark bit untouched so
    // the original visitor can perform its recursive flush.
    State registers{};
    object=address(klass.data());
    classBytes[0x12e]=0x20;
    all.size=1;pending.size=pending.capacity;
    registers.gpr.x0.qword=address(&object);
    registers.gpr.x1.qword=address(state.data());
    return !MarkLivenessInPlace(registers) && all.size==1 && object==address(klass.data());
}
}
