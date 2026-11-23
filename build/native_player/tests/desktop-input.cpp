// Compile with MinGW: g++ -std=c++17 -O2 desktop-input.cpp -ldinput8 -ldxguid -lhid -o desktop-input.exe
#include <windows.h>
#include <xinput.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string_view>
#include "../desktop-touch.h"
#include "../desktop-gamepad.h"
#include "../../../host/keyboard_touch.h"
#include "../../../host/web_controller.h"

struct Register {union {uint64_t qword;uint32_t dword;};};
struct State {struct {Register x0,x1,x30;} gpr;};
using Call=void(*)(State &,void *);
using P=void *;
template<class R,class... A> void invoke(State &s,void *fn){
    if constexpr(std::is_void_v<R>)reinterpret_cast<void(*)(int,P)>(fn)(s.gpr.x0.dword,reinterpret_cast<P>(s.gpr.x1.qword));
    else s.gpr.x0.qword=reinterpret_cast<R(*)(int)>(fn)(s.gpr.x0.dword);
}
static bool focus=true,connected=false;
static HWND keyboardFocus=HWND(1);
static int windowsKey=0;
static BOOL keyboardInfo(DWORD,GUITHREADINFO *info){info->hwndFocus=keyboardFocus;return TRUE;}
static SHORT heldWindowsKey(int key){return key==windowsKey?SHORT(0x8000):0;}
static bool walkTurnEnabled=false;
static FARPROC playerExport(HMODULE module,LPCSTR name){
    if(!std::strcmp(name,"SetDesktopWalkTurn"))
        return reinterpret_cast<FARPROC>(+[](bool enabled){walkTurnEnabled=enabled;});
    return GetProcAddress(module,name);
}
static RECT clientBounds{0,0,640,360};
static POINT cursor{260,145};
static int warps=0;
static BOOL readBounds(HWND,RECT *r){*r=clientBounds;return TRUE;}
static BOOL readCursor(POINT *p){*p=cursor;return TRUE;}
static BOOL toClient(HWND,POINT *p){p->x-=100;p->y-=55;return TRUE;}
static BOOL toScreen(HWND,POINT *p){p->x+=100;p->y+=55;return TRUE;}
static BOOL moveCursor(int x,int y){cursor={x,y};++warps;return TRUE;}
static DWORD mockState(DWORD user,XINPUT_STATE *out);
static XINPUT_GAMEPAD pad{};
static DWORD mockState(DWORD user,XINPUT_STATE *out){
    if(!connected || user!=2)return ERROR_DEVICE_NOT_CONNECTED;
    out->Gamepad=pad;return ERROR_SUCCESS;
}
static DWORD owner(HWND,DWORD *pid){*pid=focus?GetCurrentProcessId():0;return 1;}
static HWND childWindow(HWND parent,HWND,LPCWSTR,LPCWSTR){return parent==HWND(2)?HWND(1):nullptr;}
static ULONGLONG clockMs=100;
namespace controller_cursor {
static BYTE alpha;
static POINT at;
static int draws;
static bool display(HWND window,POINT client,BYTE opacity){
    assert(window==HWND(1));
    ++draws;alpha=opacity;at=client;
    return opacity!=0;
}
}
static int launcherToggles=0;
static BOOL postInputMessage(HWND window,UINT message,WPARAM w,LPARAM l){
    assert(window==HWND(1) && message==bunker::web_controller::ToggleLauncher && !w && !l);
    ++launcherToggles;
    return TRUE;
}
namespace desktop_gamepad {
static bool sonyConnected=false;
static DIJOYSTATE sony{};
static POINT recordedSwipe{};
static bool readRecordedPad(HWND,XINPUT_GAMEPAD &state,POINT &swipe,bool &click){
    if(!sonyConnected)return false;
    swipe=recordedSwipe;recordedSwipe={};
    click=(sony.rgbButtons[13]&0x80)!=0;
    state=playStationState(sony);return true;
}
}
#define GetForegroundWindow() HWND(1)
#define GetGUIThreadInfo keyboardInfo
#define GetAsyncKeyState heldWindowsKey
#define GetProcAddress playerExport
#define GetClientRect readBounds
#define GetCursorPos readCursor
#define ScreenToClient toClient
#define ClientToScreen toScreen
#define SetCursorPos moveCursor
#define GetWindowThreadProcessId owner
#define FindWindowExW childWindow
#define GetTickCount64() clockMs
#define XInputGetState mockState
#define PostMessageW postInputMessage
#define readPlayStation readRecordedPad
#include "../desktop-input.h"
#undef readPlayStation
#undef GetForegroundWindow
#undef GetGUIThreadInfo
#undef GetAsyncKeyState
#undef GetProcAddress
#undef GetClientRect
#undef GetCursorPos
#undef ScreenToClient
#undef ClientToScreen
#undef SetCursorPos
#undef GetWindowThreadProcessId
#undef FindWindowExW
#undef GetTickCount64
#undef XInputGetState
#undef PostMessageW

static int tick,h=720,w=1280,physical=0;
static void checkDualSenseReports(){
    for(bool bluetooth:{false,true}){
        desktop_gamepad::DualSenseReport touch;
        BYTE report[78]{};
        report[0]=bluetooth?0x31:1;
        auto p=report+(bluetooth?2:1);
        DWORD size=bluetooth?78:64;
        POINT delta{};
        p[7]=0xf2;p[8]=0x60;
        p[32]=4;p[33]=50;p[34]=0x40;p[35]=1;p[36]=0x80;
        assert(touch.sample(report,size,delta) && !delta.x && !delta.y);
        assert(touch.pad.wButtons==(XINPUT_GAMEPAD_DPAD_RIGHT|XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_X|
                                   XINPUT_GAMEPAD_Y|XINPUT_GAMEPAD_BACK|XINPUT_GAMEPAD_LEFT_THUMB));
        p[33]=90;p[34]=0x80;
        assert(touch.sample(report,size,delta) && delta.x==40 && delta.y==4);
        assert(!touch.takeClick(false)); // sliding a finger never presses the pad
        p[9]=2;assert(touch.sample(report,size,delta));
        p[9]=0;assert(touch.sample(report,size,delta));
        assert(touch.takeClick(false) && !touch.takeClick(false)); // both edges between frames
        p[9]=2;assert(touch.sample(report,size,delta));
        assert(touch.takeClick(false) && touch.takeClick(false)); // hold without another report
        assert(!touch.takeClick(true) && !touch.takeClick(false)); // held across refocus
        p[9]=0;assert(touch.sample(report,size,delta) && !touch.takeClick(false));
        p[32]=0x80;p[36]=6;p[37]=170;
        assert(touch.sample(report,size,delta) && delta.x==40); // new contact, second slot
        p[37]=150;assert(touch.sample(report,size,delta) && delta.x==20);
        p[32]=7;assert(touch.sample(report,size,delta) && touch.finger==-1);
        p[32]=0x80;assert(touch.sample(report,size,delta) && delta.x==20);
        assert(!touch.sample(report,10,delta)); // Bluetooth simple report has no touch data
    }
}
static float x=51,y=84;
static bool down,held,up;
static std::array<bool,512> keys{};
static std::array<bool,512> pressed{};
static bool sideDown,sideHeld,sideUp;
static std::array<int,2> deviceIds{100,101};
static void readPhysical(int i,desktop_input::Touch *out){
    auto &t=*out;
    t={};t.id=deviceIds.at(i);t.position={float(20*i),100};t.phase=bunker::desktop_touch::Moved;
}
static void checkBrowserController(){
    namespace web=bunker::web_controller;
    desktop_input::BrowserPad browser;
    static POINT at;
    static uint32_t target,dashboard;
    struct Delivered {uint32_t view; web::Action action; POINT point; int scroll;};
    std::vector<Delivered> events;
    auto hit=+[](HWND,POINT p,uint32_t *overlay){assert(p.x==at.x && p.y==at.y);*overlay=dashboard;return target;};
    auto post=[&](HWND,uint32_t v,web::Action m,POINT p,int dy){events.push_back({v,m,p,dy});};
    auto sample=[&](WORD buttons,bool connected=true){
        XINPUT_GAMEPAD input{};input.wButtons=buttons;
        browser.route(HWND(1),connected,input,at,.02f,hit,post);
        return input.wButtons;
    };
    target=10;at={40,50};assert(sample(XINPUT_GAMEPAD_A)==0);
    assert(events.size()==1&&events.back().action==web::Press);
    target=20;at={60,80};assert(sample(XINPUT_GAMEPAD_A)==0);
    assert(events.back().view==10&&events.back().action==web::Move&&events.back().point.y==80);
    sample(0);assert(events.back().view==10&&events.back().action==web::Release);
    target=0;assert(sample(XINPUT_GAMEPAD_A)==XINPUT_GAMEPAD_A);
    target=20;assert(sample(XINPUT_GAMEPAD_A)==XINPUT_GAMEPAD_A);
    sample(0);assert(events.size()==3);
    assert(sample(XINPUT_GAMEPAD_DPAD_DOWN)==0&&events.back().action==web::Scroll&&events.back().scroll>0);
    assert(sample(XINPUT_GAMEPAD_DPAD_UP)==0&&events.back().scroll<0);
    sample(XINPUT_GAMEPAD_A);sample(0,false);assert(events.back().action==web::Cancel);
    target=0;assert(sample(XINPUT_GAMEPAD_A)==0);sample(0);
    assert(sample(XINPUT_GAMEPAD_A)==XINPUT_GAMEPAD_A);sample(0);
    dashboard=30;assert(sample(XINPUT_GAMEPAD_B)==0&&events.back().action==web::Back&&events.back().view==30);
    dashboard=0;assert(sample(XINPUT_GAMEPAD_B)==0);sample(0);
    assert(sample(XINPUT_GAMEPAD_B)==XINPUT_GAMEPAD_B);
    dashboard=30;
    XINPUT_GAMEPAD unplugged{};
    assert(browser.route(HWND(1),false,unplugged,at,.02f,hit,post));
}
int main(){
    _wputenv_s(L"REINCOMP_WINDOW_PARENT",L"2");
    checkBrowserController();
    checkDualSenseReports();
    using namespace desktop_input;
    using namespace bunker::desktop_touch;
    frame=[] {return tick;};width=[] {return w;};height=[] {return h;};deltaTime=[] {return .01f;};
    physicalCount=[] {return physical;};physicalTouch=readPhysical; mousePosition=[](float *p){p[0]=x;p[1]=y;p[2]=0;};
    mouseHeld=[](int i)->uint8_t{return i==3?sideHeld:(i==0&&held);};
    mouseDown=[](int i)->uint8_t{return i==3?sideDown:(i==0&&down);};
    mouseUp=[](int i)->uint8_t{return i==3?sideUp:(i==0&&up);};keyHeld=[](int k)->uint8_t{return keys.at(k);};
    keyDown=[](int k)->uint8_t{return pressed.at(k);};
    auto read=[](int i){State s{};Touch result{};s.gpr.x0.qword=i;s.gpr.x1.qword=uintptr_t(&result);
        touch(s,reinterpret_cast<P>(readPhysical));return result;};
    auto number=[](uint64_t caller){State s{};s.gpr.x30.qword=caller;count(s,nullptr);return int(s.gpr.x0.qword);};
    auto next=[&] {++tick;clockMs+=10;return number(0x827da4f0);};
    assert(inputWindow(HWND(2),HWND(2))==HWND(1));
    assert(!inputWindow(HWND(3),HWND(3)));
    focus=false;assert(!inputWindow(HWND(1),HWND(2)));focus=true;
    for(auto size:{std::pair{960,520},std::pair{1440,900},std::pair{3840,2160}}){
        w=size.first;h=size.second;
        held=true;mouseEdge(true,.75f,.25f);
        assert(next()==1 && read(0).phase==Began);
        assert(next()==1 && read(0).phase==Stationary && read(0).position.x==.75f*w);
        held=false;mouseEdge(false,.75f,.25f);
        assert(next()==1 && read(0).phase==Ended && read(0).position.y==.25f*h);
        assert(next()==0);
    }
    w=1280;h=720;
    assert(next()==0);
    for(HWND owner:{HWND(1),HWND(2)}){
        keyboardFocus=owner;windowsKey='A';
        assert(next()==1 && read(0).phase==Began);
        assert(next()==1 && read(0).delta.x<0);
        windowsKey=0;assert(next()==1 && read(0).phase==Ended);assert(next()==0);
    }
    keyboardFocus=HWND(3);windowsKey='W';assert(next()==0);
    keyboardFocus=HWND(1);windowsKey=0;
    next();windowsKey=VK_SPACE;next();assert(skipRequested);
    skipRequested=false;next();assert(!skipRequested);
    keyboardFocus=HWND(3);next();keyboardFocus=HWND(1);next();assert(!skipRequested);
    windowsKey=0;next();pressed[VK_SPACE]=true;next();assert(!skipRequested);
    pressed[VK_SPACE]=false;windowsKey=VK_SPACE;next();assert(skipRequested);
    skipRequested=false;windowsKey=0;next();
    // Movement shortcuts affect only the floating field stick (never a UGUI finger).
    auto settledRadius=[&]{
        for(int i=0;i<14;++i)next();
        assert(movement.held && number(0x83d27da0)==0);
        return std::hypot(movement.touch.position.x-w*.5f,movement.touch.position.y-h*.5f);
    };
    for(auto [vk,unity]:{std::pair{VK_UP,273},std::pair{VK_DOWN,274},std::pair{VK_LEFT,276},std::pair{VK_RIGHT,275}}){
        windowsKey=vk;assert(next()==1 && read(0).phase==Began);
        keys[unity]=true;windowsKey=0;assert(nearbyint(settledRadius())==nearbyint(h*.15f));
        keys[unity]=false;next();assert(next()==0);
    }
    keys['w']=keys[275]=true;
    const auto runRadius=settledRadius();
    windowsKey=VK_LSHIFT;const auto walkRadius=settledRadius();
    assert(walkTurnEnabled);
    assert(std::abs(walkRadius-.35f*160/2.54f)<.01f && walkRadius<runRadius && movement.touch.phase==Stationary);
    screenDpi=[](){return 96.f;};
    assert(std::abs(settledRadius()-.35f*96/2.54f)<.01f);
    screenDpi=[](){return 0.f;};
    assert(std::abs(settledRadius()-walkRadius)<.01f);
    windowsKey=0;assert(std::abs(settledRadius()-runRadius)<.01f);
    assert(!walkTurnEnabled);
    windowsKey=VK_TAB;assert(std::abs(settledRadius()-walkRadius)<.01f);
    windowsKey=0;assert(std::abs(settledRadius()-walkRadius)<.01f && walkTurnEnabled);
    focus=false;next();assert(!walkTurnEnabled);
    focus=true;assert(std::abs(settledRadius()-walkRadius)<.01f && walkTurnEnabled);
    windowsKey=VK_TAB;assert(std::abs(settledRadius()-runRadius)<.01f);
    windowsKey=0;next();keyboardFocus=HWND(3);windowsKey=VK_TAB;next();
    keyboardFocus=HWND(1);assert(std::abs(settledRadius()-runRadius)<.01f);
    keys.fill(false);windowsKey=0;next();next();
    keyboardFocus=HWND(3);keys[273]=true;windowsKey='R';
    assert(next()==0 && !autoRunRequested);
    keys.fill(false);keyboardFocus=HWND(1);next();assert(!autoRunRequested);
    windowsKey=0;next();windowsKey='R';next();assert(autoRunRequested);
    autoRunRequested=false;next();assert(!autoRunRequested);windowsKey=0;next();
    auto escape=[&](int edge){
        State s{};s.gpr.x0.dword=VK_ESCAPE;
        void *original=reinterpret_cast<void *>(+[](int)->uint8_t{return 0;});
        if(edge==0)key<0>(s,original);
        if(edge==1)key<1>(s,original);
        if(edge==2)key<2>(s,original);
        return s.gpr.x0.qword!=0;
    };
    sideDown=sideHeld=true;next();assert(escape(0)&&escape(1)&&!escape(2));
    sideDown=false;next();assert(escape(0)&&!escape(1));
    sideHeld=false;sideUp=true;next();assert(!escape(0)&&escape(2));
    sideUp=false;keyboardFocus=HWND(3);sideDown=true;next();assert(!escape(1));
    sideDown=false;keyboardFocus=HWND(1);next();
    down=held=true;assert(next()==1);auto start=read(0);assert(start.phase==Began && start.id==0);
    down=false;x+=24;y-=7;assert(next()==1);
    auto drag=read(0);assert(drag.id==start.id && drag.phase==Moved && drag.delta.x==24 && drag.delta.y==-7);
    assert(read(0).phase==Moved);assert(next()==1 && read(0).phase==Stationary);
    up=true;held=false;assert(next()==1 && read(0).phase==Ended);up=false;assert(next()==0);
    down=up=true;assert(next()==1 && read(0).phase==Began);down=up=false;
    assert(next()==1 && read(0).phase==Ended);assert(next()==0);
    // The OS can report both ends before Unity samples the mouse position.
    mouseEdge(true,.25f,.75f);mouseEdge(false,.25f,.25f);
    assert(next()==1 && read(0).phase==Began && read(0).position.y==h*.75f);
    assert(next()==1 && read(0).phase==Moved);
    const float scrollAnchor=read(0).position.y;
    assert(scrollAnchor<h*.75f && scrollAnchor>h*.25f);
    assert(next()==1 && read(0).phase==Moved && read(0).position.y==h*.25f && read(0).delta.y<0);
    assert(next()==1 && read(0).phase==Ended);
    assert(next()==0);
    // The two ends can be on the same button even when the user dragged far
    // enough for ScrollRect to consume the click.
    mouseEdge(true,.25f,.5f);
    mouseDrag(.50f,.5f);
    mouseDrag(.75f,.5f);
    mouseDrag(.40f,.5f);
    mouseEdge(false,.25f,.5f);
    assert(next()==1 && read(0).phase==Began);
    assert(next()==1 && read(0).phase==Moved && read(0).position.x>.25f*w && read(0).position.x<.75f*w);
    assert(next()==1 && read(0).phase==Moved && read(0).position.x==.75f*w);
    assert(next()==1 && read(0).phase==Moved && read(0).position.x==.25f*w && read(0).delta.x<0);
    assert(next()==1 && read(0).phase==Ended && read(0).position.x==.25f*w);
    assert(next()==0);
    // Movement from a press that started outside the game must not start a drag.
    mouseDrag(.75f,.5f);assert(next()==0);
    // Once Unity has seen a held drag, releasing must not replay its old peak.
    x=w*.25f;y=h*.5f;held=true;mouseEdge(true,.25f,.5f);
    assert(next()==1 && read(0).phase==Began);
    x=w*.75f;mouseDrag(.75f,.5f);
    assert(next()==1 && read(0).phase==Moved && read(0).position.x<x);
    assert(next()==1 && read(0).phase==Moved && read(0).position.x==x);
    assert(next()==1 && read(0).phase==Stationary);
    x=w*.25f;mouseDrag(.25f,.5f);held=false;mouseEdge(false,.25f,.5f);
    assert(next()==1 && read(0).phase==Moved && read(0).position.x==x);
    assert(next()==1 && read(0).phase==Ended);assert(next()==0);
    pressed['d']=true;assert(next()==1 && read(0).phase==Began);
    pressed['d']=false;assert(next()==1 && read(0).phase==Moved && read(0).delta.x>0);
    assert(next()==1 && read(0).phase==Ended);assert(next()==0);
    keys['w']=true;assert(next()==1 && read(0).position.y==h*.5f);
    assert(next()==1 && read(0).delta.y>0);keys['d']=true;assert(next()==1 && read(0).delta.x>0);
    physical=2;assert(next()==3 && read(0).id==0 && read(1).id==1 && read(2).id>1);
    deviceIds={101,100};assert(next()==3 && read(0).id==1 && read(1).id==0);
    deviceIds={101,712};assert(next()==3 && read(0).id==1 && read(1).id==0);
    focus=false;assert(next()==3 && read(2).phase==Canceled);
    keys.fill(false);physical=0;focus=true;assert(next()==0);
    connected=true;clockMs+=1100;pad.sThumbLX=32000;pad.wButtons=XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_B;
    assert(next()==2 && read(0).phase==Began && read(1).phase==Began && read(0).id!=read(1).id && read(0).id==0);
    assert(warps==0 && read(0).position.x==320 && read(0).position.y==538);
    assert(number(0x83d27da0)==1 && read(0).id==0);
    assert(next()==2 && read(1).delta.x>0 && read(0).phase==Stationary && padBack);
    pad.wButtons=0;pad.sThumbLX=1000;assert(next()==2 && read(0).phase==Ended && read(1).phase==Ended);
    assert(!padBack && lastBack);assert(next()==0);
    pad.wButtons=XINPUT_GAMEPAD_START;next();assert(!skipRequested && launcherToggles==0);
    pad.wButtons=XINPUT_GAMEPAD_BACK;next();next();assert(launcherToggles==1 && !skipRequested);
    focus=false;next();focus=true;next();assert(launcherToggles==1);
    pad.wButtons=0;next();
    keyboardFocus=HWND(3); // The game is foreground with keyboard focus in its WebView.
    pad.wButtons=XINPUT_GAMEPAD_BACK;next();assert(launcherToggles==2);
    keyboardFocus=HWND(1);pad.wButtons=0;next();
    pad.sThumbRX=32767;next();assert(warps==0 && cursor.x==260);
    assert(controller_cursor::alpha==255);
    const auto aimed=controller_cursor::at;
    assert(aimed.x>cursor.x-100 && aimed.y==cursor.y-55);
    pad.sThumbRX=0;pad.wButtons=XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_Y;
    assert(next()==1 && skipRequested && read(0).position.x==aimed.x*2 && warps==0);
    skipRequested=false;next();assert(!skipRequested && read(0).phase==Stationary);
    pad.wButtons=0;next();assert(read(0).phase==Ended && warps==0);next();
    pad.sThumbRX=1000;clockMs+=870;next();
    assert(controller_cursor::alpha>0 && controller_cursor::alpha<255 && warps==0);
    clockMs+=250;next();assert(!controller_cursor::alpha);
    pad.sThumbRX=32767;next();assert(controller_cursor::alpha==255);
    const auto saved=controller_cursor::at;
    ++cursor.x;next();assert(!controller_cursor::alpha);
    const int hidden=controller_cursor::draws;
    int hoverChecks=0;
    for(int i=0;i<100;++i){
        ++cursor.x;++x;assert(next()==0);hoverChecks+=needMouse;
    }
    assert(hoverChecks==25 && controller_cursor::draws==hidden && warps==0);
    held=down=true;assert(next()==1 && read(0).phase==Began && needMouse);
    down=false;up=true;held=false;assert(next()==1 && read(0).phase==Ended && needMouse);
    up=false;pad.sThumbRX=0;next();
    pad.wButtons=XINPUT_GAMEPAD_A;assert(next()==1 && read(0).position.x==saved.x*2);
    pad.wButtons=0;next();pad.sThumbRX=32767;next();assert(controller_cursor::alpha==255);
    focus=false;next();assert(!controller_cursor::alpha);
    focus=true;pad.sThumbRX=0;next();
    POINT inside=cursor;cursor.x=42;pad.wButtons=XINPUT_GAMEPAD_A;assert(next()==0);
    cursor=inside;assert(next()==0);pad.wButtons=0;next();
    w=800;h=600;keys['a']=true;assert(next()==1 && read(0).position.x==400 && read(0).position.y==300);
    assert(number(0x83d27da0)==0);
    keys.fill(false);next();next();
    connected=false;
    using namespace desktop_gamepad;
    sonyConnected=true;
    sony.lX=sony.lY=sony.lZ=sony.lRz=32768;sony.rgdwPOV[0]=0xffffffff;
    sony.rgbButtons[1]=0x80;sony.rgbButtons[2]=0x80;
    assert(next()==1 && read(0).id==0 && read(0).phase==Began && padBack);
    physical=1;deviceIds={742};
    assert(next()==2 && read(0).id==1 && read(1).id==0);
    physical=0;
    assert(next()==1 && read(0).id==0 && read(0).phase==Stationary);
    sonyConnected=false;
    assert(next()==1 && read(0).phase==Canceled && !padBack && !lastBack);
    sonyConnected=true;sony.rgbButtons[1]=sony.rgbButtons[2]=0;sony.rgdwPOV[0]=4500;
    assert(next()==1 && read(0).phase==Began);
    assert(next()==1 && read(0).delta.x>0 && read(0).delta.y>0);
    POINT beforeSwipe=cursor;
    recordedSwipe={90,-60};next();
    assert(cursor.x==beforeSwipe.x && cursor.y==beforeSwipe.y && controller_cursor::alpha==255);
    const auto swiped=controller_cursor::at;
    assert(swiped.x==beforeSwipe.x-100+30 && swiped.y==beforeSwipe.y-55-20 && !padPointer.visible);
    sony.rgdwPOV[0]=0xffffffff;next();assert(next()==0);
    // A physical pad click remains available alongside a virtual XInput pad.
    connected=true;pad={};clockMs+=1100;
    sony.rgbButtons[13]=0x80;
    assert(next()==1 && read(0).phase==Began && read(0).position.x==swiped.x*w/640.f);
    pad.wButtons=XINPUT_GAMEPAD_A;assert(next()==1 && read(0).phase==Stationary);
    sony.rgbButtons[13]=0;assert(next()==1 && read(0).phase==Stationary);
    pad.wButtons=0;assert(next()==1 && read(0).phase==Ended);assert(next()==0);
    connected=false;
    sony.lX=0;sony.lY=65535;sony.lZ=65535;sony.lRz=0;
    next();assert(controller_cursor::alpha==255);
    const auto axes=playStationState(sony);
    assert(axes.sThumbLX==-32768 && axes.sThumbLY==-32768 && axes.sThumbRX==32767 && axes.sThumbRY==32767);
    sony.rgbButtons[3]=0x80;next();assert(skipRequested && (playStationState(sony).wButtons&XINPUT_GAMEPAD_Y));
    skipRequested=false;next();assert(!skipRequested);
    sony.rgbButtons[3]=0;sony.rgbButtons[8]=0x80;next();assert(launcherToggles==2);
    sony.rgbButtons[9]=0x80;next();next();assert(launcherToggles==3 && !skipRequested);
    sonyConnected=false;next();sonyConnected=true;next();assert(launcherToggles==3);
    sony.rgbButtons[9]=0;next();sony.rgbButtons[9]=0x80;next();assert(launcherToggles==4);
    sonyConnected=false;next();next();assert(!controller_cursor::alpha && warps==0);
    w=1600;h=900;next();
    keys['d']=true;
    assert(next()==1 && read(0).phase==Began);
    assert(next()==1 && read(0).delta.x>0);
    auto oldPosition=read(0).position;
    w=1000;h=650;
    assert(next()==1 && read(0).phase==Canceled && read(0).position.x==oldPosition.x);
    w=960;assert(next()==0);
    w=900;assert(next()==0);
    assert(next()==1 && read(0).phase==Began && read(0).position.x==450 && read(0).position.y==325);
    assert(next()==1 && read(0).delta.x>0 && read(0).delta.y==0);
    w=h=0;
    assert(next()==1 && read(0).phase==Canceled);
    assert(next()==0);
    keys.fill(false);w=1600;h=900;next();
    connected=true;pad={};pad.sThumbLX=32767;clockMs+=1100;
    assert(next()==1 && read(0).phase==Began);
    const auto full=settledRadius();pad.wButtons=XINPUT_GAMEPAD_LEFT_THUMB;
    const auto slow=settledRadius();assert(slow>0 && slow<full);
    pad.sThumbLX=10000;assert(settledRadius()<slow); // lighter tilt retains analog range
    pad.sThumbLX=0;pad.wButtons=XINPUT_GAMEPAD_DPAD_UP;assert(std::abs(settledRadius()-slow)<.01f);
    pad.wButtons|=XINPUT_GAMEPAD_LEFT_THUMB;assert(std::abs(settledRadius()-full)<.01f);
    pad.wButtons=XINPUT_GAMEPAD_X;next();assert(autoRunRequested);autoRunRequested=false;
    pad.sThumbLX=32767;next();assert(!autoRunRequested);
    DIJOYSTATE buttons{};buttons.rgdwPOV[0]=0xffffffff;buttons.rgbButtons[0]=buttons.rgbButtons[10]=0x80;
    assert(playStationState(buttons).wButtons==(XINPUT_GAMEPAD_X|XINPUT_GAMEPAD_LEFT_THUMB));
    connected=false;
    assert(next()==1 && read(0).phase==Canceled);
    assert(next()==0);
    connected=true;pad={};pad.wButtons=XINPUT_GAMEPAD_DPAD_RIGHT;clockMs+=1100;
    assert(next()==1 && read(0).phase==Began);
    keys['d']=true;connected=false;
    assert(next()==1 && read(0).phase==Moved && read(0).delta.x>0);
    keys.fill(false);assert(next()==1 && read(0).phase==Ended);assert(next()==0);
    std::puts("Native input passed: WASD/arrows, walk hold/toggle, auto-run shortcuts, mouse Back, touch, controller, focus, web pages and resize.");
    return 0;
}
