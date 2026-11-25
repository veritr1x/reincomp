// Open the game's own menu and footer. This is used only by -OpenScreen.
static std::atomic<bool> requestedScreenVisible{false};
extern "C" __declspec(dllexport) bool RequestedScreenVisible(){return requestedScreenVisible.load();}
static std::atomic<ULONGLONG> menuTransitionUntil{};
extern "C" __declspec(dllexport) bool MenuTransitionActive(){
    return GetTickCount64()<menuTransitionUntil.load(std::memory_order_relaxed);
}
static P findScreenComponent(const char *ns,const char *name,P &type){
    size_t count=0;
    auto assemblies=static_cast<P *>(il2cpp_domain_get_assemblies(il2cpp_domain_get(),&count));
    static auto find=reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.Object::FindObjectsOfType"));
    for(size_t i=0;i<count;++i){
        type=il2cpp_class_from_name(il2cpp_assembly_get_image(assemblies[i]),ns,name);
        if(!type)continue;
        P items=find(il2cpp_type_get_object(il2cpp_class_get_type(type)));
        if(!items || !invoke("il2cpp_array_length",items))return nullptr;
        P first;
        std::memcpy(&first,static_cast<const char *>(items)+il2cpp_array_object_header_size(),sizeof(first));
        return first;
    }
    return nullptr;
}
static bool advanceMenuGuide(FILE *log){
    static auto tap=reinterpret_cast<bool(*)()>(GetProcAddress(GetModuleHandleW(L"GuestRuntime.dll"),"AdvanceMenuGuide"));
    if(!tap || !tap())return false;
    if(log){std::fprintf(log,"Menu navigation guide advanced\n");std::fflush(log);}
    return true;
}
static P screenObjectField(P object,const char *name){
    if(!object)return nullptr;
    auto type=reinterpret_cast<P>(invoke("il2cpp_object_get_class",object));
    auto field=il2cpp_class_get_field_from_name(type,name);
    P value=nullptr;
    if(field)invoke("il2cpp_field_get_value",object,field,&value);
    return value;
}
static bool openSummonInfo(bool rates,FILE *log){
    P type=nullptr;
    P presenter=findScreenComponent("Dark.View.UserInterface","GachaTopScreenPresenter",type);
    P banner=screenObjectField(presenter,"_currentGachaAppealContent");
    if(!banner)return false;
    P owner=rates?presenter:banner;
    type=reinterpret_cast<P>(invoke("il2cpp_object_get_class",owner));
    const char *method=rates?"OpenGachaRate":"ShowDetail";
    P click=il2cpp_class_get_method_from_name(type,method,0),error=nullptr;
    if(!click)return false;
    invoke("il2cpp_runtime_invoke",click,owner,nullptr,&error);
    if(log){std::fprintf(log,"Summons selected banner action=%s exception=%d\n",method,bool(error));std::fflush(log);}
    return true; // A banner action is issued once per replay.
}
static bool tapSubquests(FILE *log) {
    static auto find = reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.GameObject::Find"));
    static auto component = reinterpret_cast<P(*)(P, P)>(registry.at("UnityEngine.GameObject::GetComponent"));
    static auto width = reinterpret_cast<int(*)()>(registry.at("UnityEngine.Screen::get_width"));
    static auto height = reinterpret_cast<int(*)()>(registry.at("UnityEngine.Screen::get_height"));
    static P buttonClass = desktop_skip::type("Dark.View.UserInterface.Parts", "DarkButton");
    if (!buttonClass) return false;
    auto path = reinterpret_cast<P>(invoke("il2cpp_string_new", "/containers/window_system/quest_top/all/event_button"));
    P object = find(path);
    P button = object ? component(object, il2cpp_type_get_object(il2cpp_class_get_type(buttonClass))) : nullptr;
    desktop_skip::HitTest hit;
    float position[2]{};
    const int w = width(), h = height();
    if (!button || !hit.init() || !hit.hits(button, position, w, h)) return false;
    if (log) {
        LARGE_INTEGER counter, frequency;
        QueryPerformanceCounter(&counter);
        QueryPerformanceFrequency(&frequency);
        std::fprintf(log, "Subquests tap qpcMs=%.4f\n", 1000.0*counter.QuadPart/frequency.QuadPart);
        std::fflush(log);
    }
    return desktop_skip::call(desktop_skip::classOf(button), button, "OnTap", {position});
}
static void openRequestedScreen(unsigned actor,int frame,FILE *log){
    static const char *request=std::getenv("REINCOMP_OPEN_SCREEN");
    if(!request)return;
    struct Destination {const char *key,*ns,*name;};
    static constexpr Destination screens[]={
        {"Gacha","Dark.View.UserInterface","GachaTopScreenView"},
        {"Organization","Dark.View.UserInterface","OrganizationTopScreenView"},
        {"Enhance","Dark.View.UserInterface","EnhanceTopScreenView"},
        {"Quest","Dark.View.UserInterface.Outgame.Quest","QuestTopScreenView"},
        {"Map","Dark.View.UserInterface.Outgame.Quest","WorldMapScreenView"},
        {"Library","Dark.View.UserInterface.OutGame","LibraryTopScreenView"},
        {"Menu","Dark.View.UserInterface.OutGame","MenuTopScreenView"}
    };
    static size_t index=0;
    static bool menuOpened=false,enteredTab=false,arrived=false,finished=false;
    static bool fieldTapped=false,interludeSinceOpen=false;
    if(finished)return;
    static const bool all=!std::strcmp(request,"All");
    static const ULONGLONG hold=[]{
        const char *value=std::getenv("REINCOMP_SCREEN_HOLD_SECONDS");
        return value?1000ull*std::clamp(std::atoi(value),5,300):5000ull;
    }();
    static const bool summonRates=!std::strcmp(request,"SummonRates");
    static const bool summonInfo=summonRates || !std::strcmp(request,"SummonNotice");
    static const bool subquests=!std::strcmp(request,"Subquests");
    static bool infoDispatched=false;
    const char *target=subquests?"Quest":(summonInfo?"Gacha":(all?screens[index].key:request));
    static unsigned attempts=0;
    static unsigned newsTaps=0;
    static ULONGLONG next=0,started=0;
    auto now=GetTickCount64();
    if(now<next)return;
    if(arrived){
        static const char *path=std::getenv("REINCOMP_SCREEN_CAPTURE_FILE");
        if(path){
            std::string output(path);
            if(all){
                size_t ext=output.find_last_of('.');
                output.insert(ext==output.npos?output.size():ext,std::string("-")+target);
            }
            auto capture=reinterpret_cast<void(*)(P,int,int)>(registry.at("UnityEngine.ScreenCapture::CaptureScreenshot"));
            capture(reinterpret_cast<P>(invoke("il2cpp_string_new",output.c_str())),1,1);
        }
        if(!all || ++index==std::size(screens)){finished=true;return;}
        arrived=false;requestedScreenVisible=false;attempts=0;started=0;next=now+2000;
        return;
    }
    // Loading.Out also runs after the title and first-login movies. The
    // 3D actor and the field's menu button must both be available.
    if(!menuOpened && (actor&9)!=9)return;
    next=now+2000;
    P type=nullptr;
    if(!started)started=now;
    if(subquests && enteredTab){
        static bool tapped=false;
        constexpr auto ns="Dark.View.UserInterface.Outgame.Quest";
        if(findScreenComponent(ns,"EventQuestTopScreenView",type)){
            if(log){std::fprintf(log,"Screen route arrived at EventQuestTopScreenView\n");std::fflush(log);}
            arrived=true;requestedScreenVisible=true;next=now+hold;
        }else if(now-started>=30000){
            if(log){std::fprintf(log,"Screen route timed out opening Subquests\n");std::fflush(log);}
            finished=true;
        }else if(!tapped){
            tapped=tapSubquests(log);
        }
        return;
    }
    if(summonInfo && enteredTab){
        if(!infoDispatched)infoDispatched=openSummonInfo(summonRates,log);
        if(infoDispatched && game_pages::finishedPage(summonRates?L"/gacha-rate":L"/information/")){
            if(log){std::fprintf(log,"Screen route arrived at %s (embedded page finished)\n",request);std::fflush(log);}
            requestedScreenVisible=true;finished=true;
        }else if(now-started>=90000){
            if(log){std::fprintf(log,"Screen route timed out opening %s\n",request);std::fflush(log);}
            finished=true;
        }
        return;
    }
    if(menuOpened && !std::strcmp(target,"Mama") &&
       findScreenComponent("Dark.View.UserInterface","MenuIcon",type)){
        if(log){std::fprintf(log,"Screen route arrived at Mama Menu\n");std::fflush(log);}
        arrived=true;requestedScreenVisible=true;next=now+5000;return;
    }
    for(const auto &screen:screens){
        if(!std::strcmp(target,screen.key) && findScreenComponent(screen.ns,screen.name,type)){
            if(subquests){
                enteredTab=true;started=now;return;
            }
            if(summonInfo){
                enteredTab=true;started=now;next=now+3000;return;
            }
            if(log){std::fprintf(log,"Screen route arrived at %s\n",screen.name);std::fflush(log);}
            arrived=true;enteredTab=true;requestedScreenVisible=true;next=now+hold;return;
        }
    }
    if(now-started>=(newsTaps?150000u:90000u)){
        if(log){std::fprintf(log,"Screen route timed out opening %s after %u footer taps\n",target,attempts);std::fflush(log);}
        if(!all || ++index==std::size(screens)){finished=true;return;}
        started=0;attempts=0;return;
    }
    if(menuOpened && advanceMenuGuide(log))return;
    if(attempts>=3)return;
    P object=nullptr;
    std::string method;
    if(!menuOpened){
        object=findScreenComponent("Dark.View.HeadUpDisplay.Menu","MomMenuButtonAction",type);
        if(object && !fieldTapped){
            // Field controls fade out while idle, including in the EX chamber.
            // Wake them through the same touch path as a tap on empty ground.
            static auto edge=reinterpret_cast<void(*)(bool,float,float)>(
                GetProcAddress(GetModuleHandleW(L"EngineCalls.dll"),"DesktopMouseEdge"));
            if(edge){
                edge(true,.5f,.3f);
                edge(false,.5f,.3f);
                fieldTapped=true;
                next=now+650;
                return;
            }
        }
        method="OnTapMomMenuButton";
    }else if(!enteredTab){
        // The footer is already instantiated behind Mama's room. Its handlers
        // switch tabs, bypassing the room's exit animation and HUD cleanup.
        P room=findScreenComponent("Dark.View.UserInterface","MomMenuController",type);
        if(!room){
            // A scheduled Mama's News broadcast can take the field first.
            // Let its on-screen Skip control finish before reopening the room.
            if(newsTaps<24){
                object=findScreenComponent("Dark.View.HeadUpDisplay.MomPromotion","MomPromotionContentPlayer",type);
                const char *action="OnTapSkipButton";
                if(!object){
                    object=findScreenComponent("Dark.View.HeadUpDisplay.LoginBonus","LoginBonusPresenter",type);
                    action="Skip";
                }
                if(object){
                    ++newsTaps;
                    interludeSinceOpen=true;
                    auto skip=il2cpp_class_get_method_from_name(type,action,0);
                    P error=nullptr;
                    if(skip)invoke("il2cpp_runtime_invoke",skip,object,nullptr,&error);
                    if(log)std::fprintf(log,"Screen route advanced interlude %s callback=%d exception=%d\n",action,!!skip,!!error);
                    return;
                }
            }
            // Retry only after a real interlude interrupted the request.
            // Reopening Mama's menu while the first request is pending can
            // leave duplicate UI work running after entering a battle tab.
            if(interludeSinceOpen && (actor&9)==9){
                menuOpened=false;fieldTapped=false;interludeSinceOpen=false;attempts=0;next=now+2000;
            }
            return;
        }
        std::string fieldName=std::string("_")+target+"Icon";
        fieldName[1]=char(std::tolower(static_cast<unsigned char>(fieldName[1])));
        auto field=il2cpp_class_get_field_from_name(type,fieldName.c_str());
        if(!field)return;
        invoke("il2cpp_field_get_value",room,field,&object);
        if(!object)return;
        type=reinterpret_cast<P>(invoke("il2cpp_object_get_class",object));
        method="OnTap";
    }else{
        object=findScreenComponent("Dark.View.HeadUpDisplay.Footer","FooterButton",type);
        method=std::string("OnTap")+target;
    }
    P callback=object?il2cpp_class_get_method_from_name(type,method.c_str(),0):nullptr,error=nullptr;
    if(callback){
        if(menuOpened && target==std::string_view("Menu")){
            menuTransitionUntil=GetTickCount64()+5000;
            LARGE_INTEGER counter,frequency;
            if(log && QueryPerformanceCounter(&counter) && QueryPerformanceFrequency(&frequency))
                std::fprintf(log,"Menu tab tap qpcMs=%.4f\n",1000.0*counter.QuadPart/frequency.QuadPart);
        }
        static auto openCount=reinterpret_cast<unsigned(*)()>(
            GetProcAddress(GetModuleHandleW(L"GuestRuntime.dll"),"MenuOpenRequests"));
        const unsigned before=!menuOpened && openCount?openCount():0;
        invoke("il2cpp_runtime_invoke",callback,object,nullptr,&error);
        if(!menuOpened && !error && openCount && openCount()==before){
            // The field handler ignored this tap. Wake the HUD and try again
            // after it becomes interactive. Accepted requests are issued once.
            fieldTapped=false;
            return;
        }
    }
    if(log){
        std::fprintf(log,"Screen route frame=%d action=%s component=%d callback=%d exception=%d\n",
                     frame,method.c_str(),bool(object),bool(callback),bool(error));
        std::fflush(log);
    }
    if(error || !callback){if(object)attempts=3;return;}
    if(menuOpened){++attempts;next=now+10000;}
    else menuOpened=true;
}
