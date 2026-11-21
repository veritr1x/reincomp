namespace desktop_input {
using bunker::desktop_touch::Contact;
using bunker::desktop_touch::Point;
using bunker::desktop_touch::Touch;
using bunker::desktop_touch::thumb;
static int (*frame)(), (*width)(), (*height)(), (*physicalCount)();
static float (*deltaTime)();
static void (*mousePosition)(float *);
static void (*mouseScroll)(float *);
static void (*physicalTouch)(int,Touch *);
static uint8_t (*mouseHeld)(int), (*mouseDown)(int), (*mouseUp)(int), (*keyHeld)(int), (*keyDown)(int);
static Contact pointer, movement, padPointer;
static int realCount;
static std::vector<Touch> fingers;
// The field HUD and DarkScrollRect both expect Android's primary finger ID
// to be zero. Share the ID pool across devices for the lifetime of a press.
static constexpr int64_t mouseFinger = int64_t{1} << 32, padFinger = mouseFinger + 1;
static std::vector<std::optional<int64_t>> fingerIds;
static int fingerId(int64_t source) {
    auto slot = std::find(fingerIds.begin(), fingerIds.end(), source);
    if (slot == fingerIds.end()) {
        slot = std::find(fingerIds.begin(), fingerIds.end(), std::nullopt);
        if (slot == fingerIds.end()) slot = fingerIds.emplace(slot, source);
        else *slot = source;
    }
    return static_cast<int>(slot - fingerIds.begin());
}
static void sampleFingers(void (*read)(int,Touch *)){
    fingers.resize(realCount);
    for(int i=0;i<realCount;++i)read(i,&fingers[i]);
    for(auto &id:fingerIds) {
        if (!id) continue;
        if (*id == mouseFinger) { if (!pointer.held) id.reset(); }
        else if (*id == padFinger) { if (!padPointer.held) id.reset(); }
        else if (std::none_of(fingers.begin(), fingers.end(), [&](const Touch &t) {
            return static_cast<uint32_t>(t.id) == *id;
        })) id.reset();
    }
    for (auto &finger : fingers) finger.id = fingerId(static_cast<uint32_t>(finger.id));
}
static bool padBack, lastBack;
static bool sparseMouse=std::getenv("REINCOMP_COMPARE_MENU_POINTER")==nullptr, needMouse=true;
static uint64_t mouseChecks=0, idleChecks=0;
enum class MouseAction { Down, Move, Up };
struct MouseEdge { MouseAction action; Point client; };
static std::mutex edgeMutex;
static std::deque<MouseEdge> edges;
static std::atomic<bool> hasEdges{};
// Keep at most two positions between Unity samples: the farthest excursion
// and the endpoint. A fast drag can return to the pressed button in one frame.
struct MousePath {
    bool held=false, moved=false;
    Point start{}, farthest{}, last{};
    float distanceSquared=0;

    void move(Point at){
        if(at.x==last.x && at.y==last.y)return;
        const float dx=at.x-start.x,dy=at.y-start.y;
        const float distance=dx*dx+dy*dy;
        if(distance>=distanceSquared){
            distanceSquared=distance;
            farthest=at;
        }
        last=at;
        moved=true;
    }
    void flush(){
        if(!moved)return;
        if(farthest.x!=last.x || farthest.y!=last.y)edges.push_back({MouseAction::Move,farthest});
        edges.push_back({MouseAction::Move,last});
        start=farthest=last;
        distanceSquared=0;
        moved=false;
    }
};
static MousePath mousePath;
static void mouseDrag(float x,float y){
    std::lock_guard lock(edgeMutex);
    if(!mousePath.held)return;
    mousePath.move({x,y});
    if(mousePath.moved)hasEdges.store(true,std::memory_order_release);
}
static void mouseEdge(bool down,float x,float y){
    std::lock_guard lock(edgeMutex);
    const Point at{x,y};
    if(down){
        mousePath={true,false,at,at,at,0};
    }else if(mousePath.held){
        mousePath.move(at);
        mousePath.flush();
        mousePath.held=false;
    }
    edges.push_back({down?MouseAction::Down:MouseAction::Up,at});
    hasEdges.store(true,std::memory_order_release);
}
static void samplePointer(bool down,bool held,bool up,bool cancel,Point position,int w,int h,float dt){
    constexpr int id=0; // Replaced with the allocated ID if the contact is visible.
    static bool windowPress=false;
    static Point dragOrigin{};
    static bool crossedThreshold=false;
    auto motion=[&](Point at){
        if(windowPress && !crossedThreshold){
            const float dx=at.x-dragOrigin.x,dy=at.y-dragOrigin.y;
            const float distance=std::hypot(dx,dy);
            const float threshold=std::max(8.f,std::min(w,h)*.0125f);
            if(distance>=threshold){
                crossedThreshold=true;
                // ScrollRect anchors its content at the first drag event.
                // Give a coalesced swipe a threshold crossing before its
                // endpoint, so OnDrag can measure the remaining displacement.
                if(distance>2*threshold)
                    return Point{dragOrigin.x+dx*(threshold/distance),dragOrigin.y+dy*(threshold/distance)};
            }
        }
        return at;
    };
    if(hasEdges.load(std::memory_order_acquire)){
        std::lock_guard lock(edgeMutex);
        if(cancel){edges.clear();mousePath={};}
        for(;;){
            if(edges.empty())mousePath.flush();
            if(edges.empty())break;
            const auto edge=edges.front();
            Point at{edge.client.x*w,edge.client.y*h};
            if(edge.action!=MouseAction::Down && pointer.held &&
               (std::fabs(at.x-pointer.touch.position.x)>1 || std::fabs(at.y-pointer.touch.position.y)>1)){
                const Point position=motion(at);
                if(edge.action==MouseAction::Move && position.x==at.x && position.y==at.y){
                    edges.pop_front();
                    hasEdges.store(!edges.empty() || mousePath.moved,std::memory_order_release);
                }
                pointer.sample(id,false,true,false,false,at,position,dt);
                return;
            }
            edges.pop_front();
            hasEdges.store(!edges.empty() || mousePath.moved,std::memory_order_release);
            if(edge.action==MouseAction::Move)continue;
            const bool pressed=edge.action==MouseAction::Down;
            if(pressed==pointer.held)continue;
            windowPress=pressed;
            if(pressed){dragOrigin=at;crossedThreshold=false;}
            pointer.sample(id,pressed,pressed,!pressed,false,at,at,dt);
            return;
        }
        hasEdges.store(mousePath.moved,std::memory_order_release);
    }
    if(!pointer.held || cancel || up || !held)windowPress=false;
    pointer.sample(id,down,held,up,cancel,position,windowPress?motion(position):position,dt);
}

static bool controller(HWND window,XINPUT_GAMEPAD &result){
    static DWORD index=XUSER_MAX_COUNT;
    static ULONGLONG nextSearch=0;
    XINPUT_STATE state{};
    if(index<XUSER_MAX_COUNT && XInputGetState(index,&state)==ERROR_SUCCESS){result=state.Gamepad;return true;}
    index=XUSER_MAX_COUNT;
    auto now=GetTickCount64();
    if(now>=nextSearch){
        nextSearch=now+1000;
        for(DWORD i=0;i<XUSER_MAX_COUNT;++i){
            if(XInputGetState(i,&state)==ERROR_SUCCESS){index=i;result=state.Gamepad;return true;}
        }
    }
    return desktop_gamepad::readPlayStation(window,result);
}
static Point controllerCursor(HWND window,Point axes,float dt,Point original,int w,int h){
    if(!axes.x && !axes.y)return original;
    RECT bounds{};
    POINT at{};
    if(!GetClientRect(window,&bounds) || !GetCursorPos(&at) || !ScreenToClient(window,&at) ||
       bounds.right<1 || bounds.bottom<1)return original;
    // Keep subpixel motion from a lightly deflected stick across frames.
    static Point fraction{};
    const float scale=bounds.bottom*std::fmin(dt,0.05f);
    const float dx=axes.x*scale+fraction.x,dy=-axes.y*scale+fraction.y;
    const int ix=int(dx),iy=int(dy);
    fraction={dx-ix,dy-iy};
    at.x=std::clamp<LONG>(at.x+ix,0,bounds.right-1);
    at.y=std::clamp<LONG>(at.y+iy,0,bounds.bottom-1);
    Point position{float(at.x)*w/bounds.right,float(bounds.bottom-1-at.y)*h/bounds.bottom};
    if(ClientToScreen(window,&at))SetCursorPos(at.x,at.y);
    return position;
}
// Keep a press with its original page when the cursor crosses its border
// or the page closes before the player releases the button.
struct BrowserPad {
    HWND window{};
    uint32_t contact{};
    POINT last{};
    bool pressed=false,back=false,consumePress=false,consumeBack=false;
    float fraction=0;

    template<class Send>
    bool route(HWND owner,bool connected,XINPUT_GAMEPAD &pad,float dt,bunker::web_controller::HitTest hit,Send send){
        using namespace bunker::web_controller;
        POINT at{};uint32_t dashboard=0;
        const uint32_t target=connected&&hit?hit(owner,&at,&dashboard):0;
        const bool a=(pad.wButtons&XINPUT_GAMEPAD_A)!=0,b=(pad.wButtons&XINPUT_GAMEPAD_B)!=0;
        if(contact&&(!connected||window!=owner)){
            send(window,contact,Cancel,last,0);contact=0;
        }
        if(!connected){
            consumePress|=pressed;consumeBack|=back;fraction=0;
            return false;
        }
        if(a&&!pressed&&target){
            window=owner;contact=target;last=at;consumePress=true;
            send(window,contact,Press,at,0);
        }else if(contact){
            if(!a)send(window,contact,Release,at,0);
            else if(at.x!=last.x||at.y!=last.y)send(window,contact,Move,at,0);
            last=at;
        }
        if(!a){contact=0;consumePress=false;}
        if(a&&dashboard)consumePress=true;
        if(consumePress)pad.wButtons&=~XINPUT_GAMEPAD_A;
        pressed=a;
        if(b&&!back&&dashboard){consumeBack=true;send(owner,dashboard,Back,at,0);}
        if(!b)consumeBack=false;
        if(consumeBack)pad.wButtons&=~XINPUT_GAMEPAD_B;
        back=b;
        if(target||dashboard){
            float axis=thumb(pad.sThumbLX,pad.sThumbLY,XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE).y;
            if(pad.wButtons&XINPUT_GAMEPAD_DPAD_UP)axis=1;
            if(pad.wButtons&XINPUT_GAMEPAD_DPAD_DOWN)axis=-1;
            if(target&&axis){
                fraction-=axis*900.f*std::clamp(dt,0.f,.05f);
                const int delta=static_cast<int>(fraction);
                if(delta){send(owner,target,Scroll,at,delta);fraction-=delta;}
            }else fraction=0;
            pad.sThumbLX=pad.sThumbLY=0;
            pad.wButtons&=~(XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_DPAD_DOWN|XINPUT_GAMEPAD_DPAD_LEFT|XINPUT_GAMEPAD_DPAD_RIGHT);
        }else fraction=0;
        return dashboard!=0;
    }
};
static BrowserPad browserPad;
static void advance(){
    if(!frame || !physicalCount || !width || !height || !mouseHeld || !mouseDown || !mouseUp ||
       !mousePosition || !keyHeld || !deltaTime)return;
    static int previous=-1;
    const int current=frame();
    if(previous==current)return;
    previous=current;
    HWND window=GetForegroundWindow();
    DWORD pid=0;
    GetWindowThreadProcessId(window,&pid);
    const bool focused=pid==GetCurrentProcessId();
    realCount=physicalCount();
    if(!realCount || physicalTouch)sampleFingers(physicalTouch);
    int w=width(), h=height();
    float dt=deltaTime(), mouse[3]{};
    mousePosition(mouse);
    Point pos{mouse[0],mouse[1]};
    bool button=mouseHeld(0)!=0;
    if(sparseMouse){
        static Point last{-1,-1};
        static int oldWidth=0,oldHeight=0;
        static bool wasFocused=false;
        static ULONGLONG refresh=0;
        float wheel[2]{};
        if(mouseScroll)mouseScroll(wheel);
        auto now=GetTickCount64();
        needMouse=pos.x!=last.x || pos.y!=last.y || w!=oldWidth || h!=oldHeight ||
                  focused!=wasFocused || wheel[0] || wheel[1] ||
                  button || mouseDown(0) || mouseUp(0) ||
                  mouseHeld(1) || mouseDown(1) || mouseUp(1) ||
                  mouseHeld(2) || mouseDown(2) || mouseUp(2) || now>=refresh;
        if(needMouse)refresh=now+75;
        last=pos;
        oldWidth=w;oldHeight=h;wasFocused=focused;
    }
    // Unity mirrors the primary physical touch as a mouse on some drivers.
    static bool suppressMouse=false;
    if(realCount)suppressMouse=true;
    else if(!button && !mouseUp(0))suppressMouse=false;
    samplePointer(mouseDown(0),button,mouseUp(0),!focused || suppressMouse,pos,w,h,dt);
    if(pointer.visible)pointer.touch.id=fingerId(mouseFinger);
    static const bool tracePointer=std::getenv("REINCOMP_TRACE_DESKTOP_INPUT")!=nullptr;
    if(tracePointer && pointer.visible && pointer.touch.phase!=bunker::desktop_touch::Stationary){
        POINT client{};RECT bounds{};
        GetCursorPos(&client);ScreenToClient(window,&client);GetClientRect(window,&bounds);
        if(auto log=std::fopen("desktop-input.log","a")){
            std::fprintf(log,"frame=%d focused=%d down=%d up=%d real=%d visible=%d phase=%d touch=%.0f,%.0f delta=%.0f,%.0f unity=%.0f,%.0f/%d,%d client=%ld,%ld/%ld,%ld\n",
                current,focused,mouseDown(0),mouseUp(0),realCount,pointer.visible,int(pointer.touch.phase),
                pointer.touch.position.x,pointer.touch.position.y,pointer.touch.delta.x,pointer.touch.delta.y,
                pos.x,pos.y,w,h,client.x,client.y,bounds.right,bounds.bottom);
            std::fclose(log);
        }
    }
    if(tracePointer && !fingers.empty()){
        for(int index=0;index<realCount;++index){
            const auto &contact=fingers[index];
            if(contact.phase==bunker::desktop_touch::Stationary)continue;
            if(auto log=std::fopen("desktop-input.log","a")){
                std::fprintf(log,"frame=%d physical=%d id=%d phase=%d position=%.0f,%.0f\n",
                    current,realCount,contact.id,int(contact.phase),contact.position.x,contact.position.y);
                std::fclose(log);
            }
        }
    }

    XINPUT_GAMEPAD pad{};
    const bool padConnected=focused && controller(window,pad);
    auto aim=thumb(pad.sThumbRX,pad.sThumbRY,XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
    if(aim.x || aim.y)needMouse=true;
    if(focused)pos=controllerCursor(window,aim,dt,pos,w,h);
    static const auto hit=reinterpret_cast<bunker::web_controller::HitTest>(
        GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),"DesktopWebTarget"));
    const bool dashboard=browserPad.route(window,padConnected,pad,dt,hit,bunker::web_controller::post);
    lastBack=padConnected && padBack;
    padBack=(pad.wButtons&XINPUT_GAMEPAD_B)!=0;
    padPointer.sample(0,false,pad.wButtons&XINPUT_GAMEPAD_A,false,!padConnected||dashboard,pos,pos,dt);
    if(padPointer.visible)padPointer.touch.id=fingerId(padFinger);

    bunker::windows::keyboard_touch::Stick keys;
    if(focused&&!dashboard){
        auto active=[](int key){return keyHeld(key) || (keyDown && keyDown(key));};
        keys.Set('W',active('w') || (pad.wButtons&XINPUT_GAMEPAD_DPAD_UP));
        keys.Set('A',active('a') || (pad.wButtons&XINPUT_GAMEPAD_DPAD_LEFT));
        keys.Set('S',active('s') || (pad.wButtons&XINPUT_GAMEPAD_DPAD_DOWN));
        keys.Set('D',active('d') || (pad.wButtons&XINPUT_GAMEPAD_DPAD_RIGHT));
    }
    auto stick=keys.Target(w,h);
    auto analog=thumb(pad.sThumbLX,pad.sThumbLY,XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
    Point origin{stick.origin_x,h-stick.origin_y};
    Point target=stick.active?Point{stick.x,h-stick.y}:Point{origin.x+analog.x*h*.15f,origin.y+analog.y*h*.15f};
    bool moving=stick.active || analog.x || analog.y;
    // The game records the floating stick's center at Began. Its old pixel
    // coordinates are no longer valid after a resize or fullscreen switch.
    static int stickWidth=w,stickHeight=h;
    static bool resizingStick=false,usedController=false;
    resizingStick=(w!=stickWidth || h!=stickHeight) && (movement.held || (resizingStick && moving));
    const bool cancelMovement=!focused || dashboard || w<=0 || h<=0 ||
        resizingStick || (usedController && !padConnected && !keys.Any());
    stickWidth=w;stickHeight=h;
    usedController=padConnected && (analog.x || analog.y || (pad.wButtons &
        (XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_DPAD_DOWN|XINPUT_GAMEPAD_DPAD_LEFT|XINPUT_GAMEPAD_DPAD_RIGHT)));
    static Point lastTarget{};
    if(moving)lastTarget=target;
    else if(!cancelMovement && movement.held && movement.touch.phase==bunker::desktop_touch::Began){
        // A short key tap can begin and end between Unity updates. The
        // floating stick needs a motion sample after its initial contact.
        moving=true;target=lastTarget;
    }
    if(movement.held && !cancelMovement){
        auto p=bunker::windows::keyboard_touch::Step(movement.touch.position.x,movement.touch.position.y,
                                                    target.x,target.y,h*std::fmin(dt,0.05f)*2.4f);
        target={p.first,p.second};
    }
    movement.sample(0x40000001,false,moving,false,cancelMovement,origin,target,dt);
    if(tracePointer && movement.visible && movement.touch.phase!=bunker::desktop_touch::Stationary){
        if(auto log=std::fopen("desktop-input.log","a")){
            std::fprintf(log,"frame=%d movement phase=%d position=%.0f,%.0f delta=%.0f,%.0f\n",
                current,int(movement.touch.phase),movement.touch.position.x,movement.touch.position.y,
                movement.touch.delta.x,movement.touch.delta.y);
            std::fclose(log);
        }
    }
}
static void count(State &s,void *){
    advance();
    // The Global 3.7.1 raw gesture collector calls Input.touchCount here.
    // A keyboard/stick drag belongs to that collector. Passing the same
    // contact to UGUI makes the full-screen HUD claim its pointer ID before
    // the original field gesture detector checks IsPointerOverGameObject.
    const bool fieldGestures=s.gpr.x30.qword==0x827da4f0;
    s.gpr.x0.qword=realCount+pointer.visible+padPointer.visible+(fieldGestures && movement.visible);
}
static void touch(State &s,void *fn){
    advance();
    int index=static_cast<int>(s.gpr.x0.dword);
    if(index>=0 && index<realCount){
        if(fingers.empty())sampleFingers(reinterpret_cast<decltype(physicalTouch)>(fn));
        std::memcpy(reinterpret_cast<void *>(s.gpr.x1.qword),&fingers[index],sizeof(Touch));
        return;
    }
    if(index>=realCount){
        int synthetic=index-realCount;
        // Keep the movement-only contact last so both views use the same
        // indices for the physical fingers, mouse and controller pointer.
        for(const auto *contact:{&pointer,&padPointer,&movement}){
            if(contact->visible && synthetic--==0){
                std::memcpy(reinterpret_cast<void *>(s.gpr.x1.qword),&contact->touch,sizeof(Touch));
                return;
            }
        }
    }
    invoke<void,int32_t,P>(s,fn);
}
template<int edge> static void key(State &s,void *fn){
    if(s.gpr.x0.dword==27){
        advance();
        bool active=edge==0?padBack:edge==1?padBack&&!lastBack:!padBack&&lastBack;
        if(active){s.gpr.x0.qword=1;return;}
    }
    invoke<uint8_t,int32_t>(s,fn);
}
static Call bind(const char *keyName,void *fn){
    std::string_view keyNameView(keyName);
    if(keyNameView=="UnityEngine.Time::get_frameCount")frame=reinterpret_cast<decltype(frame)>(fn);
    else if(keyNameView=="UnityEngine.Time::get_unscaledDeltaTime")deltaTime=reinterpret_cast<decltype(deltaTime)>(fn);
    else if(keyNameView=="UnityEngine.Screen::get_width")width=reinterpret_cast<decltype(width)>(fn);
    else if(keyNameView=="UnityEngine.Screen::get_height")height=reinterpret_cast<decltype(height)>(fn);
    else if(keyNameView=="UnityEngine.Input::GetMouseButton")mouseHeld=reinterpret_cast<decltype(mouseHeld)>(fn);
    else if(keyNameView=="UnityEngine.Input::GetMouseButtonDown")mouseDown=reinterpret_cast<decltype(mouseDown)>(fn);
    else if(keyNameView=="UnityEngine.Input::GetMouseButtonUp")mouseUp=reinterpret_cast<decltype(mouseUp)>(fn);
    else if(keyNameView=="UnityEngine.Input::get_mousePosition_Injected")mousePosition=reinterpret_cast<decltype(mousePosition)>(fn);
    else if(keyNameView=="UnityEngine.Input::get_mouseScrollDelta_Injected")mouseScroll=reinterpret_cast<decltype(mouseScroll)>(fn);
    else if(keyNameView=="UnityEngine.Input::get_mousePresent")return [](State &s,void *original){
        // Global 3.7.1 StandaloneInputModule.Process, immediately after its
        // BaseInput.mousePresent call. Only its idle ProcessMouseEvent raycast
        // can be deferred. Every other consumer sees physical mouse presence.
        if(sparseMouse && s.gpr.x30.qword==0x83d27ba0){
            advance();++mouseChecks;
            if(!needMouse && !realCount && !pointer.visible && !padPointer.visible && !movement.visible){
                s.gpr.x0.qword=0;++idleChecks;return;
            }
        }
        invoke<uint8_t>(s,original);
    };
    else if(keyNameView=="UnityEngine.Input::get_touchCount"){physicalCount=reinterpret_cast<decltype(physicalCount)>(fn);return count;}
    else if(keyNameView=="UnityEngine.Input::GetTouch_Injected"){physicalTouch=reinterpret_cast<decltype(physicalTouch)>(fn);return touch;}
    else if(keyNameView=="UnityEngine.Input::get_touchSupported")return [](State &s,void *){s.gpr.x0.qword=1;};
    else if(keyNameView=="UnityEngine.Input::GetKeyInt"){keyHeld=reinterpret_cast<decltype(keyHeld)>(fn);return key<0>;}
    else if(keyNameView=="UnityEngine.Input::GetKeyDownInt"){keyDown=reinterpret_cast<decltype(keyDown)>(fn);return key<1>;}
    else if(keyNameView=="UnityEngine.Input::GetKeyUpInt")return key<2>;
    return nullptr;
}
}
