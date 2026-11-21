#include <algorithm>
// Restrict the two All views used by CalculatorWeapon.GetAfterEvolutionWeapons.
// Keep its original FirstOrDefault, Where, Select and newly allocated List<int>.
// Both predicates read plain integer fields from the current master table.
namespace weapon_evolution {
extern "C" Memory *sub_847c006c(State &,addr_t,Memory *);
extern "C" Memory *sub_827bf094(State &,addr_t,Memory *);
constexpr addr_t firstRow=0x827bf158,groupRows=0x827bf21c;
static thread_local bool compareOriginal=false;
static const bool checking=std::getenv("REINCOMP_VERIFY_WEAPON_QUERY")!=nullptr;
static thread_local uint64_t originalRows=0,candidateRows=0;
static thread_local bool sweepStarted=false;
static thread_local std::vector<int32_t> verifyIds;
struct Range { addr_t array; int32_t first,last; uint8_t ascending,hasValue; };
template<class T> static T read(addr_t at){T v;std::memcpy(&v,reinterpret_cast<void *>(at),sizeof(v));return v;}

static Memory *all(State &s,addr_t pc,Memory *memory){
    const auto caller=s.gpr.x30.qword;
    if((caller!=firstRow && caller!=groupRows) || compareOriginal)return sub_847c006c(s,pc,memory);
    const auto destination=s.gpr.x8.qword,closure=s.gpr.x19.qword;
    auto result=sub_847c006c(s,pc,memory);
    if(s.gpr.pc.qword!=caller || !destination || !closure)return result;
    auto view=read<Range>(destination);
    if(!view.array || !view.hasValue || !view.ascending || view.first!=0 || view.last<0)return result;
    const bool first=caller==firstRow;
    const auto keyObject=first?closure:read<addr_t>(closure+0x18);
    if(!keyObject)return result;
    const auto key=read<int32_t>(keyObject+0x10);
    int32_t left=-1,right=-1;
    const size_t count=size_t(view.last)+1;
    if(checking && first && !sweepStarted && verifyIds.empty() && GetCurrentThreadId()==owner){
        for(size_t i=0;i<count;++i){
            auto row=read<addr_t>(view.array+0x20+sizeof(addr_t)*i);
            if(row)verifyIds.push_back(read<int32_t>(row+0x18));
        }
        verifyIds.push_back(-1);
        std::sort(verifyIds.begin(),verifyIds.end());
        verifyIds.erase(std::unique(verifyIds.begin(),verifyIds.end()),verifyIds.end());
    }
    for(size_t i=0;i<count;++i){
        const auto row=read<addr_t>(view.array+0x20+sizeof(addr_t)*size_t(i));
        if(!row)return result; // Keep the original predicate's null exception.
        if(read<int32_t>(row+(first?0x18:0x10))==key){
            if(left<0)left=int32_t(i);
            right=int32_t(i);
            if(first)break;
        }
    }
    view.hasValue=left>=0;
    if(checking){
        originalRows+=uint64_t(view.last)+1;
        if(view.hasValue)candidateRows+=uint64_t(right-left)+1;
    }
    view.first=view.hasValue?left:0;
    view.last=right;
    // Update only the bounds and hasValue; the original getter owns the rest.
    std::memcpy(reinterpret_cast<void *>(destination+8),&view.first,sizeof(view.first));
    std::memcpy(reinterpret_cast<void *>(destination+12),&view.last,sizeof(view.last));
    std::memcpy(reinterpret_cast<void *>(destination+0x11),&view.hasValue,1);
    return result;
}

static std::vector<int32_t> values(addr_t list){
    if(!list)return {};
    const auto array=read<addr_t>(list+0x10);
    const auto count=read<int32_t>(list+0x18);
    if(count<0 || !array || size_t(count)>read<size_t>(array+0x18))std::abort();
    auto *begin=reinterpret_cast<const int32_t *>(array+0x20);
    return {begin,begin+count};
}
static Memory *verify(State &s,addr_t pc,Memory *memory){
    State initial=s;
    compareOriginal=true;
    auto result=sub_827bf094(s,pc,memory);
    compareOriginal=false;
    if(s.gpr.pc.qword!=initial.gpr.x30.qword)return result;
    const bool existed=s.gpr.x0.qword!=0;
    const auto expected=values(s.gpr.x0.qword);
    originalRows=candidateRows=0;
    s=initial;
    result=sub_827bf094(s,pc,memory);
    const bool equal=s.gpr.pc.qword==initial.gpr.x30.qword && existed==bool(s.gpr.x0.qword) && expected==values(s.gpr.x0.qword);
    static FILE *log=std::fopen("weapon-query-check.tsv","w");
    if(log){
        std::fprintf(log,"%u\t%d\t%zu\t%d\t%llu\t%llu\n",initial.gpr.x0.dword,existed,expected.size(),equal,
            (unsigned long long)originalRows,(unsigned long long)candidateRows);
        std::fflush(log);
    }
    if(!equal)std::abort();
    if(!sweepStarted && !verifyIds.empty()){
        sweepStarted=true;
        for(auto id:verifyIds)threads->Run(0x827bf094,{uint32_t(id),0});
        verifyIds.clear();
    }
    return result;
}
static void install(){
    recomp::AddGuestDirectEntry(0x847c006c,all);
    if(checking)recomp::AddGuestDirectEntry(0x827bf094,verify);
}
}
