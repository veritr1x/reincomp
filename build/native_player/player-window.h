// The game's Win32 window owns its title, icons and Bunker's WebView2 pane.
#include <commctrl.h>
#include <filesystem>
#include <memory>
#include "../../host/bunker_view.h"
#include "../../host/web_controller.h"
#include "game-pages.h"

namespace player_window {
static std::unique_ptr<bunker::windows::BunkerView> bunker;
static std::atomic<HWND> attached;
static std::atomic<HHOOK> setupHook;
static std::atomic<uint32_t> pendingResolution;

static LRESULT CALLBACK events(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR){
    if(message==WM_SYSKEYDOWN && w==VK_RETURN && (GetWindowLongPtrW(window,GWL_STYLE)&WS_CHILD)){
        if(!(l&(LPARAM(1)<<30)))PostMessageW(GetParent(window),WM_APP+104,0,0);
        return 0;
    }
    if(message==WM_APP+0x36e){
        if(w==0 || w>65535 || l<=0 || l>65535)return 0;
        pendingResolution.store((uint32_t(w)<<16)|uint32_t(l),std::memory_order_release);
        return 1;
    }
    if(message==WM_DEVICECHANGE || message==WM_INPUT_DEVICE_CHANGE){
        if(auto changed=reinterpret_cast<void(*)()>(GetProcAddress(GetModuleHandleW(L"EngineCalls.dll"),"DesktopGamepadsChanged")))changed();
    }
    if(message==game_pages::dispatch){game_pages::consume(window,bunker&&bunker->IsVisible());return 0;}
    if(message==WM_APP+0x36d){game_pages::overlay(w!=0);return 0;}
    if(message==WM_SIZE || message==WM_ACTIVATE || (message>=bunker::web_controller::Press && message<=bunker::web_controller::Back)){
        for(auto &[name,page]:game_pages::pages)if(page.view->Message(window,message,w,l))return 0;
    }
    bool handled=bunker && bunker->Message(window,message,w,l);
    const bool drag=message==WM_MOUSEMOVE && (w&MK_LBUTTON);
    if(!handled && (message==WM_LBUTTONDOWN || message==WM_LBUTTONDBLCLK || message==WM_LBUTTONUP || drag) &&
       !(bunker && bunker->IsVisible())){
        static auto edge=reinterpret_cast<void(*)(bool,float,float)>(GetProcAddress(GetModuleHandleW(L"EngineCalls.dll"),"DesktopMouseEdge"));
        static auto move=reinterpret_cast<void(*)(float,float)>(GetProcAddress(GetModuleHandleW(L"EngineCalls.dll"),"DesktopMouseDrag"));
        RECT bounds{};
        if(edge && GetClientRect(window,&bounds) && bounds.right>0 && bounds.bottom>0){
            const auto x=static_cast<short>(LOWORD(l)),y=static_cast<short>(HIWORD(l));
            const float px=float(x)/bounds.right,py=float(bounds.bottom-1-y)/bounds.bottom;
            if(drag){if(move)move(px,py);}
            else edge(message!=WM_LBUTTONUP,px,py);
        }
    }
    if(message==WM_NCDESTROY){
        RemoveWindowSubclass(window,events,id);
        game_pages::pages.clear();
        bunker.reset();
        attached=nullptr;
    }
    return handled?0:DefSubclassProc(window,message,w,l);
}
static void attach(HWND window){
    if(!SetWindowSubclass(window,events,1,0))return;
    attached=window;
    SetWindowTextW(window,L"Nier Reincarnation");
    for(int small:{0,1}){
        auto icon=LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(101),IMAGE_ICON,
                             GetSystemMetrics(small?SM_CXSMICON:SM_CXICON),GetSystemMetrics(small?SM_CYSMICON:SM_CYICON),LR_SHARED);
        if(icon)SendMessageW(window,WM_SETICON,small?ICON_SMALL:ICON_BIG,reinterpret_cast<LPARAM>(icon));
    }
    if(const auto *path=_wgetenv(L"REINCOMP_INSTALL_ROOT")){
        std::filesystem::path root(path);
        bunker=std::make_unique<bunker::windows::BunkerView>((root/"pods/WebView2Loader.dll").string(),
                (root/"data/bunker-view.json").string(),(root/"data/webview").string());
        PostMessageW(window,game_pages::dispatch,0,0);
    }
}
static bool isPlayer(HWND window){
    wchar_t name[64]{};
    return GetClassNameW(window,name,64) && !wcscmp(name,L"UnityWndClass") && !GetWindow(window,GW_OWNER);
}
static LRESULT CALLBACK onWindowThread(int code,WPARAM w,LPARAM l){
    if(code==HC_ACTION){
        auto window=reinterpret_cast<CWPSTRUCT *>(l)->hwnd;
        if(isPlayer(window)){
            UnhookWindowsHookEx(setupHook);
            setupHook=nullptr;
            attach(window);
        }
    }
    return CallNextHookEx(nullptr,code,w,l);
}
static void install(){
    // The main Unity window can live on a different thread in the player.
    // Install its subclass on the owning message thread.
    static ULONGLONG next=0;
    if(GetTickCount64()<next)return;
    next=GetTickCount64()+1000;
    if(attached || setupHook)return;
    auto find=[](HWND window,LPARAM)->BOOL{
        DWORD pid=0;
        auto thread=GetWindowThreadProcessId(window,&pid);
        if(pid!=GetCurrentProcessId() || !isPlayer(window))return TRUE;
        if(thread==GetCurrentThreadId())attach(window);
        else{
            setupHook=SetWindowsHookExW(WH_CALLWNDPROC,onWindowThread,nullptr,thread);
            if(setupHook)PostMessageW(window,WM_NULL,0,0);
        }
        return FALSE;
    };
    if(const auto *value=_wgetenv(L"REINCOMP_WINDOW_PARENT")){
        auto parent=reinterpret_cast<HWND>(_wcstoui64(value,nullptr,10));
        if(parent && IsWindow(parent))EnumChildWindows(parent,find,0);
    }
    if(!attached && !setupHook)EnumWindows(find,0);
}
}
extern "C" __declspec(dllexport) void EnqueueGamePage(bunker::windows::GamePageAction action,const wchar_t *id,
        const wchar_t *value,int x,int y,int w,int h,const wchar_t *extra){
    if(!id||!*id)return;
    {std::lock_guard lock(game_pages::mutex);
     game_pages::commands.push_back({action,id,value?value:L"",x,y,w,h,game_pages::screenW,game_pages::screenH,extra?extra:L""});}
    if(auto window=player_window::attached.load())PostMessageW(window,game_pages::dispatch,0,0);
}
extern "C" __declspec(dllexport) int QueryGamePage(const wchar_t *id,int query,wchar_t *out,int capacity){
    if(query==0)return game_pages::screenW;
    if(query==1)return game_pages::screenH;
    std::lock_guard lock(game_pages::mutex);
    auto it=game_pages::status.find(id?id:L"");if(it==game_pages::status.end())return 0;
    if(query==2)return it->second.back;
    if(query==3)return it->second.forward;
    const auto &value=it->second.url;
    if(out&&capacity>int(value.size()))std::copy(value.c_str(),value.c_str()+value.size()+1,out);
    return static_cast<int>(value.size());
}
extern "C" __declspec(dllexport) uint32_t DesktopWebTarget(HWND window,POINT *at,uint32_t *dashboard){
    using bunker::windows::BunkerView;
    *dashboard=0;
    if(!BunkerView::HasControllerArea())return 0;
    if(!GetCursorPos(at)||!ScreenToClient(window,at))return 0;
    return BunkerView::ControllerTarget(window,at->x,at->y,dashboard);
}
