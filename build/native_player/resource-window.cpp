// First-run WebView and its replacement Unity child share this top-level HWND.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include "../../host/bunker_view.h"
#include "window-size.h"

namespace {
using bunker::windows::BunkerView;
constexpr UINT choose = WM_APP+101, player = WM_APP+102, finish = WM_APP+103, toggle = WM_APP+104;
constexpr UINT openProgram = WM_APP+105, resetProgram=WM_APP+106;
constexpr UINT showSetup = WM_APP+107, updateSetup=WM_APP+108;
constexpr UINT_PTR setupTimer=2;
enum { pickArchive=301, pickFolder, deployGame };
std::unique_ptr<BunkerView> page, dashboard;
std::filesystem::path install;
HWND child{};
DWORD playerPid{};
using window_size::Mode;
window_size::Fullscreen fullscreen;
HWND setupHint{}, archiveButton{}, folderButton{}, deployButton{}, setupStatus{};

void layoutSetup(HWND window) {
    if (!setupHint) return;
    RECT r{};
    GetClientRect(window,&r);
    const int margin=16, width=std::max(1L,r.right-2*margin);
    MoveWindow(setupHint,margin,margin,width,40,TRUE);
    int y=margin+44;
    for (HWND button : {archiveButton,folderButton,deployButton}) {
        MoveWindow(button,margin,y,std::min(width,280),30,TRUE);
        y+=36;
    }
    MoveWindow(setupStatus,margin,y,width,std::max(1L,r.bottom-y-margin),TRUE);
}

void showNativeSetup(HWND window) {
    if (setupHint || child) return;
    auto control=[&](const wchar_t *cls,const wchar_t *label,DWORD style,UINT_PTR id) {
        HWND h=CreateWindowExW(0,cls,label,WS_CHILD|WS_VISIBLE|style,0,0,0,0,window,
                                reinterpret_cast<HMENU>(id),GetModuleHandleW(nullptr),nullptr);
        SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
        return h;
    };
    setupHint=control(L"STATIC",L"Bunker setup\nChoose your Android game files, then select Deploy.",0,0);
    archiveButton=control(L"BUTTON",L"Choose resource archive...",WS_TABSTOP|WS_DISABLED,pickArchive);
    folderButton=control(L"BUTTON",L"Copy extracted folder...",WS_TABSTOP|WS_DISABLED,pickFolder);
    deployButton=control(L"BUTTON",L"Deploy",WS_TABSTOP|WS_DISABLED,deployGame);
    setupStatus=control(L"STATIC",L"Checking game files...",SS_NOPREFIX,0);
    layoutSetup(window);
    SetTimer(window,setupTimer,700,nullptr);
    std::cout << "setup-state" << std::endl;
}

void closeNativeSetup(HWND window) {
    KillTimer(window,setupTimer);
    for (HWND control : {setupHint,archiveButton,folderButton,deployButton,setupStatus})
        if (control) DestroyWindow(control);
    setupHint=archiveButton=folderButton=deployButton=setupStatus=nullptr;
}

void fit(HWND window) {
    RECT r{};
    GetClientRect(window, &r);
    layoutSetup(window);
    if (child && IsWindow(child)) PostMessageW(child,WM_APP+0x370,0,0);
    else if (page) page->PageFrame(0, 0, r.right, r.bottom, r.right, r.bottom);
}

bool setMode(HWND window,Mode mode,LPARAM size=0) {
    if (mode!=Mode::Windowed && fullscreen.mode==Mode::Windowed) window_size::save(window,install/"data");
    return fullscreen.set(window,mode,size);
}

void attach(HWND window) {
    // Unity may briefly have a top-level window before it adopts its parent.
    for (HWND parent : {window, static_cast<HWND>(nullptr)}) {
        for (HWND w = nullptr; (w = FindWindowExW(parent,w,L"UnityWndClass",nullptr));) {
            DWORD pid{};
            GetWindowThreadProcessId(w,&pid);
            if (pid != playerPid || !(GetWindowLongPtrW(w,GWLP_USERDATA) & 1)) continue;
            if (GetParent(w) != window) {
                auto style = GetWindowLongPtrW(w,GWL_STYLE);
                SetWindowLongPtrW(w,GWL_STYLE,(style & ~WS_POPUP) | WS_CHILD);
                SetLastError(0);
                if (!SetParent(w,window) && GetLastError()) continue;
            }
            child = w;
            closeNativeSetup(window);
            dashboard.reset();
            if (page) { page->Visible(false); page.reset(); }
            fit(window);
            ShowWindowAsync(w,SW_SHOW);
            PostMessageW(w,WM_APP+0x36f,0,0);
            KillTimer(window,1);
            return;
        }
    }
}

LRESULT CALLBACK events(HWND w,UINT message,WPARAM wp,LPARAM lp) {
    if (fullscreen.mode==Mode::Windowed && (message==WM_EXITSIZEMOVE || message==WM_CLOSE || message==finish))
        window_size::save(w,install/"data");
    if (dashboard && dashboard->Message(w,message,wp,lp)) return 0;
    switch (message) {
    case WM_WINDOWPOSCHANGING: {
        const auto result=DefWindowProcW(w,message,wp,lp);
        fullscreen.constrain(w,*reinterpret_cast<WINDOWPOS *>(lp));
        return result;
    }
    case showSetup: showNativeSetup(w); return 0;
    case updateSetup: {
        std::unique_ptr<std::wstring> status(reinterpret_cast<std::wstring *>(lp));
        if (setupHint) {
            EnableWindow(archiveButton,wp & 1); EnableWindow(folderButton,wp & 1);
            EnableWindow(deployButton,wp & 2);
            SetWindowTextW(setupStatus,status->c_str());
        }
        return 0;
    }
    case WM_COMMAND:
        if (setupHint && HIWORD(wp)==BN_CLICKED) {
            switch (LOWORD(wp)) {
            case pickArchive: std::cout << "setup-choose" << std::endl; return 0;
            case pickFolder:  std::cout << "setup-folder" << std::endl; return 0;
            case deployGame:  std::cout << "setup-deploy" << std::endl; return 0;
            }
        }
        break;
    case resetProgram: dashboard.reset(); if(page)page->Visible(true);return 0;
    case openProgram: {
        std::unique_ptr<std::wstring> path(reinterpret_cast<std::wstring *>(lp));
        if (!page || child) return 0;
        if (!dashboard) dashboard=std::make_unique<BunkerView>((install/L"pods/WebView2Loader.dll").wstring(),
            (install/L"data/bunker-view.json").wstring(),(install/L"data/setup-webview").wstring());
        dashboard->OpenDashboard(w,*path);
        return 0;
    }
    case WM_APP+0x36d: if (page) page->Visible(!wp); return 0;
    case choose: {
        if (wp==4 || wp==5) {
            IFileOpenDialog *dialog=nullptr;
            IShellItem *selected=nullptr;
            PWSTR path=nullptr;
            const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) {
                FILEOPENDIALOGOPTIONS options{};
                dialog->GetOptions(&options);
                dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR);
                dialog->SetTitle(L"Choose the extracted Android game files");
                if (SUCCEEDED(dialog->Show(w)) && SUCCEEDED(dialog->GetResult(&selected)))
                    selected->GetDisplayName(SIGDN_FILESYSPATH,&path);
            }
            if (path) std::cout << (wp==4?"picked-folder":"picked-in-place") << "\t" << std::filesystem::path(path).u8string() << std::endl;
            else std::cout << "cancel" << std::endl;
            CoTaskMemFree(path);
            if (selected) selected->Release();
            if (dialog) dialog->Release();
            if (SUCCEEDED(initialized)) CoUninitialize();
            return 0;
        }
        wchar_t file[32768]{};
        OPENFILENAMEW dlg{sizeof(dlg)};
        dlg.hwndOwner=w; dlg.lpstrFile=file; dlg.nMaxFile=32768;
        dlg.lpstrFilter=wp==3?L"Master data (*.bin.e)\0*.bin.e\0\0":wp==2?L"Save backup (*.zip)\0*.zip\0\0":wp==1?L"Save backup (*.zip, *.db)\0*.zip;*.db\0\0":L"Resource archives (*.7z, *.zip)\0*.7z;*.zip\0\0";
        dlg.lpstrTitle=wp==3?L"Import master data":wp==2?L"Export save backup":wp==1?L"Import save backup":L"Choose the Android resource dump";
        dlg.lpstrDefExt=wp==2?L"zip":nullptr;
        dlg.Flags=(wp==2?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST)|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_EXPLORER;
        if (wp==2?GetSaveFileNameW(&dlg):GetOpenFileNameW(&dlg))
            std::cout << (wp==3?"import-master":wp==2?"export-save":wp==1?"import-save":"picked") << "\t" << std::filesystem::path(file).u8string() << std::endl;
        else std::cout << "cancel" << std::endl;
        return 0;
    }
    case player: playerPid=DWORD(wp); child=nullptr; SetTimer(w,1,50,nullptr); return 0;
    case WM_TIMER:
        if (wp==1 && playerPid) attach(w);
        if (wp==setupTimer && setupHint) std::cout << "setup-state" << std::endl;
        return 0;
    case WM_EXITSIZEMOVE:
    case WM_SETFOCUS: if (child) PostMessageW(child,WM_APP+0x36f,0,0); break;
    case WM_DPICHANGED: {
        if (fullscreen.mode==Mode::Windowed) {
            const auto *target=reinterpret_cast<const RECT *>(lp);
            SetWindowPos(w,nullptr,target->left,target->top,target->right-target->left,target->bottom-target->top,SWP_NOZORDER|SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_SIZE: fit(w); return 0;
    case window_size::applyDisplay: {
        const auto mode=Mode(wp&~WPARAM(2));
        if (mode!=Mode::Windowed && mode!=Mode::Borderless && mode!=Mode::Exclusive) return 0;
        if (!setMode(w,mode,mode==Mode::Exclusive?lp:0)) return -1;
        if ((wp&2) && mode==Mode::Windowed) window_size::resizeClient(w,HIWORD(lp),LOWORD(lp));
        return 1;
    }
    case WM_SYSKEYDOWN: if (wp!=VK_RETURN || (lp&(LPARAM(1)<<30))) break; [[fallthrough]];
    case toggle: setMode(w,fullscreen.toggle(),fullscreen.exclusiveSize); return 0;
    case WM_ACTIVATE:
        if (fullscreen.mode==Mode::Exclusive) PostMessageW(w,window_size::refreshDisplay,LOWORD(wp)==WA_INACTIVE,0);
        break;
    case window_size::coverDisplay:
        fullscreen.covered=wp!=0;
        PostMessageW(w,window_size::refreshDisplay,0,0);return 0;
    case window_size::refreshDisplay: fullscreen.present(w,!wp && GetForegroundWindow()==w);return 0;
    case WM_CLOSE:
        if (child && IsWindow(child)) { PostMessageW(child,WM_CLOSE,0,0); return 0; }
        [[fallthrough]];
    case finish: DestroyWindow(w); return 0;
    case WM_DESTROY: fullscreen.release(); dashboard.reset(); page.reset(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w,message,wp,lp);
}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    int argc=0;
    auto args=CommandLineToArgvW(GetCommandLineW(),&argc);
    if (!args || argc!=3) return 2;
    std::filesystem::path root(args[1]);
    install=root;
    std::wstring url(args[2]); LocalFree(args);
    auto setDpi=reinterpret_cast<BOOL(WINAPI *)(HANDLE)>(GetProcAddress(GetModuleHandleW(L"user32"),"SetProcessDpiAwarenessContext"));
    if (setDpi) setDpi(reinterpret_cast<HANDLE>(-4));
    WNDCLASSW wc{};
    wc.lpszClassName=L"BunkerResourceWindow"; wc.lpfnWndProc=events; wc.hInstance=instance;
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); wc.hIcon=ExtractIconW(instance,(root/L"game/WindowsPlayer.exe").c_str(),0);
    if (!RegisterClassW(&wc)) return 1;
    const DWORD style=WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN;
    const auto getDpi=reinterpret_cast<UINT(WINAPI *)()>(GetProcAddress(GetModuleHandleW(L"user32"),"GetDpiForSystem"));
    const UINT dpi=getDpi?getDpi():96;
    const auto saved=window_size::read(root/"data");
    RECT client{0,0,saved.width?saved.width:MulDiv(960,dpi,96),saved.height?saved.height:MulDiv(520,dpi,96)};
    window_size::adjust(client,style,0,dpi);
    RECT work{0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};
    SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
    const LONG width=std::min(client.right-client.left,work.right-work.left);
    const LONG height=std::min(client.bottom-client.top,work.bottom-work.top);
    auto w=CreateWindowExW(0,wc.lpszClassName,L"Nier Reincarnation",style,
        work.left+(work.right-work.left-width)/2,work.top+(work.bottom-work.top-height)/2,width,height,nullptr,nullptr,instance,nullptr);
    if (!w) return 1;
    // WebView2 discovery and the first ShowWindow can block in a cold Wine prefix.
    std::cout << "window\t" << reinterpret_cast<uintptr_t>(w) << std::endl;
    page=std::make_unique<BunkerView>((root/L"pods/WebView2Loader.dll").wstring(),(root/L"data/setup-webview").wstring(),
        w,[w](const std::wstring &event,const std::wstring &){
            if (event==L"WebViewUnavailable") PostMessageW(w,showSetup,0,0);
        });
    fit(w); page->OpenExternalLinks(true); page->Navigate(url); page->Visible(true);
    ShowWindow(w,saved.maximized?SW_SHOWMAXIMIZED:SW_SHOW);
    std::thread([w]{
        std::string line;
        while (std::getline(std::cin,line)) {
            if (line.size()>=8 && line.compare(0,6,"setup ")==0 && line[6]>='0' && line[6]<='3' && line[7]=='\t') {
                const char *text=line.c_str()+8;
                const int size=MultiByteToWideChar(CP_UTF8,0,text,-1,nullptr,0);
                if (!size) continue;
                auto status=std::make_unique<std::wstring>(size,L'\0');
                MultiByteToWideChar(CP_UTF8,0,text,-1,status->data(),size);
                if (PostMessageW(w,updateSetup,line[6]-'0',reinterpret_cast<LPARAM>(status.get()))) status.release();
            } else if (line=="choose") PostMessageW(w,choose,0,0);
            else if (line=="choose-folder" || line=="choose-in-place") PostMessageW(w,choose,line=="choose-folder"?4:5,0);
            else if (line=="reset-program") PostMessageW(w,resetProgram,0,0);
            else if (line=="import-master") PostMessageW(w,choose,3,0);
            else if (line=="import-save" || line=="export-save") PostMessageW(w,choose,line=="import-save"?1:2,0);
            else if (line.compare(0,8,"program ")==0) {
                auto path=std::make_unique<std::wstring>(line.begin()+8,line.end());
                if (PostMessageW(w,openProgram,0,reinterpret_cast<LPARAM>(path.get()))) path.release();
            }
            else if (line=="borderless" || line=="windowed") PostMessageW(w,window_size::applyDisplay,line=="borderless",0);
            else if (line.rfind("player ",0)==0) PostMessageW(w,player,strtoul(line.c_str()+7,nullptr,10),0);
        }
        PostMessageW(w,finish,0,0);
    }).detach();
    MSG msg{};
    while (GetMessageW(&msg,nullptr,0,0)>0) {
        if (setupHint && IsDialogMessageW(w,&msg)) continue;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return 0;
}
