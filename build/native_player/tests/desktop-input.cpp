// Compile with MinGW: g++ -std=c++17 -O2 desktop-input.cpp -ldinput8 -ldxguid -o desktop-input.exe
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
static DWORD mockState(DWORD user,XINPUT_STATE *out);
static XINPUT_GAMEPAD pad{};
static DWORD mockState(DWORD user,XINPUT_STATE *out){
    if(!connected || user!=2)return ERROR_DEVICE_NOT_CONNECTED;
    out->Gamepad=pad;return ERROR_SUCCESS;
}
static DWORD owner(HWND,DWORD *pid){*pid=focus?GetCurrentProcessId():0;return 1;}
static HWND childWindow(HWND parent,HWND,LPCWSTR,LPCWSTR){return parent==HWND(2)?HWND(1):nullptr;}
static ULONGLONG clockMs=100;
namespace desktop_gamepad {
static bool sonyConnected=false;
static DIJOYSTATE sony{};
static bool readRecordedPad(HWND,XINPUT_GAMEPAD &state){
    if(!sonyConnected)return false;
    state=playStationState(sony);return true;
}
}
#define GetForegroundWindow() HWND(1)
#define GetWindowThreadProcessId owner
#define FindWindowExW childWindow
#define GetTickCount64() clockMs
#define XInputGetState mockState
#define readPlayStation readRecordedPad
#include "../desktop-input.h"
#undef readPlayStation
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#undef FindWindowExW
#undef GetTickCount64
#undef XInputGetState

static int tick,h=720,w=1280,physical=0;
static float x=51,y=84;
static bool down,held,up;
static std::array<bool,128> keys{};
static std::array<bool,128> pressed{};
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
    auto hit=+[](HWND,POINT *p,uint32_t *overlay){*p=at;*overlay=dashboard;return target;};
    auto post=[&](HWND,uint32_t v,web::Action m,POINT p,int dy){events.push_back({v,m,p,dy});};
    auto sample=[&](WORD buttons,bool connected=true){
        XINPUT_GAMEPAD input{};input.wButtons=buttons;
        browser.route(HWND(1),connected,input,.02f,hit,post);
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
}
int main(){
    checkBrowserController();
    using namespace desktop_input;
    using namespace bunker::desktop_touch;
    frame=[] {return tick;};width=[] {return w;};height=[] {return h;};deltaTime=[] {return .01f;};
    physicalCount=[] {return physical;};physicalTouch=readPhysical; mousePosition=[](float *p){p[0]=x;p[1]=y;p[2]=0;};
    mouseHeld=[](int)->uint8_t{return held;};mouseDown=[](int)->uint8_t{return down;};
    mouseUp=[](int)->uint8_t{return up;};keyHeld=[](int k)->uint8_t{return keys.at(k);};
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
    assert(number(0x83d27da0)==1 && read(0).id==0);
    assert(next()==2 && read(1).delta.x>0 && read(0).phase==Stationary && padBack);
    pad.wButtons=0;pad.sThumbLX=1000;assert(next()==2 && read(0).phase==Ended && read(1).phase==Ended);
    assert(!padBack && lastBack);assert(next()==0);
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
    sony.lX=0;sony.lY=65535;sony.lZ=65535;sony.lRz=0;
    const auto axes=playStationState(sony);
    assert(axes.sThumbLX==-32768 && axes.sThumbLY==-32768 && axes.sThumbRX==32767 && axes.sThumbRY==32767);
    sonyConnected=false;next();next();
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
    connected=false;
    assert(next()==1 && read(0).phase==Canceled);
    assert(next()==0);
    connected=true;pad={};pad.wButtons=XINPUT_GAMEPAD_DPAD_RIGHT;clockMs+=1100;
    assert(next()==1 && read(0).phase==Began);
    keys['d']=true;connected=false;
    assert(next()==1 && read(0).phase==Moved && read(0).delta.x>0);
    keys.fill(false);assert(next()==1 && read(0).phase==Ended);assert(next()==0);
    std::puts("Native input frame, drag, tap, WASD, two physical fingers, focus loss, controller, web pages and resized coordinates passed.");
    return 0;
}
