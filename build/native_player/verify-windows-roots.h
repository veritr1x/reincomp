// Called after genuine IL2CPP initialization only in the separately requested root test.
struct RootFixture {
    void *domain;
    uintptr_t masked=0;
    uint32_t strong=0,kept=0,discarded=0;
};
constexpr uintptr_t root_mask=0x539ac8b178d52f40ULL;
static DWORD WINAPI makeRootObjects(void *args){
    auto &x=*static_cast<RootFixture *>(args);
    auto thread=il2cpp_thread_attach(x.domain);
    if(!thread)return 1;
    auto a=il2cpp_string_new("native stack root lifetime");
    auto b=il2cpp_string_new("unreachable allocation control");
    if(!a||!b||a==b)return 2;
    x.strong=il2cpp_gchandle_new(a,false);
    x.kept=il2cpp_gchandle_new_weakref(a,false);
    x.discarded=il2cpp_gchandle_new_weakref(b,false);
    x.masked=reinterpret_cast<uintptr_t>(a)^root_mask;
    il2cpp_thread_detach(thread);
    return 0;
}
static DWORD WINAPI collectRootObjects(void *args){
    auto &x=*static_cast<RootFixture *>(args);
    auto thread=il2cpp_thread_attach(x.domain);
    if(!thread)return 1;
    for(unsigned n=0;n<4;++n)il2cpp_gc_collect(2);
    il2cpp_thread_detach(thread);
    return 0;
}
static void rootWorker(LPTHREAD_START_ROUTINE task,RootFixture &x){
    auto thread=CreateThread(nullptr,0,task,&x,0,nullptr);
    DWORD code=0;
    if(!thread || WaitForSingleObject(thread,30000)!=WAIT_OBJECT_0 || !GetExitCodeThread(thread,&code) || code)
        boundary("native stack-root test worker did not complete");
    CloseHandle(thread);
}
static void verifyWindowsRoots(){
    RootFixture x{il2cpp_domain_get()};
    rootWorker(makeRootObjects,x);
    volatile uintptr_t root=x.masked^root_mask;
    il2cpp_gchandle_free(x.strong);x.strong=0;
    rootWorker(collectRootObjects,x);
    auto retained=il2cpp_gchandle_get_target(x.kept);
    auto control=il2cpp_gchandle_get_target(x.discarded);
    bool kept=retained && reinterpret_cast<uintptr_t>(retained)==root && il2cpp_string_length(retained)==26;
    wchar_t buf[32768]{};auto n=GetModuleFileNameW(nullptr,buf,32768);
    std::wstring file(buf,n);
    file=file.substr(0,file.find_last_of(L"\\/")+1)+L"native-root-test.txt";
    FILE *report=_wfopen(file.c_str(),L"w");
    if(report){std::fprintf(report,"Root held on the waiting Windows main thread survived=%d; unrooted weak control collected=%d; explicit full collections=4\n",kept,!control);std::fclose(report);}
    il2cpp_gchandle_free(x.kept);il2cpp_gchandle_free(x.discarded);
    RootFixture nested{il2cpp_domain_get()};
    rootWorker(makeRootObjects,nested);
    auto collect=reinterpret_cast<bool(*)(uint64_t,uint32_t)>(GetProcAddress(GetModuleHandleW(L"GuestRuntime.dll"),"CollectWithParentRoot"));
    bool completed=collect&&collect(nested.masked,nested.strong);
    auto nestedKept=il2cpp_gchandle_get_target(nested.kept);
    bool held=nestedKept && (reinterpret_cast<uintptr_t>(nestedKept)^root_mask)==nested.masked;
    bool cleared=!il2cpp_gchandle_get_target(nested.discarded);
    if(auto extra=_wfopen(file.c_str(),L"a")){
        std::fprintf(extra,"Parent guest register root survived=%d; unrooted weak control collected=%d; nested full collections=%d\n",held,cleared,completed?4:0);
        std::fclose(extra);
    }
    il2cpp_gchandle_free(nested.kept);il2cpp_gchandle_free(nested.discarded);
    ExitProcess(kept&&!control&&completed&&held&&cleared?0:133);
}
