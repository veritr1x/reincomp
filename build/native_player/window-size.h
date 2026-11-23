#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <climits>
#include <cstdlib>

namespace window_size {
// Bit 1 requests a resize. The remaining bits identify the window mode.
// LPARAM packs the requested width in the high word and height in the low word.
inline constexpr UINT applyDisplay=WM_APP+0x371;
inline constexpr UINT refreshDisplay=WM_APP+0x380, coverDisplay=WM_APP+0x381;
enum class Mode : UINT { Windowed=0, Borderless=1, Exclusive=4 };
struct Size { int width{}, height{}, maximized{}; };

inline UINT dpi(HWND window) {
    static const auto get=reinterpret_cast<UINT(WINAPI *)(HWND)>(
        GetProcAddress(GetModuleHandleW(L"user32"),"GetDpiForWindow"));
    return get?get(window):96;
}

inline BOOL adjust(RECT &rect, DWORD style, DWORD exStyle, UINT dpi) {
    static const auto adjustForDpi=reinterpret_cast<BOOL(WINAPI *)(RECT *,DWORD,BOOL,DWORD,UINT)>(
        GetProcAddress(GetModuleHandleW(L"user32"),"AdjustWindowRectExForDpi"));
    return adjustForDpi?adjustForDpi(&rect,style,FALSE,exStyle,dpi):AdjustWindowRectEx(&rect,style,FALSE,exStyle);
}

inline Size read(const std::filesystem::path &data) {
    Size size;
    if (!(std::ifstream(data/"window-size") >> size.width >> size.height >> size.maximized) ||
        size.width<1 || size.width>32767 || size.height<1 || size.height>32767 ||
        (size.maximized!=0 && size.maximized!=1)) return {};
    return size;
}

// Save the restored client size even when the window is maximized or minimized.
inline void save(HWND window, const std::filesystem::path &data) {
    const auto style=static_cast<DWORD>(GetWindowLongPtrW(window,GWL_STYLE));
    if ((style&WS_CHILD) || !(style&WS_CAPTION)) return;
    WINDOWPLACEMENT placement{sizeof(placement)};
    RECT client{};
    if (!GetWindowPlacement(window,&placement) || !GetClientRect(window,&client)) return;
    const bool maximized=IsZoomed(window) || (IsIconic(window) && (placement.flags&WPF_RESTORETOMAXIMIZED));
    if (maximized || IsIconic(window)) {
        RECT frame{};
        const auto exStyle=static_cast<DWORD>(GetWindowLongPtrW(window,GWL_EXSTYLE));
        if (!adjust(frame,style&~(WS_MAXIMIZE|WS_MINIMIZE),exStyle,dpi(window))) return;
        const auto normal=placement.rcNormalPosition;
        client.right=normal.right-normal.left-(frame.right-frame.left);
        client.bottom=normal.bottom-normal.top-(frame.bottom-frame.top);
    }
    if (client.right<1 || client.right>32767 || client.bottom<1 || client.bottom>32767) return;
    const auto path=data/"window-size";
    const auto temp=data/("window-size."+std::to_string(GetCurrentProcessId())+".tmp");
    std::ofstream out(temp,std::ios::trunc);
    out << client.right << ' ' << client.bottom << ' ' << int(maximized) << '\n';
    out.close();
    if (!out || !MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING)) DeleteFileW(temp.c_str());
}

// OpenGL fullscreen uses a temporary Windows video mode on the window's
// monitor. Release it for other apps and for the composited Bunker WebView.
struct Fullscreen {
    Mode mode=Mode::Windowed, presented=Mode::Windowed, lastFullscreen=Mode::Borderless;
    bool covered{};
    WINDOWPLACEMENT previous{sizeof(WINDOWPLACEMENT)};
    LONG_PTR framedStyle{};
    MONITORINFOEXW monitor{sizeof(MONITORINFOEXW)};
    DEVMODEW desktop{},requested{};
    LPARAM exclusiveSize{};

    void constrain(HWND window,WINDOWPOS &pos) const {
        if (mode==Mode::Windowed || IsIconic(window) || (pos.flags&SWP_HIDEWINDOW)) return;
        // Unity can request the old client size after WM_DISPLAYCHANGE. Keep
        // the outer frame on the monitor even when that resize arrives later.
        const RECT r=monitor.rcMonitor;
        if (!(pos.flags&SWP_NOMOVE)) {pos.x=r.left;pos.y=r.top;}
        if (!(pos.flags&SWP_NOSIZE)) {pos.cx=r.right-r.left;pos.cy=r.bottom-r.top;}
    }

    void chooseResolution(LPARAM size) {
        requested=desktop;
        if (!size) return;
        long long bestDistance=LLONG_MAX;
        int bestHz=INT_MAX;
        DEVMODEW candidate{};
        candidate.dmSize=sizeof(candidate);
        for (DWORD i=0;EnumDisplaySettingsW(monitor.szDevice,i,&candidate);++i) {
            if (candidate.dmBitsPerPel!=desktop.dmBitsPerPel || (candidate.dmDisplayFlags&DM_INTERLACED)) continue;
            const long long dx=long(candidate.dmPelsWidth)-HIWORD(size),dy=long(candidate.dmPelsHeight)-LOWORD(size);
            const auto distance=dx*dx+dy*dy;
            const int hz=std::abs(int(candidate.dmDisplayFrequency)-int(desktop.dmDisplayFrequency));
            if (distance<bestDistance || (distance==bestDistance && hz<bestHz)) {
                requested=candidate;
                bestDistance=distance; bestHz=hz;
            }
        }
    }
    bool release() {
        if (presented!=Mode::Exclusive) return true;
        if (ChangeDisplaySettingsExW(monitor.szDevice,&desktop,nullptr,0,nullptr)!=DISP_CHANGE_SUCCESSFUL) return false;
        presented=Mode::Borderless;
        return true;
    }
    bool present(HWND window,bool active,bool force=false) {
        const Mode target=mode==Mode::Exclusive && (!active || covered)?Mode::Borderless:mode;
        if (!force && target==presented) {
            if (target!=Mode::Exclusive) return true;
            // A driver can restore the desktop before a rapid focus round trip
            // has finished dispatching WM_ACTIVATE to the game.
            DEVMODEW actual{}; actual.dmSize=sizeof(actual);
            if (EnumDisplaySettingsW(monitor.szDevice,ENUM_CURRENT_SETTINGS,&actual) &&
                actual.dmPelsWidth==requested.dmPelsWidth && actual.dmPelsHeight==requested.dmPelsHeight) return true;
        }
        if (target!=Mode::Exclusive && !release()) return false;
        if (target==Mode::Exclusive) {
            requested.dmFields=DM_PELSWIDTH|DM_PELSHEIGHT|DM_BITSPERPEL|DM_DISPLAYFREQUENCY;
            if (ChangeDisplaySettingsExW(monitor.szDevice,&requested,nullptr,CDS_FULLSCREEN,nullptr)!=DISP_CHANGE_SUCCESSFUL)
                return false;
        }
        presented=target;
        constexpr UINT flags=SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED;
        if (target==Mode::Windowed) {
            SetWindowLongPtrW(window,GWL_STYLE,framedStyle);
            SetWindowPlacement(window,&previous);
            SetWindowPos(window,nullptr,0,0,0,0,flags|SWP_NOMOVE|SWP_NOSIZE);
        } else {
            const LONG_PTR frame=target==Mode::Exclusive?WS_POPUP:WS_BORDER;
            SetWindowLongPtrW(window,GWL_STYLE,(framedStyle & ~(WS_OVERLAPPEDWINDOW|WS_MAXIMIZE|WS_POPUP)) | frame);
            // Re-query the selected monitor in this window's coordinate space.
            // Display modes are physical pixels even in DPI-virtualized callers.
            EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR h,HDC,LPRECT,LPARAM context)->BOOL {
                auto &selected=*reinterpret_cast<MONITORINFOEXW *>(context);
                MONITORINFOEXW current{sizeof(current)};
                if (!GetMonitorInfoW(h,&current) || wcscmp(current.szDevice,selected.szDevice)) return TRUE;
                selected.rcMonitor=current.rcMonitor;
                return FALSE;
            },reinterpret_cast<LPARAM>(&monitor));
            const auto r=monitor.rcMonitor;
            SetWindowPos(window,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,flags);
        }
        return true;
    }
    bool set(HWND window,Mode next,LPARAM size=0) {
        if (mode==next && (next!=Mode::Exclusive || size==exclusiveSize)) return true;
        if (mode==Mode::Windowed) {
            if (!GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor) ||
                !GetWindowPlacement(window,&previous)) return false;
            framedStyle=GetWindowLongPtrW(window,GWL_STYLE);
        }
        const auto oldResolution=requested;
        if (next==Mode::Exclusive) {
            if (mode!=Mode::Exclusive) {
                desktop.dmSize=sizeof(desktop);
                if (!EnumDisplaySettingsW(monitor.szDevice,ENUM_CURRENT_SETTINGS,&desktop)) return false;
                desktop.dmFields=DM_PELSWIDTH|DM_PELSHEIGHT|DM_BITSPERPEL|DM_DISPLAYFREQUENCY;
            }
            chooseResolution(size);
        }
        const Mode old=mode;
        mode=next;
        if (!present(window,GetForegroundWindow()==window,true)) {
            mode=old; requested=oldResolution;
            return false;
        }
        if(next!=Mode::Windowed)lastFullscreen=next;
        if(next==Mode::Exclusive)exclusiveSize=size;
        return true;
    }
    Mode toggle() const {
        return mode==Mode::Windowed?lastFullscreen:Mode::Windowed;
    }
};

inline void resizeClient(HWND window, int width, int height) {
    if (width<=0 || height<=0) return;
    if (IsZoomed(window)) ShowWindow(window,SW_RESTORE);
    RECT r{0,0,width,height};
    if (adjust(r,GetWindowLongPtrW(window,GWL_STYLE),GetWindowLongPtrW(window,GWL_EXSTYLE),dpi(window)))
        SetWindowPos(window,nullptr,0,0,r.right-r.left,r.bottom-r.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
}
}
