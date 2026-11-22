// Resolve the current scene's own Skip control when a shortcut is pressed.
// Raycast at its centre so a modal, a disabled canvas, or a different scene
// cannot turn the shortcut into a click on an underlying button.
namespace desktop_skip {
static P type(const char *ns,const char *name){
    size_t count=0;
    auto assemblies=static_cast<P *>(il2cpp_domain_get_assemblies(il2cpp_domain_get(),&count));
    for(size_t i=0;i<count;++i)
        if(auto cls=il2cpp_class_from_name(il2cpp_assembly_get_image(assemblies[i]),ns,name))return cls;
    return nullptr;
}
static P classOf(P object){
    return object?reinterpret_cast<P>(invoke("il2cpp_object_get_class",object)):nullptr;
}
// Void methods return null on success too. Report dispatch separately.
static bool call(P cls,P instance,const char *name,std::initializer_list<P> args,P *out=nullptr){
    auto method=cls?il2cpp_class_get_method_from_name(cls,name,int(args.size())):nullptr;
    if(!method)return false;
    P error=nullptr;
    P result=reinterpret_cast<P>(invoke("il2cpp_runtime_invoke",method,instance,args.begin(),&error));
    if(out)*out=result;
    return !error;
}
static P get(P object,const char *name,std::initializer_list<P> args={}){
    P value=nullptr;
    return call(classOf(object),object,name,args,&value)?value:nullptr;
}
template<class T> static bool value(P box,T &out){
    if(!box)return false;
    std::memcpy(&out,reinterpret_cast<P>(invoke("il2cpp_object_unbox",box)),sizeof(out));
    return true;
}
struct HitTest {
    P system{},pointer{},results{},lastHit{};
    bool init(){
        static P systemType=type("UnityEngine.EventSystems","EventSystem");
        static P pointerType=type("UnityEngine.EventSystems","PointerEventData");
        if(!call(systemType,nullptr,"get_current",{},&system) || !system || !pointerType)return false;
        auto cache=screenObjectField(get(system,"get_currentInputModule"),"m_RaycastResultCache");
        if(!cache)return false;
        pointer=reinterpret_cast<P>(invoke("il2cpp_object_new",pointerType));
        results=reinterpret_cast<P>(invoke("il2cpp_object_new",classOf(cache)));
        return pointer && results && call(pointerType,pointer,".ctor",{system}) &&
               call(classOf(results),results,".ctor",{});
    }
    bool hits(P control,float (&position)[2],int width,int height){
        lastHit=nullptr;
        bool active=false;
        if(!value(get(control,"get_isActiveAndEnabled"),active) || !active)return false;
        P transform=get(control,"get_transform");
        float rect[4],world[3];
        if(!value(get(transform,"get_rect"),rect))return false;
        float centre[]={rect[0]+rect[2]*.5f,rect[1]+rect[3]*.5f,0.f};
        if(!value(get(transform,"TransformPoint",{centre}),world))return false;
        static P casterClass=type("UnityEngine.UI","GraphicRaycaster");
        static P casterType=casterClass?il2cpp_type_get_object(il2cpp_class_get_type(casterClass)):nullptr;
        P caster=casterType?get(control,"GetComponentInParent",{casterType}):nullptr;
        if(!caster)return false;
        P camera=get(caster,"get_eventCamera"),box=nullptr;
        static P utility=type("UnityEngine","RectTransformUtility");
        if(!call(utility,nullptr,"WorldToScreenPoint",{camera,world},&box) || !value(box,position))return false;
        if(!(position[0]>=0 && position[0]<width && position[1]>=0 && position[1]<height))return false;
        using bunker::windows::BunkerView;
        if(BunkerView::HasControllerArea()){
            auto window=player_window::attached.load();
            RECT bounds{};
            if(!GetClientRect(window,&bounds))return false;
            auto x=long(position[0]*bounds.right/width);
            auto y=bounds.bottom-1-long(position[1]*bounds.bottom/height);
            uint32_t dashboard=0;
            if(BunkerView::ControllerTarget(window,x,y,&dashboard) || dashboard)return false;
        }
        if(!call(classOf(pointer),pointer,"set_position",{position}) ||
           !call(classOf(system),system,"RaycastAll",{pointer,results}))return false;
        int count=0,index=0;
        if(!value(get(results,"get_Count"),count) || !count)return false;
        // RaycastResult starts with m_GameObject in the pinned Unity player.
        P hit=nullptr;
        if(!value(get(results,"get_Item",{&index}),hit))return false;
        lastHit=hit;
        P hitTransform=get(hit,"get_transform");
        bool descendant=false;
        return hitTransform && value(get(hitTransform,"IsChildOf",{transform}),descendant) && descendant;
    }
};
static void press(int width,int height){
    struct Binding {const char *ns,*owner,*field;};
    static constexpr Binding buttons[]={
        {"Dark.View.MoviePlay","MoviePlayer","_closeButtonForCage"},
        {"Dark.View.MoviePlay","MoviePlayer","_closeButton"},
        {"Dark.View.HeadUpDisplay.Menu","ReplaySkipButtonAction","_skipButton"},
        {"Dark.View.HeadUpDisplay.MomPromotion","MomPromotionContentPlayer","_skipButton"},
        {"Dark.View.HeadUpDisplay.LoginBonus","LoginBonusPresenter","_skipButton"},
        {"Dark.View.HeadUpDisplay.Omikuji","OmikujiAction","_skipButton"},
        {"Dark.View.UserInterface","GachaProductionScreenView","_skipButton"},
        {"Dark.View.HeadUpDisplay.StorySkip","StorySkipAction","_storySkipToggle"},
        {"Dark.View.HeadUpDisplay.StorySkip","StorySkipAction","_battleSkipToggle"},
        // A movie with hidden transport controls first takes a tap to reveal them.
        {"Dark.View.MoviePlay","MoviePlayer","_tapScreenButton"},
        {"Dark.View.Movie","PreTitleMovie","tapScreenButton"},
        {"Dark.View.Title","SplashScreen","_splashNoteTap"},
        {"Dark.View.Title","SplashScreen","_splashTap"},
        {"Dark.View.Title","SplashScreen","_splashSoundTextTap"}
    };
    const char *selected="none";
    HitTest hit;
    bool ready=hit.init();
    auto trace=std::getenv("REINCOMP_TRACE_DESKTOP_INPUT")?std::fopen("desktop-input.log","a"):nullptr;
    if(trace)std::fprintf(trace,"skip raycaster ready=%d\n",ready);
    if(ready)for(const auto &b:buttons){
        P cls=nullptr,owner=findScreenComponent(b.ns,b.owner,cls);
        P button=screenObjectField(owner,b.field);
        float position[2]{};
        if(!button)continue;
        bool matches=hit.hits(button,position,width,height);
        if(trace){
            auto name=reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.Object::GetName"));
            std::fprintf(trace,"skip candidate=%s.%s at=%.0f,%.0f hit=%ls matches=%d\n",
                b.owner,b.field,position[0],position[1],hit.lastHit?playerString(name(hit.lastHit)).c_str():L"(none)",matches);
        }
        if(!matches)continue;
        const char *kind=il2cpp_class_get_name(classOf(button));
        bool enabled=false,pressed=false;
        if(!std::strcmp(kind,"DarkToggle")){
            if(value(get(button,"IsInteractable"),enabled) && enabled)
                pressed=call(classOf(button),button,"OnPointerClick",{hit.pointer});
        }else if(!std::strcmp(b.owner,"SplashScreen")){
            pressed=call(cls,owner,"OnTap",{position});
        }else if(value(get(button,"get_IsEnable"),enabled) && enabled){
            pressed=call(classOf(button),button,"OnTap",{position});
        }
        if(pressed){selected=b.field;break;}
    }
    if(trace){
        std::fprintf(trace,"skip control=%s\n",selected);
        std::fclose(trace);
    }
}
}
