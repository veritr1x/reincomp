#include "../../build/linux_player/desktop_input.h"
#include <cassert>
#include <cstdio>

namespace {
int number=0,real=0;
bool mouse=false,press=false,release=false,right=false,focused=true;
using bunker::desktop_touch::Touch;
}

int main() {
    LinuxDesktopInput input;
    auto reg=[&](const char *name,auto f){return input.bind(name,reinterpret_cast<void *>(+f));};
    reg("UnityEngine.Time::get_frameCount",[]{return number;});
    reg("UnityEngine.Time::get_unscaledDeltaTime",[]{return .016f;});
    reg("UnityEngine.Screen::get_width",[]{return 960;});
    reg("UnityEngine.Screen::get_height",[]{return 540;});
    reg("UnityEngine.Application::get_isFocused",[]()->uint8_t{return focused;});
    reg("UnityEngine.Input::get_mousePosition_Injected",[](float *p){p[0]=320;p[1]=250;});
    reg("UnityEngine.Input::GetMouseButton",[](int)->uint8_t{return mouse;});
    reg("UnityEngine.Input::GetMouseButtonDown",[](int)->uint8_t{return press;});
    reg("UnityEngine.Input::GetMouseButtonUp",[](int)->uint8_t{return release;});
    reg("UnityEngine.Input::GetKeyInt",[](int key)->uint8_t{return right && key=='d';});
    auto count=reg("UnityEngine.Input::get_touchCount",[]{return real;});
    auto get=reg("UnityEngine.Input::GetTouch_Injected",[](int index,Touch *p){assert(index==0);p->id=8;});
    State state{};
    Touch contact{};
    auto touches=[&](bool field=false){state.gpr.x30.qword=field ? 0x827da4f0 : 0;count(state);return state.gpr.x0.qword;};
    auto first=[&](){state.gpr.x0.qword=0;state.gpr.x1.qword=reinterpret_cast<addr_t>(&contact);get(state);return contact;};
    using namespace bunker::desktop_touch;
    assert(touches()==0);
    ++number; press=mouse=true; assert(touches()==1 && first().phase==Began && contact.id==0 && contact.position.x==320);
    ++number; press=false; assert(touches()==1 && first().phase==Stationary);
    ++number; mouse=false; release=true; assert(touches()==1 && first().phase==Ended);
    ++number; release=false; right=true; assert(touches()==0 && touches(true)==1 && first().phase==Began && contact.id==0);
    ++number; assert(touches(true)==1 && first().phase==Moved && contact.position.x>480);
    ++number; focused=false; assert(touches(true)==1 && first().phase==Canceled);
    ++number; focused=true; right=false; real=1; mouse=true; assert(touches()==1 && first().id==8);
    ++number; real=0; assert(touches()==0);
    std::puts("PASS native mouse taps, field-only WASD movement, focus cancellation, physical touchscreen priority");
}
