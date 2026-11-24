#include "../../host/game_page.h"
#include "../../host/bunker_ports.h"
namespace webview_jni {
using bunker::jni::Host;
using bunker::windows::GamePageAction;
using Arguments=Host::MethodArguments;
static std::wstring string(Host &host,addr_t value){
    auto text=value?host.StringValue(value):std::nullopt;
    return text?std::wstring(text->begin(),text->end()):std::wstring();
}
static void post(GamePageAction action,const std::wstring &id,const std::wstring &value={},int x=0,int y=0,int w=0,int h=0,const std::wstring &extra={}){
    static auto send=reinterpret_cast<bunker::windows::PostGamePage>(GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),"EnqueueGamePage"));
    if(send)send(action,id.c_str(),value.c_str(),x,y,w,h,extra.c_str());
}
static int query(const std::wstring &id,int kind,wchar_t *text=nullptr,int capacity=0){
    static auto read=reinterpret_cast<bunker::windows::QueryGamePage>(GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),"QueryGamePage"));
    return read?read(id.c_str(),kind,text,capacity):0;
}
static void install(Host &host){
    constexpr auto owner="com/onevcat/uniwebview/UniWebViewInterface";
    const std::string str="Ljava/lang/String;";
    struct Op {const char *name;std::string args;GamePageAction action;};
    const Op ops[]={
        {"init",str+"IIII",GamePageAction::Init},{"setFrame",str+"IIII",GamePageAction::Frame},
        {"setSize",str+"II",GamePageAction::Size},{"setPosition",str+"II",GamePageAction::Position},
        {"load",str+str,GamePageAction::Load},{"destroy",str,GamePageAction::Destroy},
        {"goBack",str,GamePageAction::Back},{"goForward",str,GamePageAction::Forward},
        {"reload",str,GamePageAction::Reload},{"stop",str,GamePageAction::Stop},
        {"showWebViewDialog",str+"Z",GamePageAction::Show},
        {"setWebViewAlpha",str+"F",GamePageAction::Alpha},{"setBackgroundColor",str+"FFFF",GamePageAction::Background},
        {"setHeaderField",str+str+str,GamePageAction::Header},{"cleanCache",str,GamePageAction::ClearCache},
        {"setOpenLinksInExternalBrowser",str+"Z",GamePageAction::ExternalLinks}
    };
    const std::pair<std::string,std::string> nativeDefaults[]={
        {"prepare","()V"},{"setBouncesEnabled","("+str+"Z)V"},{"setCalloutEnabled","("+str+"Z)V"}
    };
    std::vector<bunker::jni::MethodDeclaration> methods;
    for(const auto &[name,sig]:nativeDefaults)methods.push_back({name,sig,true,false});
    for(const auto &op:ops)methods.push_back({op.name,"("+op.args+")V",true,false});
    host.DefineClass(owner,std::move(methods),"java/lang/Object",true);
    // Windows creates the WebView2 environment lazily on the window thread.
    for(const auto &[name,sig]:nativeDefaults)
        host.BindStaticVoidMethod(owner,name,sig,[](addr_t,const Arguments &){});
    for(const auto &op:ops)
        host.BindStaticVoidMethod(owner,op.name,"("+op.args+")V",[&host,action=op.action](addr_t,const Arguments &a){
            auto id=string(host,a[0]);
            if(action==GamePageAction::Load){
                auto url=string(host,a[1]);
                bunker::ports::routeAssetUrl(url,8080+bunker::ports::windowsOffset());
                post(action,id,url);
            }
            else if(action==GamePageAction::Header)post(action,id,string(host,a[1]),0,0,0,0,string(host,a[2]));
            else if(action==GamePageAction::Size)post(action,id,{},0,0,int(a[1]),int(a[2]));
            else if(action==GamePageAction::Show)post(a[1]?GamePageAction::Show:GamePageAction::Hide,id);
            else if(action==GamePageAction::Alpha || action==GamePageAction::Background){
                auto channel=[](addr_t arg){uint32_t bits=static_cast<uint32_t>(arg);float v;std::memcpy(&v,&bits,4);
                    return int(std::clamp(v,0.0f,1.0f)*255.0f+0.5f);};
                post(action,id,{},channel(a[1]),channel(a[2]),channel(a[3]),channel(a[4]));
            }
            else post(action,id,{},int(a[1]),int(a[2]),int(a[3]),int(a[4]));
        });
    for(bool show:{false,true})
        host.DefineStaticBooleanMethod(owner,show?"show":"hide","("+str+"ZIF"+str+")Z",
            [&host,show](addr_t,const Arguments &a){
                auto id=string(host,a[0]);if(id.empty())return false;
                post(show?GamePageAction::Show:GamePageAction::Hide,id,string(host,a[4]));return true;
            });
    for(bool forward:{false,true})
        host.DefineStaticBooleanMethod(owner,forward?"canGoForward":"canGoBack","("+str+")Z",
            [&host,forward](addr_t,const Arguments &a){return query(string(host,a[0]),forward?3:2)!=0;});
    for(int axis:{0,1})host.DefineStaticFloatBitsMethod(owner,axis?"screenHeight":"screenWidth","()F",
        [axis](addr_t,const Arguments &){float size=float(query({},axis));uint32_t bits;std::memcpy(&bits,&size,4);return bits;});
    host.DefineStaticObjectMethod(owner,"getUrl","("+str+")"+str,[&host](addr_t,const Arguments &a){
        auto id=string(host,a[0]);auto len=query(id,4);
        std::vector<wchar_t> buffer(size_t(len)+1);
        if(query(id,4,buffer.data(),int(buffer.size()))>len)return host.NewString(u"");
        return host.NewString(std::u16string(buffer.data(),buffer.data()+wcslen(buffer.data())));
    });
}
}
