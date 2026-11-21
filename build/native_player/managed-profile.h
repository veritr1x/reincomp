// Measure Unity's main-thread managed callbacks during a loaded checkpoint.
namespace managed_profile {
struct Sample { uint64_t calls=0; int64_t ticks=0; };
static std::map<P,Sample> samples;
static std::atomic<DWORD> mainThread{};
static bool finished,fieldLoaded;
static int64_t frequency,readyAt;
static bool active;
static int64_t children;
static FILE *slowLog;
static P updateDelegate,queueMethod;
static size_t methodSlot,targetSlot,itemsSlot,countSlot,itemsHeader,arrayLengthSlot;
static size_t fieldOffset(P type,const char *name){
    auto member=il2cpp_class_get_field_from_name(type,name);
    return member?il2cpp_field_get_offset(member):0;
}
template<class T> static T read(P object,size_t offset){
    T result;
    std::memcpy(&result,static_cast<const char *>(object)+offset,sizeof(result));
    return result;
}
static int64_t now(){
    LARGE_INTEGER ticks;
    QueryPerformanceCounter(&ticks);
    return ticks.QuadPart;
}
static void frame(){
    if(!mainThread){
        mainThread=GetCurrentThreadId();
        LARGE_INTEGER scale;QueryPerformanceFrequency(&scale);frequency=scale.QuadPart;
    }
    static auto actor=reinterpret_cast<unsigned(*)()>(GetProcAddress(GetModuleHandleW(L"GuestRuntime.dll"),"NativeActorConditions"));
    const unsigned state=actor?actor():0;
    static const bool menu=std::getenv("REINCOMP_OPEN_SCREEN")!=nullptr;
    static const bool traceOpening=std::getenv("REINCOMP_PROFILE_MENU_TRANSITION")!=nullptr;
    fieldLoaded=menu?(traceOpening && MenuTransitionActive()):(state&13)==13;
    const int64_t at=now();
    static int64_t flushedAt=0;
    if(slowLog && at-flushedAt>=frequency){std::fflush(slowLog);flushedAt=at;}
    if(finished)return;
    if(menu?!requestedScreenVisible:(!(state&3) || (state&12)!=12)){
        readyAt=0;active=false;samples.clear();return;
    }
    if(!readyAt)readyAt=at;
    if(at-readyAt<5*frequency)return;
    if(at-readyAt<15*frequency){active=true;return;}
    active=false;finished=true;
    auto *output=std::fopen("managed-profile.tsv","w");
    if(!output)return;
    std::fprintf(output,"calls\texclusive_ms\tentry\tinvoker\tclass\tmethod\n");
    for(auto &[method,sample]:samples){
        auto klass=il2cpp_method_get_class(method);
        uintptr_t pointers[2];std::memcpy(pointers,method,sizeof(pointers));
        std::fprintf(output,"%llu\t%.3f\t%llx\t%llx\t%s.%s\t%s\n",
            (unsigned long long)sample.calls,1000.*sample.ticks/frequency,
            (unsigned long long)pointers[0],(unsigned long long)pointers[1],
            il2cpp_class_get_namespace(klass),il2cpp_class_get_name(klass),il2cpp_method_get_name(method));
    }
    std::fclose(output);
}
template<class Call> static P measure(P method,P object,Call &&call){
    if(GetCurrentThreadId()!=mainThread.load(std::memory_order_relaxed) || (!active && !fieldLoaded) || !method)return call();
    const bool aggregate=active;
    struct Action {P method,type;};
    std::array<Action,12> actions;
    size_t captured=0;
    int queued=0;
    if(method==updateDelegate && object && queueMethod && read<P>(object,methodSlot)==queueMethod){
        auto queue=read<P>(object,targetSlot);
        if(queue && itemsSlot && countSlot){
            auto array=read<P>(queue,itemsSlot);
            queued=read<int>(queue,countSlot);
            if(array && queued>0){
                auto length=read<uintptr_t>(array,arrayLengthSlot);
                auto limit=(std::min)({actions.size(),size_t(queued),size_t(length)});
                for(size_t i=0;i<limit;++i){
                    auto item=read<P>(array,itemsHeader+i*sizeof(P));
                    if(!item)continue;
                    auto instance=read<P>(item,targetSlot);
                    actions[captured++]={read<P>(item,methodSlot),instance?read<P>(instance,0):nullptr};
                }
            }
        }
    }
    const auto saved=children;
    children=0;
    const auto begin=now();
    auto result=call();
    const auto elapsed=now()-begin;
    const auto exclusive=elapsed-children;
    if(aggregate){
        auto &sample=samples[method];
        ++sample.calls;sample.ticks+=exclusive;
    }
    if(fieldLoaded && elapsed*1000>=3*frequency){
        if(!slowLog){
            slowLog=std::fopen("slow-callbacks.tsv","w");
            if(slowLog)std::fprintf(slowLog,"start_qpc_ms\telapsed_ms\texclusive_ms\tclass\tmethod\tqueued\tactions\n");
        }
        if(slowLog){
            auto klass=il2cpp_method_get_class(method);
            if(object && !std::strcmp(il2cpp_class_get_name(klass),"UpdateFunction") && !std::strcmp(il2cpp_method_get_name(method),"Invoke")){
                if(!updateDelegate){
                    updateDelegate=method;
                    methodSlot=fieldOffset(klass,"method");
                    targetSlot=fieldOffset(klass,"m_target");
                }
                if(methodSlot && targetSlot){
                    auto target=read<P>(object,methodSlot);
                    if(target){
                        method=target;klass=il2cpp_method_get_class(target);
                        if(!queueMethod && !std::strcmp(il2cpp_class_get_name(klass),"ContinuationQueue") &&
                           !std::strcmp(il2cpp_class_get_namespace(klass),"UniRx.Async.Internal")){
                            itemsSlot=fieldOffset(klass,"actionList");
                            countSlot=fieldOffset(klass,"actionListCount");
                            itemsHeader=il2cpp_array_object_header_size();
                            arrayLengthSlot=il2cpp_offset_of_array_length_in_array_object_header();
                            queueMethod=target;
                        }
                    }
                }
            }
            std::fprintf(slowLog,"%.4f\t%.3f\t%.3f\t%s.%s\t%s\t%d\t",
                1000.*begin/frequency,1000.*elapsed/frequency,1000.*exclusive/frequency,
                il2cpp_class_get_namespace(klass),il2cpp_class_get_name(klass),il2cpp_method_get_name(method),queued);
            for(size_t i=0;i<captured;++i){
                const auto &action=actions[i];
                if(!action.method)continue;
                auto type=action.type?action.type:il2cpp_method_get_class(action.method);
                auto name=il2cpp_type_get_name(il2cpp_class_get_type(type));
                std::fprintf(slowLog,"%s::%s ",name,il2cpp_method_get_name(action.method));
                il2cpp_free(name);
            }
            std::fputc('\n',slowLog);
            if(elapsed>=frequency/100)std::fflush(slowLog);
        }
    }
    children=saved+elapsed;
    return result;
}
}
