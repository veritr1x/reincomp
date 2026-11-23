#pragma once
#include "../native_player/desktop-touch.h"
#include "keyboard_touch.h"
#include "runtime.h"
#include <functional>
#include <string_view>

class LinuxDesktopInput {
    using Contact=bunker::desktop_touch::Contact;
    using Point=bunker::desktop_touch::Point;
    using Touch=bunker::desktop_touch::Touch;
    int (*frame)()=nullptr,(*count)()=nullptr,(*width)()=nullptr,(*height)()=nullptr;
    float (*delta)()=nullptr;
    void (*position)(float *)=nullptr,(*touch)(int,Touch *)=nullptr;
    uint8_t (*held)(int)=nullptr,(*down)(int)=nullptr,(*up)(int)=nullptr,(*key)(int)=nullptr;
    uint8_t (*focus)()=nullptr;
    int lastFrame=-1,physical=0,previousWidth=0,previousHeight=0;
    bool mirroredMouse=false;
    Contact pointer,movement;

    void sample() {
        if (!frame || !count || !width || !height || !delta || !position || !held || !down || !up || !key) return;
        int current=frame();
        if (current==lastFrame) return;
        lastFrame=current;
        physical=count();
        const bool pressed=down(0),button=held(0),released=up(0);
        if (physical) mirroredMouse=true;
        else if (!button && !released) mirroredMouse=false;
        float pos[3]{};
        position(pos);
        const Point at{pos[0],pos[1]};
        const float dt=delta();
        bool inactive=focus && !focus();
        int w=width(),h=height();
        const int mouseId=pointer.held ? pointer.touch.id : (movement.held && movement.touch.id==0 ? 1 : 0);
        pointer.sample(mouseId,pressed,button,released,inactive || mirroredMouse,at,at,dt);
        bunker::windows::keyboard_touch::Stick stick;
        stick.Set('W',key('w') || key(273)); stick.Set('S',key('s') || key(274));
        stick.Set('A',key('a') || key(276)); stick.Set('D',key('d') || key(275));
        auto target=stick.Target(w,h);
        Point origin{target.origin_x,h-target.origin_y}, to{target.x,h-target.y};
        if (movement.held && target.active) {
            auto next=bunker::windows::keyboard_touch::Step(movement.touch.position.x,movement.touch.position.y,
                                                          to.x,to.y,h*std::fmin(dt,.05f)*2.4f);
            to={next.first,next.second};
        }
        bool resized=previousWidth!=w || previousHeight!=h;
        previousWidth=w; previousHeight=h;
        const int stickId=movement.held ? movement.touch.id : (pointer.visible && pointer.touch.id==0 ? 1 : 0);
        movement.sample(stickId,false,target.active,false,inactive || physical || resized,origin,to,dt);
    }
public:
    std::function<void(State &)> bind(std::string_view name,void *fn) {
        if (name=="UnityEngine.Time::get_frameCount") frame=reinterpret_cast<decltype(frame)>(fn);
        else if (name=="UnityEngine.Screen::get_width") width=reinterpret_cast<decltype(width)>(fn);
        else if (name=="UnityEngine.Screen::get_height") height=reinterpret_cast<decltype(height)>(fn);
        else if (name=="UnityEngine.Time::get_unscaledDeltaTime") delta=reinterpret_cast<decltype(delta)>(fn);
        else if (name=="UnityEngine.Input::get_mousePosition_Injected") position=reinterpret_cast<decltype(position)>(fn);
        else if (name=="UnityEngine.Input::GetMouseButton") held=reinterpret_cast<decltype(held)>(fn);
        else if (name=="UnityEngine.Input::GetMouseButtonDown") down=reinterpret_cast<decltype(down)>(fn);
        else if (name=="UnityEngine.Input::GetMouseButtonUp") up=reinterpret_cast<decltype(up)>(fn);
        else if (name=="UnityEngine.Input::GetKeyInt") key=reinterpret_cast<decltype(key)>(fn);
        else if (name=="UnityEngine.Application::get_isFocused") focus=reinterpret_cast<decltype(focus)>(fn);
        else if (name=="UnityEngine.Input::get_touchSupported") return [](State &s){s.gpr.x0.qword=1;};
        else if (name=="UnityEngine.Input::get_touchCount") {
            count=reinterpret_cast<decltype(count)>(fn);
            return [this](State &s){
                sample();
                // Only the original field gesture collector should see the stick.
                bool field=s.gpr.x30.qword==0x827da4f0;
                s.gpr.x0.qword=physical+(!physical && pointer.visible)+(field && !physical && movement.visible);
            };
        } else if (name=="UnityEngine.Input::GetTouch_Injected") {
            touch=reinterpret_cast<decltype(touch)>(fn);
            return [this](State &s){
                sample();
                int index=static_cast<int32_t>(s.gpr.x0.dword);
                auto out=reinterpret_cast<Touch *>(s.gpr.x1.qword);
                if (!physical && index>=0) {
                    for (const Contact *c:{&pointer,&movement}) {
                        if (c->visible && index--==0) { *out=c->touch; return; }
                    }
                }
                touch(static_cast<int32_t>(s.gpr.x0.dword),out);
            };
        }
        return {};
    }
};
