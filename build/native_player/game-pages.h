// Commands come from the game's UniWebView Java calls. WebView2 belongs to
// the HWND thread. Deliver its callbacks to Unity on the next game frame.
#include "../../host/game_page.h"
#include <deque>
#include <mutex>
#include <string_view>
namespace game_pages {
using bunker::windows::BunkerView;
using bunker::windows::GamePageAction;
struct Command {GamePageAction action;std::wstring id,text;int x,y,w,h,vw,vh;std::wstring extra;};
struct Page {
    std::unique_ptr<BunkerView> view;
    int x=0,y=0,w=1,h=1,vw=1,vh=1;
    bool shown=false,opaque=true;
};
struct Status {std::wstring url;bool back=false,forward=false,finished=false;};
static std::map<std::wstring,Page> pages;
static std::map<std::wstring,Status> status;
static std::deque<Command> commands;
static std::deque<std::wstring> messages;
static std::mutex mutex;
static std::atomic<bool> pending{};
static std::atomic<int> screenW{1},screenH{1};
static constexpr UINT dispatch=WM_APP+0x36c;
static bool finishedPage(std::wstring_view path){
    std::lock_guard lock(mutex);
    for(const auto &[id,page]:status){
        if(!page.finished)continue;
        std::wstring_view url(page.url);
        url=url.substr(0,url.find_first_of(L"?#"));
        if(url.size()>=path.size() && url.substr(url.size()-path.size())==path)return true;
    }
    return false;
}
static void trace(const wchar_t *event,int code,int x=0,int y=0,int w=0,int h=0){
    static const bool enabled=std::getenv("REINCOMP_TRACE_GAME_PAGES")!=nullptr;
    if(enabled)if(auto f=std::fopen("game-pages.log","a")){
        std::fprintf(f,"%ls %d (%d,%d %dx%d)\n",event,code,x,y,w,h);std::fclose(f);
    }
}
static std::wstring jsonText(const std::wstring &in){
    std::wstring out;
    for(wchar_t c:in){
        if(c==L'"'||c==L'\\')out+=L'\\';
        if(c>=32)out+=c;
    }
    return out;
}
static void notify(const std::wstring &id,const std::wstring &event,const std::wstring &data){
    trace(event.c_str(),0);
    std::wstring payload=data;
    if(event==L"PageFinished" || event==L"PageErrorReceived")
        payload=L"{\"identifier\":\"\",\"resultCode\":\""+std::wstring(event==L"PageFinished"?L"200":L"-1")+
                L"\",\"data\":\""+jsonText(data)+L"\"}";
    std::lock_guard lock(mutex);
    if(event==L"PageStarted"||event==L"PageFinished"){
        auto page=pages.find(id);
        if(page!=pages.end()&&page->second.view)
            status[id]={data,page->second.view->History(false,false),page->second.view->History(true,false),event==L"PageFinished"};
    }
    messages.push_back(id+L"@"+event+L"@"+payload);pending=true;
}
static void consume(HWND hwnd,bool bunkerVisible){
    std::deque<Command> batch;
    {std::lock_guard lock(mutex);commands.swap(batch);}
    for(auto &c:batch){
        trace(L"UniWebView command",int(c.action),c.x,c.y,c.w,c.h);
        if(c.action==GamePageAction::Destroy){
            pages.erase(c.id);std::lock_guard lock(mutex);status.erase(c.id);continue;
        }
        auto pos=pages.find(c.id);
        if(pos==pages.end()){
            if(c.action!=GamePageAction::Init)continue;
            auto install=_wgetenv(L"REINCOMP_INSTALL_ROOT");
            if(!install)continue;
            std::filesystem::path root(install);
            pos=pages.try_emplace(c.id).first;
            pos->second.view=std::make_unique<BunkerView>((root/"pods/WebView2Loader.dll").string(),
                (root/"data/game-webview").string(),hwnd,
                [id=c.id](const auto &event,const auto &data){notify(id,event,data);});
        }
        auto &p=pos->second;
        switch(c.action){
            case GamePageAction::Init:case GamePageAction::Frame:case GamePageAction::Size:case GamePageAction::Position:
                if(c.action!=GamePageAction::Size){p.x=c.x;p.y=c.y;}
                if(c.action!=GamePageAction::Position){p.w=c.w;p.h=c.h;}
                p.vw=c.vw;p.vh=c.vh;p.view->PageFrame(p.x,p.y,p.w,p.h,p.vw,p.vh);break;
            case GamePageAction::Load:p.view->Navigate(c.text);break;
            case GamePageAction::Show:case GamePageAction::Hide:
                p.shown=c.action==GamePageAction::Show;p.view->Visible(p.shown&&p.opaque&&!bunkerVisible);
                notify(c.id,p.shown?L"ShowTransitionFinished":L"HideTransitionFinished",c.text);break;
            case GamePageAction::Alpha:
                p.opaque=c.x>0;p.view->Visible(p.shown&&p.opaque&&!bunkerVisible);break;
            case GamePageAction::Background:p.view->Background(c.x,c.y,c.w,c.h);break;
            case GamePageAction::Header:p.view->SetHeader(c.text,c.extra);break;
            case GamePageAction::ClearCache:p.view->ClearCache();break;
            case GamePageAction::ExternalLinks:p.view->OpenExternalLinks(c.x!=0);break;
            case GamePageAction::Back:case GamePageAction::Forward:p.view->History(c.action==GamePageAction::Forward,true);break;
            case GamePageAction::Reload:p.view->Reload();break;
            case GamePageAction::Stop:p.view->Stop();break;
            case GamePageAction::Destroy:break;
        }
    }
}
static void overlay(bool covered){
    for(auto &[id,p]:pages)if(p.shown&&p.opaque)p.view->Visible(!covered);
}
static void frame(int w,int h){
    screenW=w;screenH=h;
    if(!pending.exchange(false))return;
    std::deque<std::wstring> batch;
    {std::lock_guard lock(mutex);messages.swap(batch);}
    auto string=[](const wchar_t *s){return reinterpret_cast<P>(invoke("il2cpp_string_new_utf16",s,wcslen(s)));};
    static auto find=reinterpret_cast<P(*)(P)>(registry.at("UnityEngine.GameObject::Find"));
    static auto send=reinterpret_cast<void(*)(P,P,P,int)>(registry.at("UnityEngine.GameObject::SendMessage"));
    if(auto listener=find(string(L"UniWebViewAndroidStaticListener")))
        for(auto &text:batch)send(listener,string(L"OnJavaMessage"),string(text.c_str()),1);
}
}
