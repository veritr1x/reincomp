// Closed IL2CPP types first used when a field story update reaches the client.
// Asking the original VM about assignability prepares the type hierarchy,
// method arrays and generic context without invoking a managed constructor.
namespace field_metadata {
constexpr addr_t registrationSlot=0x879a5ec8;
constexpr unsigned questUpdateTypes[]={
    // WhereSelectArrayIterator, Buffer, List, Dictionary and IList for each row.
    61248,61253,58583,58590,56131, // EntityIUserMainQuestFlowStatus
    61254,61259,58617,58624,56141, // EntityIUserMainQuestMainFlowStatus
    61266,61271,58651,58658,56151  // EntityIUserMainQuestProgressStatus
};
template<class T> static T read(addr_t address){
    T value;
    std::memcpy(&value,reinterpret_cast<const void *>(address),sizeof(value));
    return value;
}
static size_t nextType=0;
static bool loading=false;
static void begin(){
    const auto profile=std::getenv("REINCOMP_BENCH_PROFILE");
    loading=profile && !std::strcmp(profile,"native-checkpoint-field") && nextType<std::size(questUpdateTypes);
}
static void prepareNext(){
    if(!loading || GetCurrentThreadId()!=owner)return;
    const auto registration=read<addr_t>(registrationSlot);
    if(!registration)return;
    const auto index=questUpdateTypes[nextType++];
    if(nextType==std::size(questUpdateTypes))loading=false;
    const auto count=read<int32_t>(registration+0x30);
    const auto types=read<addr_t>(registration+0x38);
    if(!types || count<=0 || index>=unsigned(count))return;
    const auto type=read<addr_t>(types+sizeof(addr_t)*index);
    if(!type || (read<uint32_t>(type+8)&0x40ff0000)!=0x00150000)return;
    const auto klass=threads->Run(0x8254d1f4,{type}); // il2cpp_class_from_il2cpp_type
    if(!klass)return;
    static const auto objectType=[]{
        const auto corlib=threads->Run(modules->Symbol(handle,"il2cpp_get_corlib"));
        return threads->Run(modules->Symbol(handle,"il2cpp_class_from_name"),
            {corlib,reinterpret_cast<addr_t>("System"),reinterpret_cast<addr_t>("Object")});
    }();
    if(!objectType)return;
    const auto assignable=threads->Run(0x8254d1e4,{objectType,klass});
    if(std::getenv("REINCOMP_PROFILE_STORY_WORK")){
        static FILE *log=std::fopen("field-metadata.tsv","w");
        if(log){std::fprintf(log,"%u\t%d\n",index,bool(assignable));std::fflush(log);}
    }
}
}
