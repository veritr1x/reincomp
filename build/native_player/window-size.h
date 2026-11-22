#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>

namespace window_size {
struct Size { int width{}, height{}, maximized{}; };

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
        static const auto dpiForWindow=reinterpret_cast<UINT(WINAPI *)(HWND)>(
            GetProcAddress(GetModuleHandleW(L"user32"),"GetDpiForWindow"));
        if (!adjust(frame,style&~(WS_MAXIMIZE|WS_MINIMIZE),exStyle,dpiForWindow?dpiForWindow(window):96)) return;
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
}
