// Compile with MinGW: g++ -std=c++17 borderless-window.cpp -o borderless-window.exe
#include "../window-size.h"
#include <cassert>

int main() {
    const auto originalDesktop=GetThreadDesktop(GetCurrentThreadId());
    const auto name=L"ReincompWindowTest"+std::to_wstring(GetCurrentProcessId());
    HDESK desktop=CreateDesktopW(name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
    assert(desktop && SetThreadDesktop(desktop));
    HWND window=CreateWindowExW(0,L"STATIC",L"Borderless window test",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
        50,50,640,480,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    assert(window);
    RECT before{};
    assert(GetWindowRect(window,&before));
    const auto original=GetWindowLongPtrW(window,GWL_STYLE);
    MONITORINFO monitor{sizeof(monitor)};
    assert(GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor));

    using window_size::Mode;
    window_size::Fullscreen fullscreen;
    assert(fullscreen.set(window,Mode::Borderless));
    assert(fullscreen.set(window,Mode::Borderless));
    RECT bounds{},client{};
    assert(GetWindowRect(window,&bounds) && EqualRect(&bounds,&monitor.rcMonitor));
    const auto style=GetWindowLongPtrW(window,GWL_STYLE);
    assert((style&(WS_POPUP|WS_CAPTION|WS_THICKFRAME))==WS_BORDER);
    assert(!(GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_TOPMOST));
    // A monitor-sized OpenGL client can bypass the desktop compositor.
    assert(GetClientRect(window,&client));
    assert(client.right<bounds.right-bounds.left && client.bottom<bounds.bottom-bounds.top);
    WINDOWPOS lateResize{window,nullptr,bounds.left-1,bounds.top-1,client.right+4,client.bottom+4,0};
    fullscreen.constrain(window,lateResize);
    assert(lateResize.x==bounds.left && lateResize.y==bounds.top &&
           lateResize.cx==bounds.right-bounds.left && lateResize.cy==bounds.bottom-bounds.top);

    // Selecting exclusive while viewing Bunker keeps the menu composed.
    fullscreen.covered=true;
    assert(fullscreen.set(window,Mode::Exclusive));
    assert(fullscreen.mode==Mode::Exclusive && fullscreen.presented==Mode::Borderless);
    assert(fullscreen.toggle()==Mode::Windowed);
    assert(fullscreen.set(window,Mode::Windowed));
    assert(GetWindowRect(window,&bounds) && EqualRect(&bounds,&before));
    assert(GetWindowLongPtrW(window,GWL_STYLE)==original);
    DestroyWindow(window);
    assert(SetThreadDesktop(originalDesktop));
    CloseDesktop(desktop);
}
