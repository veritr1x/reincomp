#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include "bunker_view.h"
#include "../third_party/webview2/WebView2.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>

namespace bunker::windows {
namespace {
constexpr UINT_PTR kTimer=0xB0B;
// The header declares these without defining them.
constexpr GUID kEnvironmentHandler{0x4e8a3389,0xc9d8,0x4bd2,{0xb6,0xb5,0x12,0x4f,0xee,0x6c,0xc1,0x4d}};
constexpr GUID kControllerHandler{0x6c4819f3,0xc9b7,0x4260,{0x81,0x27,0xc9,0xf5,0xbd,0xe7,0xf6,0x8c}};
constexpr GUID kKeyHandler{0xb29c7e28,0xfa79,0x41a8,{0x8e,0x44,0x65,0x81,0x1c,0x76,0xdc,0xb2}};
constexpr GUID kNavigationHandler{0x9adbe429,0xf36d,0x432b,{0x9d,0xdc,0xf8,0x88,0x1f,0xbd,0x76,0xe3}};
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

  void Fit(){
    if(!controller||!window)return;
    RECT client{};GetClientRect(window,&client);controller->put_Bounds(client);
  }
  void Show(bool on){
    visible=on;
    if(controller){
      controller->put_IsVisible(on?TRUE:FALSE);
      if(on){Fit();controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);}
    }
    if(on)Load();else{KillTimer(window,kTimer);SetFocus(window);}
    std::fprintf(stderr,"Bunker view: %s\n",on?"shown":"hidden");
  }
  void Toggle(){
    if(failed||closed)return;
    if(!controller&&!creating){Create();return;}
    if(controller)Show(!visible);else visible=!visible;  // still being created: applied when it is ready
  }
  void Fail(const char *what,HRESULT result){
    failed=true;creating=false;visible=false;
    std::fprintf(stderr,"Bunker view: %s failed (0x%08lx). The Microsoft Edge WebView2 Runtime may be missing.\n",what,static_cast<unsigned long>(result));
  }
  void Create(){
    using CreateEnvironment=HRESULT(STDAPICALLTYPE*)(PCWSTR,PCWSTR,ICoreWebView2EnvironmentOptions*,ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(com)&&com!=RPC_E_CHANGED_MODE){Fail("COM",com);return;}
    auto module=LoadLibraryW(loader.c_str());
    FARPROC address=module?GetProcAddress(module,"CreateCoreWebView2EnvironmentWithOptions"):nullptr;
    if(!address){Fail("loading WebView2Loader.dll",HRESULT_FROM_WIN32(GetLastError()));return;}
    CreateEnvironment create=nullptr;static_assert(sizeof(create)==sizeof(address));std::memcpy(&create,&address,sizeof(create));
    creating=true;visible=true;
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
        if(key=='B'&&GetAsyncKeyState(VK_CONTROL)<0){
          arguments->put_Handled(TRUE);
          COREWEBVIEW2_PHYSICAL_KEY_STATUS status{};arguments->get_PhysicalKeyStatus(&status);
          if(kind==COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN&&!status.WasKeyDown)self->Show(false);
        }
        return S_OK;
      });
    controller->add_AcceleratorKeyPressed(on_key,&key_token);on_key->Release();
    // The pages' own Close is a link the app around them takes.
    auto *on_navigation=new Handler<ICoreWebView2NavigationStartingEventHandler,ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs*>(
      kNavigationHandler,[weak](ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs *arguments)->HRESULT{
        auto self=weak.lock();if(!self||!arguments)return S_OK;
        LPWSTR address=nullptr;
        if(SUCCEEDED(arguments->get_Uri(&address))&&address){
          const std::wstring uri(address);CoTaskMemFree(address);
          const std::wstring close=L"/bunker/close";
          if(uri.size()>=close.size()&&!uri.compare(uri.size()-close.size(),close.size(),close)){arguments->put_Cancel(TRUE);self->Show(false);}
        }
        return S_OK;
      });
    web->add_NavigationStarting(on_navigation,&navigation_token);on_navigation->Release();
    Show(visible);
  }
  // Open the pages once the launcher says where they are; until then, say so and look again.
  void Load(){
    if(loaded||!web||!visible)return;
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
        {L"lunar_tools",Wide(token),TRUE},{L"lunar_theme",L"dark",FALSE}};
      for(const auto &one:wanted){
        ICoreWebView2Cookie *cookie=nullptr;
        if(SUCCEEDED(cookies->CreateCookie(one.name,one.value.c_str(),domain.c_str(),L"/",&cookie))&&cookie){
          cookie->put_IsHttpOnly(one.http_only);cookie->put_SameSite(COREWEBVIEW2_COOKIE_SAME_SITE_KIND_STRICT);
          cookies->AddOrUpdateCookie(cookie);cookie->Release();
        }
      }
    }
    Drop(cookies);Drop(web2);
    loaded=SUCCEEDED(web->Navigate(Wide(url+"/").c_str()));
    std::fprintf(stderr,"Bunker view: pages %s\n",loaded?"opened":"could not be opened");
  }
  void Close(){
    closed=true;visible=false;if(window)KillTimer(window,kTimer);
    if(web)web->remove_NavigationStarting(navigation_token);
    if(controller){controller->remove_AcceleratorKeyPressed(key_token);controller->Close();}
    Drop(web);Drop(controller);
  }
};

BunkerView::BunkerView(const std::string &loader,const std::string &state_file,const std::string &user_data):impl_(std::make_shared<Impl>()){
  impl_->loader=Wide(loader,CP_ACP);impl_->state_file=Wide(state_file,CP_ACP);impl_->user_data=Wide(user_data,CP_ACP);
}
BunkerView::~BunkerView()=default;
bool BunkerView::Message(void *hwnd,uint32_t message,uint64_t wparam,int64_t lparam){
  auto &p=*impl_;
  if(!p.window)p.window=static_cast<HWND>(hwnd);
  switch(message){
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
