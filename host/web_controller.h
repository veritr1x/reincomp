#pragma once
#include <windows.h>
#include <cstdint>

namespace bunker::web_controller {
enum Action : UINT { Press=WM_APP+0x374, Move, Release, Cancel, Scroll, Back };
using HitTest = uint32_t (*)(HWND,POINT *,uint32_t *);
inline void post(HWND window,uint32_t view,Action action,POINT at,int scroll=0){
    const auto value=uint64_t(view)|(uint64_t(static_cast<uint16_t>(scroll))<<32);
    PostMessageW(window,action,static_cast<WPARAM>(value),MAKELPARAM(at.x,at.y));
}
}
