static DWORD WINAPI samplePlayerMain(void *){
    FILE *log=std::fopen("main-samples.log","w");
    if(!log)return 0;
    const bool detailed=std::getenv("REINCOMP_SAMPLE_FIELD_CPU")!=nullptr;
    const bool menu=std::getenv("REINCOMP_OPEN_SCREEN")!=nullptr;
    auto ready=reinterpret_cast<bool(*)()>(GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),"RequestedScreenVisible"));
    const bool opening=std::getenv("REINCOMP_PROFILE_MENU_TRANSITION")!=nullptr;
    auto transition=reinterpret_cast<bool(*)()>(GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),"MenuTransitionActive"));
    struct Module {const char *name;uintptr_t start,end;};
    Module modules[9]{};
    unsigned n=0;
    for(const char *name:{"UnityPlayer.dll","GuestRuntime.dll","GameAssembly.dll","EngineCalls.dll","ntdll.dll","KernelBase.dll","ucrtbase.dll","nvoglv64.dll","kernel32.dll"}){
        auto base=reinterpret_cast<uintptr_t>(GetModuleHandleA(name));
        if(!base)continue;
        auto pe=reinterpret_cast<const IMAGE_NT_HEADERS *>(base+reinterpret_cast<const IMAGE_DOS_HEADER *>(base)->e_lfanew);
        modules[n++]={name,base,base+pe->OptionalHeader.SizeOfImage};
    }
    auto where=[&](uintptr_t pc){
        for(auto &m:modules)if(pc>=m.start&&pc<m.end){std::fprintf(log," %s+%llx",m.name,(unsigned long long)(pc-m.start));return true;}
        return false;
    };
    unsigned samples=0,coarse=0;
    const auto deadline=GetTickCount64()+240000;
    while(coarse<200 && samples<4500 && GetTickCount64()<deadline){
        if(opening && (!transition || !transition())){
            if(samples)break;
            Sleep(10);continue;
        }
        if(menu && !opening && (!ready || !ready())){
            ++coarse;Sleep(1000);continue;
        }
        auto out=loadingOut.load();
        bool field=detailed && out && GetTickCount64()-out>=6000 && (actorConditions.load()&1);
        if(menu)field=detailed && (opening || (ready && ready()));
        if(field)++samples;else ++coarse;
        Sleep(field?5:1000);
        CONTEXT c{};c.ContextFlags=CONTEXT_CONTROL;
        uintptr_t stack[192]{};SIZE_T bytes=0;
        FILETIME creation,exit,kernel,user;
        if(!GetThreadTimes(ownerHandle,&creation,&exit,&kernel,&user))break;
        const char *phase=ownerCall.load();
        if(SuspendThread(ownerHandle)==DWORD(-1))break;
        bool ok=GetThreadContext(ownerHandle,&c);
        if(ok && (!field || (opening && samples%4==0)))ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void *>(c.Rsp),stack,sizeof stack,&bytes);
        ResumeThread(ownerHandle);
        ULARGE_INTEGER cpu{};cpu.LowPart=user.dwLowDateTime;cpu.HighPart=user.dwHighDateTime;
        std::fprintf(log,"%llu mainUserMs=%llu phase=%s",GetTickCount64(),cpu.QuadPart/10000,phase);
        if(ok){where(c.Rip);unsigned seen=0;for(unsigned j=0;j<bytes/sizeof(*stack)&&seen<11;++j)if(where(stack[j]))++seen;}
        std::fputc('\n',log);
        if(!field || samples%128==0)std::fflush(log);
    }
    std::fclose(log);return 0;
}
