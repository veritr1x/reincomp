#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace bunker::desktop_touch {
struct Point { float x=0, y=0; };
enum Phase : int { Began, Moved, Stationary, Ended, Canceled };
// The pinned UnityEngine.Input.GetTouch copies 0x44 bytes to its caller.
struct Touch {
    int id;
    Point position, raw, delta;
    float seconds;
    int taps;
    Phase phase;
    int type;
    float pressure, maxPressure, radius, radiusVariance, altitude, azimuth;
};
static_assert(sizeof(Touch)==68 && offsetof(Touch,phase)==36 && offsetof(Touch,delta)==20);

struct Contact {
    Touch touch{};
    bool held=false;
    bool visible=false;
    void sample(int id, bool pressed, bool heldNow, bool released, bool cancel,
                Point origin, Point position, float seconds) {
        visible=false;
        if(!held && (cancel || !(pressed || heldNow)))return;
        auto previous=held?touch.position:origin;
        Phase phase;
        if(!held){phase=Began;position=origin;}
        else if(cancel){phase=Canceled;position=previous;}
        else if(released || !heldNow)phase=Ended;
        else phase=position.x!=previous.x || position.y!=previous.y?Moved:Stationary;
        touch={id,position,position,{position.x-previous.x,position.y-previous.y},
               seconds,1,phase,0,1,1,1,0,0,0};
        visible=true;
        held=phase!=Ended && phase!=Canceled;
    }
};
inline Point thumb(int16_t x,int16_t y,float deadZone){
    float magnitude=std::hypot(float(x),float(y));
    if(magnitude<=deadZone)return {};
    float scaled=(std::fmin(magnitude,32767.f)-deadZone)/((32767.f-deadZone)*magnitude);
    return {x*scaled,y*scaled};
}
}
