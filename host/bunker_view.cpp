#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include "bunker_view.h"
#include "web_controller.h"
#include "../third_party/webview2/WebView2.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

namespace bunker::windows {
// The portable release linked this view into three modules at different
// revisions. The embedded guest uses the original dashboard, the game adapter
// also takes Unicode paths, and the launcher can open a specific Pod page.
#ifndef REINCOMP_VIEW_REVISION
#define REINCOMP_VIEW_REVISION 2
#endif
namespace {
constexpr UINT_PTR kTimer=0xB0B;
struct ControllerArea {uint32_t id; HWND window; RECT bounds; bool dashboard;};
std::mutex areaMutex;
std::vector<ControllerArea> areas;
std::atomic<unsigned> areaCount{};
std::atomic<uint32_t> nextArea{1};
// The header declares these without defining them.
constexpr GUID kEnvironmentHandler{0x4e8a3389,0xc9d8,0x4bd2,{0xb6,0xb5,0x12,0x4f,0xee,0x6c,0xc1,0x4d}};
constexpr GUID kControllerHandler{0x6c4819f3,0xc9b7,0x4260,{0x81,0x27,0xc9,0xf5,0xbd,0xe7,0xf6,0x8c}};
constexpr GUID kKeyHandler{0xb29c7e28,0xfa79,0x41a8,{0x8e,0x44,0x65,0x81,0x1c,0x76,0xdc,0xb2}};
constexpr GUID kNavigationHandler{0x9adbe429,0xf36d,0x432b,{0x9d,0xdc,0xf8,0x88,0x1f,0xbd,0x76,0xe3}};
constexpr GUID kCompletedHandler{0xd33a35bf,0x1c49,0x4f98,{0x93,0xab,0x00,0x6e,0x05,0x33,0xfe,0x1c}};
constexpr GUID kWebView2{0x9e8f0cf8,0xe670,0x4b5e,{0xb2,0xbc,0x73,0xe0,0x61,0xe3,0x18,0x4c}};

// A COM callback object around a function.
template<class Interface,class... Arguments>
class Handler final:public Interface {
 public:
  using Function=std::function<HRESULT(Arguments...)>;
  Handler(const GUID &id,Function function):id_(id),function_(std::move(function)){}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void **out) override {
    if(!out)return E_POINTER;
    if(IsEqualGUID(id,IID_IUnknown)||IsEqualGUID(id,id_)){*out=static_cast<Interface*>(this);AddRef();return S_OK;}
    *out=nullptr;return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {return ++references_;}
  ULONG STDMETHODCALLTYPE Release() override {const auto left=--references_;if(!left)delete this;return left;}
  HRESULT STDMETHODCALLTYPE Invoke(Arguments... arguments) override {return function_(arguments...);}
 private:
  ~Handler()=default;
  const GUID id_;Function function_;std::atomic<ULONG> references_{1};
};
template<class T>void Drop(T *&pointer){if(pointer){pointer->Release();pointer=nullptr;}}

std::wstring Wide(const std::string &text,UINT code_page=CP_UTF8){
  const int size=MultiByteToWideChar(code_page,0,text.data(),int(text.size()),nullptr,0);
  std::wstring wide(size_t(size),L'\0');MultiByteToWideChar(code_page,0,text.data(),int(text.size()),wide.data(),size);return wide;
}
// The string value of KEY in the flat JSON object the launcher writes.
std::string Field(const std::string &json,const std::string &key){
  auto at=json.find("\""+key+"\"");if(at==std::string::npos)return{};
  at=json.find(':',at);if(at==std::string::npos)return{};
  at=json.find('"',at);if(at==std::string::npos)return{};
  std::string value;
  for(++at;at<json.size()&&json[at]!='"';++at){
    if(json[at]=='\\'&&at+1<json.size()){const char next=json[++at];value+=next=='n'?'\n':next;}else value+=json[at];
  }
  return value;
}
std::wstring Page(const std::wstring &text){
  return L"<!doctype html><meta charset=utf-8><body style=\"margin:0;height:100vh;display:flex;align-items:center;justify-content:center;"
         L"background:#1b1a17;color:#dad4bb;font:18px sans-serif;text-align:center\"><div><p>"+text+L"</p><p style=\"opacity:.6\">Ctrl+B closes this.</p></div>";
}
}  // namespace

struct BunkerView::Impl:std::enable_shared_from_this<Impl> {
  std::wstring loader,state_file,user_data;
  HWND window=nullptr;
  ICoreWebView2Controller *controller=nullptr;ICoreWebView2 *web=nullptr;
  bool visible=false,creating=false,failed=false,loaded=false,waiting=false,closed=false;
  EventRegistrationToken key_token{},navigation_token{};
  EventRegistrationToken completed_token{},request_token{};
  PageEvent pageEvent;
  std::wstring address,origin;
#ifdef REINCOMP_BUNKER_MODULES
  // The running game shares its launcher's loopback UI. Pod Programs still
  // uses the authenticated dashboard session and its existing startup path.
  std::wstring launcherHome;
#endif
#if REINCOMP_VIEW_REVISION >= 2 || defined(REINCOMP_BUNKER_MODULES)
  std::wstring dashboardPath=L"/";
#endif
  std::map<std::wstring,std::wstring> headers;
  bool externalLinks=false;
  COREWEBVIEW2_COLOR background{255,255,255,255};
  int x=0,y=0,width=1,height=1,viewportWidth=1,viewportHeight=1;
  const uint32_t areaId=nextArea.fetch_add(1);
  bool touching=false;

  void UpdateArea(const RECT *bounds){
    std::lock_guard lock(areaMutex);
    auto match=std::find_if(areas.begin(),areas.end(),[&](const auto &a){return a.id==areaId;});
    if(bounds){
      if(match==areas.end())areas.push_back({areaId,window,*bounds,!pageEvent});
      else match->bounds=*bounds;
    }else if(match!=areas.end())areas.erase(match);
    areaCount.store(static_cast<unsigned>(areas.size()),std::memory_order_release);
  }
  void ControllerInput(UINT action,POINT point,int scroll=0){
    using namespace bunker::web_controller;
    if(!web||!controller)return;
    if(action==Back){if(!pageEvent&&visible)Show(false);return;}
    if(!visible&&action!=Cancel)return;
    if(action==Release||action==Cancel){
      if(touching)web->CallDevToolsProtocolMethod(L"Input.dispatchTouchEvent",
          action==Release?LR"({"type":"touchEnd","touchPoints":[]})":LR"({"type":"touchCancel","touchPoints":[]})",nullptr);
      touching=false;return;
    }
    if(action==Move&&!touching)return;
    RECT bounds{};double zoom=1,scale=1;
    if(FAILED(controller->get_Bounds(&bounds)))return;
    controller->get_ZoomFactor(&zoom);
    ICoreWebView2Controller3 *v3=nullptr;
    if(SUCCEEDED(controller->QueryInterface(IID_ICoreWebView2Controller3,reinterpret_cast<void **>(&v3)))){
      v3->get_RasterizationScale(&scale);v3->Release();
    }
    if(zoom<=0||scale<=0||IsRectEmpty(&bounds))return;
    const int px=int((std::clamp(point.x,bounds.left,bounds.right-1)-bounds.left)/(zoom*scale)+0.5);
    const int py=int((std::clamp(point.y,bounds.top,bounds.bottom-1)-bounds.top)/(zoom*scale)+0.5);
    wchar_t json[256];
    if(action==Scroll){
      swprintf(json,256,LR"({"type":"mouseWheel","x":%d,"y":%d,"deltaX":0,"deltaY":%d})",px,py,scroll);
      web->CallDevToolsProtocolMethod(L"Input.dispatchMouseEvent",json,nullptr);
    }else{
      swprintf(json,256,LR"({"type":"%ls","touchPoints":[{"x":%d,"y":%d,"id":1}]})",action==Press?L"touchStart":L"touchMove",px,py);
      if(SUCCEEDED(web->CallDevToolsProtocolMethod(L"Input.dispatchTouchEvent",json,nullptr)))touching=true;
    }
  }

  void ApplyBackground(){
    ICoreWebView2Controller2 *color=nullptr;
    if(controller&&SUCCEEDED(controller->QueryInterface(IID_ICoreWebView2Controller2,reinterpret_cast<void **>(&color)))){
      color->put_DefaultBackgroundColor(background);color->Release();
    }
  }

  void Fit(){
    if(!controller||!window)return;
    RECT client{};GetClientRect(window,&client);
    if(pageEvent){
      auto w=client.right,h=client.bottom;
      client={MulDiv(x,w,viewportWidth),MulDiv(y,h,viewportHeight),
              MulDiv(x+width,w,viewportWidth),MulDiv(y+height,h,viewportHeight)};
    }
    controller->put_Bounds(client);
    if(visible)UpdateArea(&client);
  }
  void Show(bool on){
    if(!on){if(touching)ControllerInput(bunker::web_controller::Cancel,{});UpdateArea(nullptr);}
#ifdef REINCOMP_BUNKER_MODULES
    if(!on && !launcherHome.empty()){dashboardPath.clear();loaded=waiting=false;}
#endif
    visible=on;
    if(controller){
      controller->put_IsVisible(on?TRUE:FALSE);
      if(on){Fit();controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);}
    }
    if(on)Load();else if(!pageEvent){KillTimer(window,kTimer);SetFocus(window);}
    if(!pageEvent&&window)PostMessageW(window,WM_APP+0x36d,on?1:0,0);
    if(!pageEvent)std::fprintf(stderr,"Bunker view: %s\n",on?"shown":"hidden");
  }
  void Toggle(){
    if(failed||closed)return;
    if(!controller&&!creating){Create();return;}
    if(controller)Show(!visible);else visible=!visible;  // still being created: applied when it is ready
  }
  void Fail(const char *what,HRESULT result){
    failed=true;creating=false;visible=false;
    std::fprintf(stderr,"Bunker view: %s failed (0x%08lx). The Microsoft Edge WebView2 Runtime may be missing.\n",what,static_cast<unsigned long>(result));
#if REINCOMP_VIEW_REVISION >= 2
    if(pageEvent)pageEvent(L"WebViewUnavailable",address);
#endif
  }
  void Create(){
    using CreateEnvironment=HRESULT(STDAPICALLTYPE*)(PCWSTR,PCWSTR,ICoreWebView2EnvironmentOptions*,ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(com)&&com!=RPC_E_CHANGED_MODE){Fail("COM",com);return;}
    auto module=LoadLibraryW(loader.c_str());
    FARPROC address=module?GetProcAddress(module,"CreateCoreWebView2EnvironmentWithOptions"):nullptr;
    if(!address){Fail("loading WebView2Loader.dll",HRESULT_FROM_WIN32(GetLastError()));return;}
    CreateEnvironment create=nullptr;static_assert(sizeof(create)==sizeof(address));std::memcpy(&create,&address,sizeof(create));
    creating=true;
    if(!pageEvent)visible=true;
    std::weak_ptr<Impl> weak=shared_from_this();
    auto *on_environment=new Handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,HRESULT,ICoreWebView2Environment*>(
      kEnvironmentHandler,[weak](HRESULT result,ICoreWebView2Environment *environment)->HRESULT{
        auto self=weak.lock();if(!self||self->closed)return S_OK;
        if(FAILED(result)||!environment){self->Fail("creating the web view environment",result);return S_OK;}
        auto *on_controller=new Handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,HRESULT,ICoreWebView2Controller*>(
          kControllerHandler,[weak](HRESULT result,ICoreWebView2Controller *controller)->HRESULT{
            auto self=weak.lock();if(!self||self->closed)return S_OK;
            if(FAILED(result)||!controller){self->Fail("creating the web view",result);return S_OK;}
            self->Ready(controller);return S_OK;
          });
        result=environment->CreateCoreWebView2Controller(self->window,on_controller);on_controller->Release();
        if(FAILED(result))self->Fail("creating the web view",result);
        return S_OK;
      });
    const auto result=create(nullptr,user_data.c_str(),nullptr,on_environment);on_environment->Release();
    if(FAILED(result))Fail("starting WebView2",result);
  }
  void Ready(ICoreWebView2Controller *created){
    controller=created;controller->AddRef();creating=false;
    if(FAILED(controller->get_CoreWebView2(&web))||!web){Fail("reading the web view",E_FAIL);return;}
    ICoreWebView2Settings *settings=nullptr;
    if(SUCCEEDED(web->get_Settings(&settings))&&settings){
      settings->put_AreDevToolsEnabled(FALSE);settings->put_IsStatusBarEnabled(FALSE);settings->Release();
    }
    // Keys the page does not take itself arrive here: Ctrl+B closes the view.
    std::weak_ptr<Impl> weak=shared_from_this();
    auto *on_key=new Handler<ICoreWebView2AcceleratorKeyPressedEventHandler,ICoreWebView2Controller*,ICoreWebView2AcceleratorKeyPressedEventArgs*>(
      kKeyHandler,[weak](ICoreWebView2Controller*,ICoreWebView2AcceleratorKeyPressedEventArgs *arguments)->HRESULT{
        auto self=weak.lock();if(!self||!arguments)return S_OK;
        COREWEBVIEW2_KEY_EVENT_KIND kind{};UINT key=0;
        if(FAILED(arguments->get_KeyEventKind(&kind))||FAILED(arguments->get_VirtualKey(&key)))return S_OK;
        if(key=='B'&&(GetKeyState(VK_CONTROL)<0||GetAsyncKeyState(VK_CONTROL)<0)){
          arguments->put_Handled(TRUE);
          COREWEBVIEW2_PHYSICAL_KEY_STATUS status{};arguments->get_PhysicalKeyStatus(&status);
          if(kind==COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN&&!status.WasKeyDown){
            if(self->pageEvent)PostMessageW(self->window,web_controller::ToggleLauncher,0,0);
            else PostMessageW(self->window,WM_APP+0x36e,0,0);
          }
        }
        return S_OK;
      });
    controller->add_AcceleratorKeyPressed(on_key,&key_token);on_key->Release();
    // Chromium may handle Ctrl+B inside a focused document. In that case
    // the DOM event carries the modifiers from the actual keypress.
    web->AddScriptToExecuteOnDocumentCreated(
        LR"(addEventListener('keydown', e => {
          if (e.isTrusted && e.ctrlKey && !e.altKey && !e.shiftKey && e.code === 'KeyB') {
            e.preventDefault(); e.stopImmediatePropagation();
            if (!e.repeat) location.href = '/bunker/desktop-toggle';
          }
        }, true);)",nullptr);
    // The pages' own Close is a link the app around them takes.
    auto *on_navigation=new Handler<ICoreWebView2NavigationStartingEventHandler,ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs*>(
      kNavigationHandler,[weak](ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs *arguments)->HRESULT{
        auto self=weak.lock();if(!self||!arguments)return S_OK;
        LPWSTR address=nullptr;
        if(SUCCEEDED(arguments->get_Uri(&address))&&address){
          const std::wstring uri(address);CoTaskMemFree(address);
#ifdef REINCOMP_BUNKER_MODULES
          if(!self->launcherHome.empty() && uri.rfind(self->launcherHome+L"open/",0)==0){
            arguments->put_Cancel(TRUE);
            self->dashboardPath=uri.substr(self->launcherHome.size()+4);
            self->loaded=false;
            self->Load();
            return S_OK;
          }
#endif
          const std::wstring toggle=L"/bunker/desktop-toggle";
          if(uri.size()>=toggle.size()&&!uri.compare(uri.size()-toggle.size(),toggle.size(),toggle)){
            arguments->put_Cancel(TRUE);
            PostMessageW(self->window,self->pageEvent?web_controller::ToggleLauncher:WM_APP+0x36e,0,0);return S_OK;
          }
          if(self->pageEvent){
            if(uri.compare(0,self->origin.size(),self->origin)!=0 || self->origin.empty()){
              arguments->put_Cancel(TRUE);
              BOOL user=FALSE;arguments->get_IsUserInitiated(&user);
              if(user&&self->externalLinks&&(uri.rfind(L"https://",0)==0||uri.rfind(L"http://",0)==0))
                ShellExecuteW(self->window,L"open",uri.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
            }else self->pageEvent(L"PageStarted",uri);
            return S_OK;
          }
          const std::wstring close=L"/bunker/close";
          if(uri.size()>=close.size()&&!uri.compare(uri.size()-close.size(),close.size(),close)){
            arguments->put_Cancel(TRUE);
#ifdef REINCOMP_BUNKER_MODULES
            if(!self->launcherHome.empty() && !self->dashboardPath.empty()){
              self->dashboardPath.clear();self->loaded=false;self->Load();return S_OK;
            }
#endif
            self->Show(false);
          }
        }
        return S_OK;
      });
    web->add_NavigationStarting(on_navigation,&navigation_token);on_navigation->Release();
    if(pageEvent){
      web->AddWebResourceRequestedFilter(L"*",COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT);
      auto *request=new Handler<ICoreWebView2WebResourceRequestedEventHandler,ICoreWebView2*,ICoreWebView2WebResourceRequestedEventArgs*>(
        IID_ICoreWebView2WebResourceRequestedEventHandler,[weak](ICoreWebView2*,ICoreWebView2WebResourceRequestedEventArgs *args)->HRESULT{
          auto self=weak.lock();if(!self||!args||self->origin.empty())return S_OK;
          ICoreWebView2WebResourceRequest *req=nullptr;
          if(FAILED(args->get_Request(&req))||!req)return S_OK;
          LPWSTR uri=nullptr;req->get_Uri(&uri);
          if(uri&&std::wstring_view(uri).rfind(self->origin,0)==0){
            ICoreWebView2HttpRequestHeaders *headers=nullptr;
            if(SUCCEEDED(req->get_Headers(&headers))&&headers){
              for(const auto &[name,value]:self->headers)headers->SetHeader(name.c_str(),value.c_str());
              headers->Release();
            }
          }
          CoTaskMemFree(uri);req->Release();return S_OK;
        });
      web->add_WebResourceRequested(request,&request_token);request->Release();
      auto *done=new Handler<ICoreWebView2NavigationCompletedEventHandler,ICoreWebView2*,ICoreWebView2NavigationCompletedEventArgs*>(
        kCompletedHandler,[weak](ICoreWebView2 *sender,ICoreWebView2NavigationCompletedEventArgs *args)->HRESULT{
          auto self=weak.lock();if(!self||!args)return S_OK;
          BOOL ok=FALSE;args->get_IsSuccess(&ok);
          LPWSTR raw=nullptr;sender->get_Source(&raw);
          std::wstring uri=raw?raw:L"";CoTaskMemFree(raw);
          self->pageEvent(ok?L"PageFinished":L"PageErrorReceived",uri);
          return S_OK;
        });
      web->add_NavigationCompleted(done,&completed_token);done->Release();
    }
    Show(visible);
    if(pageEvent){ApplyBackground();Load();} // The game can wait for PageFinished before showing its dialog.
  }
  // Open the pages once the launcher says where they are; until then, say so and look again.
  void Load(){
    if(loaded||!web)return;
    if(pageEvent){
      if(!address.empty())loaded=SUCCEEDED(web->Navigate(address.c_str()));
      return;
    }
    if(!visible)return;
#ifdef REINCOMP_BUNKER_MODULES
    if(!launcherHome.empty() && dashboardPath.empty()){
      loaded=SUCCEEDED(web->Navigate((launcherHome+L"?game").c_str()));
      return;
    }
#endif
    std::ifstream file(state_file.c_str(),std::ios::binary);std::stringstream content;content<<file.rdbuf();
    const auto json=content.str();const auto url=Field(json,"url"),token=Field(json,"token"),error=Field(json,"error");
    if(!error.empty()){KillTimer(window,kTimer);web->NavigateToString(Page(Wide(error)).c_str());return;}
    if(url.empty()||token.empty()){
      if(!waiting){waiting=true;web->NavigateToString(Page(L"The Bunker is starting…").c_str());}
      SetTimer(window,kTimer,500,nullptr);return;
    }
    KillTimer(window,kTimer);
    const auto scheme=url.find("//");const auto host=url.substr(scheme==std::string::npos?0:scheme+2);
    const auto domain=Wide(host.substr(0,host.find(':')));
    ICoreWebView2_2 *web2=nullptr;ICoreWebView2CookieManager *cookies=nullptr;
    if(SUCCEEDED(web->QueryInterface(kWebView2,reinterpret_cast<void**>(&web2)))&&web2&&SUCCEEDED(web2->get_CookieManager(&cookies))&&cookies){
      const struct {const wchar_t *name;std::wstring value;BOOL http_only;} wanted[]={
        {L"lunar_tools",Wide(token),TRUE},
#if REINCOMP_VIEW_REVISION >= 2
        {L"lunar_theme",Wide(Field(json,"theme")),FALSE}};
#else
        {L"lunar_theme",L"dark",FALSE}};
#endif
      for(const auto &one:wanted){
        ICoreWebView2Cookie *cookie=nullptr;
        if(SUCCEEDED(cookies->CreateCookie(one.name,one.value.c_str(),domain.c_str(),L"/",&cookie))&&cookie){
          cookie->put_IsHttpOnly(one.http_only);cookie->put_SameSite(COREWEBVIEW2_COOKIE_SAME_SITE_KIND_STRICT);
          cookies->AddOrUpdateCookie(cookie);cookie->Release();
        }
      }
    }
    Drop(cookies);Drop(web2);
#if REINCOMP_VIEW_REVISION >= 2 || defined(REINCOMP_BUNKER_MODULES)
    loaded=SUCCEEDED(web->Navigate((Wide(url)+dashboardPath).c_str()));
#else
    loaded=SUCCEEDED(web->Navigate(Wide(url+"/").c_str()));
#endif
    std::fprintf(stderr,"Bunker view: pages %s\n",loaded?"opened":"could not be opened");
  }
  void Close(){
    if(closed)return;
    if(touching)ControllerInput(bunker::web_controller::Cancel,{});
    UpdateArea(nullptr);
    closed=true;visible=false;if(window&&!pageEvent)KillTimer(window,kTimer);
    if(web&&pageEvent){web->remove_NavigationCompleted(completed_token);web->remove_WebResourceRequested(request_token);}
    if(web)web->remove_NavigationStarting(navigation_token);
    if(controller){controller->remove_AcceleratorKeyPressed(key_token);controller->Close();}
    Drop(web);Drop(controller);
  }
};

BunkerView::BunkerView(const std::string &loader,const std::string &state_file,const std::string &user_data):
#if REINCOMP_VIEW_REVISION >= 2
  BunkerView(Wide(loader,CP_ACP),Wide(state_file,CP_ACP),Wide(user_data,CP_ACP)){}
BunkerView::BunkerView(const std::wstring &loader,const std::wstring &state_file,const std::wstring &user_data):impl_(std::make_shared<Impl>()){
  impl_->loader=loader;impl_->state_file=state_file;impl_->user_data=user_data;
#else
  impl_(std::make_shared<Impl>()){
  impl_->loader=Wide(loader,CP_ACP);impl_->state_file=Wide(state_file,CP_ACP);impl_->user_data=Wide(user_data,CP_ACP);
#endif
#ifdef REINCOMP_BUNKER_MODULES
  if(const auto *url=_wgetenv(L"REINCOMP_LAUNCHER_URL");url && *url){
    impl_->launcherHome=url;
    impl_->dashboardPath.clear();
  }
#endif
}
BunkerView::BunkerView(const std::string &loader,const std::string &user_data,void *parent,PageEvent notify):
#if REINCOMP_VIEW_REVISION >= 1
  BunkerView(Wide(loader,CP_ACP),Wide(user_data,CP_ACP),parent,std::move(notify)){}
BunkerView::BunkerView(const std::wstring &loader,const std::wstring &user_data,void *parent,PageEvent notify):impl_(std::make_shared<Impl>()){
  impl_->loader=loader;impl_->user_data=user_data;
#else
  impl_(std::make_shared<Impl>()){
  impl_->loader=Wide(loader,CP_ACP);impl_->user_data=Wide(user_data,CP_ACP);
#endif
  impl_->window=static_cast<HWND>(parent);impl_->pageEvent=std::move(notify);
}
BunkerView::~BunkerView(){impl_->Close();}
#if REINCOMP_VIEW_REVISION >= 2
void BunkerView::OpenDashboard(void *parent,const std::wstring &path){
  if(path.empty()||path.front()!=L'/'||path.find(L"//")!=path.npos||path.find_first_not_of(L"/abcdefghijklmnopqrstuvwxyz")!=path.npos)return;
  auto &p=*impl_;p.window=static_cast<HWND>(parent);p.dashboardPath=path;p.loaded=false;p.waiting=false;
  if(!p.controller&&!p.creating&&!p.failed)p.Create();else p.Show(true);
}
#endif
void BunkerView::PageFrame(int x,int y,int w,int h,int vw,int vh){
  auto &p=*impl_;p.x=x;p.y=y;p.width=w;p.height=h;p.viewportWidth=(std::max)(1,vw);p.viewportHeight=(std::max)(1,vh);p.Fit();
}
void BunkerView::Navigate(const std::wstring &url){
  auto &p=*impl_;
  auto slash=url.find(L'/',7);
  if(slash==url.npos)return;
  auto authority=url.substr(0,slash);
  const std::wstring local=L"http://127.0.0.1:";
  if(authority.compare(0,local.size(),local)!=0 || authority.size()<=local.size() ||
     authority.find_first_not_of(L"0123456789",local.size())!=authority.npos)return;
  p.origin=authority+L"/";p.address=url;p.loaded=false;
  if(!p.controller&&!p.creating&&!p.failed)p.Create();else p.Load();
}
void BunkerView::Visible(bool on){impl_->Show(on);}
void BunkerView::OpenExternalLinks(bool enabled){impl_->externalLinks=enabled;}
void BunkerView::ClearCache(){
  ICoreWebView2_13 *v13=nullptr;
  if(!impl_->web||FAILED(impl_->web->QueryInterface(IID_ICoreWebView2_13,reinterpret_cast<void **>(&v13))))return;
  ICoreWebView2Profile *profile=nullptr;
  if(SUCCEEDED(v13->get_Profile(&profile))&&profile){
    ICoreWebView2Profile2 *p2=nullptr;
    if(SUCCEEDED(profile->QueryInterface(IID_ICoreWebView2Profile2,reinterpret_cast<void **>(&p2)))){
      auto *done=new Handler<ICoreWebView2ClearBrowsingDataCompletedHandler,HRESULT>(
        IID_ICoreWebView2ClearBrowsingDataCompletedHandler,[](HRESULT){return S_OK;});
      p2->ClearBrowsingData(COREWEBVIEW2_BROWSING_DATA_KINDS_DISK_CACHE,done);done->Release();p2->Release();
    }
    profile->Release();
  }
  v13->Release();
}
void BunkerView::SetHeader(const std::wstring &name,const std::wstring &value){
  if(value.empty())impl_->headers.erase(name);else impl_->headers[name]=value;
}
void BunkerView::Background(int red,int green,int blue,int alpha){
  impl_->background={static_cast<BYTE>(alpha?255:0),static_cast<BYTE>(red),static_cast<BYTE>(green),static_cast<BYTE>(blue)};
  impl_->ApplyBackground();
}
bool BunkerView::IsVisible()const{return impl_->visible;}
bool BunkerView::HasControllerArea(){return areaCount.load(std::memory_order_acquire)!=0;}
uint32_t BunkerView::ControllerTarget(void *parent,long x,long y,uint32_t *dashboard){
  *dashboard=0;
  std::lock_guard lock(areaMutex);
  uint32_t result=0;
  for(auto it=areas.rbegin();it!=areas.rend();++it){
    if(it->window!=parent)continue;
    const bool inside=PtInRect(&it->bounds,{x,y});
    if(it->dashboard){*dashboard=it->id;return inside?it->id:0;}
    if(!result&&inside)result=it->id;
  }
  return result;
}
bool BunkerView::History(bool forward,bool move){
  auto *web=impl_->web;if(!web)return false;
  BOOL available=FALSE;if(forward)web->get_CanGoForward(&available);else web->get_CanGoBack(&available);
  if(move&&available){if(forward)web->GoForward();else web->GoBack();}
  return available!=FALSE;
}
void BunkerView::Reload(){if(impl_->web)impl_->web->Reload();}
void BunkerView::Stop(){if(impl_->web)impl_->web->Stop();}
bool BunkerView::Message(void *hwnd,uint32_t message,uint64_t wparam,int64_t lparam){
  auto &p=*impl_;
  if(message==WM_ACTIVATE && LOWORD(wparam)==WA_INACTIVE && p.touching)p.ControllerInput(web_controller::Cancel,{});
  if(message>=web_controller::Press && message<=web_controller::Back){
    if(static_cast<uint32_t>(wparam)!=p.areaId)return false;
    p.ControllerInput(message,{static_cast<short>(LOWORD(lparam)),static_cast<short>(HIWORD(lparam))},
                      static_cast<int16_t>(wparam>>32));
    return true;
  }
  if(!p.window)p.window=static_cast<HWND>(hwnd);
  if(p.pageEvent){if(message==WM_SIZE)p.Fit();if(message==WM_DESTROY)p.Close();return false;}
  switch(message){
    case web_controller::ToggleLauncher:p.Toggle();return true;
    case WM_APP+0x36e:if(p.visible)p.Show(false);return true;
    case WM_KEYDOWN:
      if(wparam=='B'&&GetKeyState(VK_CONTROL)<0){
        if(!(static_cast<uintptr_t>(lparam)&(uintptr_t{1}<<30)))p.Toggle();  // first press only
        return true;
      }
      return false;
    case WM_KEYUP:return wparam=='B'&&GetKeyState(VK_CONTROL)<0;
    case WM_TIMER:if(wparam==kTimer){p.Load();return true;}return false;
    case WM_SIZE:p.Fit();return false;
    case WM_SETFOCUS:if(p.visible&&p.controller)p.controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);return false;
    case WM_DESTROY:p.Close();return false;
    default:return false;
  }
}
}  // namespace bunker::windows
