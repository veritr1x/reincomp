#pragma once
// jp.qualiarts.octo.lib.Http from the APK (classes2.dex), used by Octo's
// AndroidFileDownloadRequest to download asset bundles. The JNI host does not
// run Java, so this host class does the same work natively:
// downloadFile(url, path[, overwrite], callback) starts a background HTTP GET
// that streams the body to `path` while computing its MD5. The C# side
// (AndroidFileDownloadRequest.<Execute>d__10) polls the HttpResult fields
// (isComplete, statusCode, contentLength, writtenLength, md5, errorCode,
// errorMessage, isIoError) and calls isError(). It passes a null
// IHttpCallback, so no callback is delivered. Plain http:// only (the
// Octo asset server is the local Lunar Tear server). Portable: POSIX sockets
// on Linux, Winsock on Windows.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <cctype>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "jni_host.h"

namespace bunker::windows::octo_http {

namespace detail {

// RFC 1321 MD5.
class Md5 {
 public:
  void Update(const uint8_t *data,size_t size){
    while(size){
      const size_t take=std::min(size,size_t(64)-used_);
      std::memcpy(block_+used_,data,take);used_+=take;data+=take;size-=take;length_+=take;
      if(used_==64){Transform(block_);used_=0;}
    }
  }
  std::string HexDigest(){
    const uint64_t bits=length_*8;const uint8_t pad=0x80,zero=0;
    Update(&pad,1);while(used_!=56)Update(&zero,1);
    uint8_t tail[8];for(int i=0;i<8;++i)tail[i]=uint8_t(bits>>(8*i));Update(tail,8);
    static const char hex[]="0123456789abcdef";std::string out;
    for(uint32_t word:state_)for(int i=0;i<4;++i){const uint8_t b=uint8_t(word>>(8*i));out+=hex[b>>4];out+=hex[b&15];}
    return out;
  }
 private:
  static uint32_t Rotl(uint32_t x,int c){return (x<<c)|(x>>(32-c));}
  void Transform(const uint8_t *p){
    static const uint32_t k[64]={
      0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
      0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
      0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
      0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
      0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
      0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
      0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
      0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    static const int r[64]={7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                            4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
    uint32_t m[16];for(int i=0;i<16;++i)m[i]=uint32_t(p[i*4])|uint32_t(p[i*4+1])<<8|uint32_t(p[i*4+2])<<16|uint32_t(p[i*4+3])<<24;
    uint32_t a=state_[0],b=state_[1],c=state_[2],d=state_[3];
    for(int i=0;i<64;++i){
      uint32_t f;int g;
      if(i<16){f=(b&c)|(~b&d);g=i;}else if(i<32){f=(d&b)|(~d&c);g=(5*i+1)%16;}
      else if(i<48){f=b^c^d;g=(3*i+5)%16;}else{f=c^(b|~d);g=(7*i)%16;}
      const uint32_t t=d;d=c;c=b;b=b+Rotl(a+f+k[i]+m[g],r[i]);a=t;
    }
    state_[0]+=a;state_[1]+=b;state_[2]+=c;state_[3]+=d;
  }
  uint32_t state_[4]={0x67452301,0xefcdab89,0x98badcfe,0x10325476};
  uint8_t block_[64]{};size_t used_=0;uint64_t length_=0;
};

#ifdef _WIN32
using Socket=SOCKET;
constexpr Socket kInvalidSocket=INVALID_SOCKET;
inline void CloseSocket(Socket s){closesocket(s);}
inline bool StartSockets(){static const bool ok=[]{WSADATA data;return WSAStartup(MAKEWORD(2,2),&data)==0;}();return ok;}
#else
using Socket=int;
constexpr Socket kInvalidSocket=-1;
inline void CloseSocket(Socket s){::close(s);}
inline bool StartSockets(){return true;}
#endif

}  // namespace detail

// One download; shared by the worker thread and the Java HttpResult object.
struct Download {
  std::mutex mutex;
  bool complete=false,io_error=false;
  int32_t status=0,content_length=-1,written=0;
  std::string md5;
  std::optional<std::string> error_code,error_message;
  std::atomic<bool> abort{false};
};

class Service {
 public:
  // Maps an Android path to the host file; nullopt if it is not writable here.
  using PathResolver=std::function<std::optional<std::filesystem::path>(const std::string&)>;

  Service(jni::Host &java,PathResolver resolve):java_(java),resolve_(std::move(resolve)){Define();}
  ~Service(){
    std::unique_lock lock(mutex_);
    for(auto &weak:downloads_)if(auto d=weak.lock())d->abort=true;
    idle_.wait(lock,[this]{return active_==0;});
  }
  Service(const Service&)=delete;
  Service &operator=(const Service&)=delete;

 private:
  static constexpr const char *kHttp="jp/qualiarts/octo/lib/Http";
  static constexpr const char *kTask="jp/qualiarts/octo/lib/HttpAsyncTask";
  static constexpr const char *kResult="jp/qualiarts/octo/lib/HttpResult";
  static constexpr const char *kCallback="jp/qualiarts/octo/lib/IHttpCallback";

  struct Task {std::shared_ptr<Download> download;jni::Host::ObjectValue result;};

  static std::u16string Utf16(const std::string &s){return std::u16string(s.begin(),s.end());}
  static std::string Ascii(const std::u16string &s){std::string out;for(char16_t c:s)out+=c<0x80?char(c):'?';return out;}

  void Define(){
    using Args=jni::Host::MethodArguments;using Value=jni::Host::ComputedFieldValue;
    java_.DefineInterface(kCallback,{{"onComplete","(Ljp/qualiarts/octo/lib/HttpAsyncTask;Ljp/qualiarts/octo/lib/HttpResult;)V"},{"onProgress","(II)V"}},false);
    java_.DefineClass(kHttp);
    java_.DefineClass(kTask);
    java_.DefineClass(kResult);
    for(const char *name:{kHttp,kTask,kResult,kCallback})java_.MarkClassInitialized(name);
    auto start=[this](const Args &a,int callback_index)->addr_t{
      auto url=java_.StringValue(a[0]),path=java_.StringValue(a[1]);
      if(!url||!path){java_.ThrowException("java/lang/NullPointerException",u"Http.downloadFile(null)");return 0;}
      if(a[callback_index]&&!warned_callback_.exchange(true))std::fprintf(stderr,"Octo Http: IHttpCallback is not delivered; the caller polls the result\n");
      return Start(Ascii(*url),Ascii(*path));
    };
    java_.DefineStaticObjectMethod(kHttp,"downloadFile","(Ljava/lang/String;Ljava/lang/String;Ljp/qualiarts/octo/lib/IHttpCallback;)Ljp/qualiarts/octo/lib/HttpAsyncTask;",
                                   [start](addr_t,const Args &a){return start(a,2);});
    java_.DefineStaticObjectMethod(kHttp,"downloadFile","(Ljava/lang/String;Ljava/lang/String;ZLjp/qualiarts/octo/lib/IHttpCallback;)Ljp/qualiarts/octo/lib/HttpAsyncTask;",
                                   [start](addr_t,const Args &a){return start(a,3);});
    java_.DefineObjectMethod(kTask,"getResult","()Ljp/qualiarts/octo/lib/HttpResult;",[this](addr_t self,const Args&)->addr_t{
      auto task=java_.ReadHostState<Task>(self);return task?java_.LocalValue(task->result):0;});
    java_.DefineVoidMethod(kTask,"abort","()V",[this](addr_t self,const Args&){
      if(auto task=java_.ReadHostState<Task>(self))task->download->abort=true;});
    auto field=[this](const char *name,const char *signature,std::function<Value(Download&)> read){
      java_.DefineComputedField<Download>(kResult,name,signature,[read](Download &d){std::lock_guard lock(d.mutex);return read(d);});};
    field("isComplete","Z",[](Download &d){return Value{d.complete};});
    field("isIoError","Z",[](Download &d){return Value{d.io_error};});
    field("statusCode","I",[](Download &d){return Value{uint32_t(d.status)};});
    field("contentLength","I",[](Download &d){return Value{uint32_t(d.content_length)};});
    field("writtenLength","I",[](Download &d){return Value{uint32_t(d.written)};});
    field("md5","Ljava/lang/String;",[](Download &d){Value v;if(d.complete&&!d.md5.empty())v.text=Utf16(d.md5);return v;});
    field("errorCode","Ljava/lang/String;",[](Download &d){Value v;if(d.error_code)v.text=Utf16(*d.error_code);return v;});
    field("errorMessage","Ljava/lang/String;",[](Download &d){Value v;if(d.error_message)v.text=Utf16(*d.error_message);return v;});
    java_.DefineBooleanMethod(kResult,"isError","()Z",[this](addr_t self,const Args&){
      auto d=java_.ReadHostState<Download>(self);if(!d)return false;
      std::lock_guard lock(d->mutex);return d->io_error||d->error_code.has_value();});
  }

  addr_t Start(const std::string &url,const std::string &path){
    auto download=std::make_shared<Download>();
    auto result=java_.NewHostInstance(kResult,download);if(!result)return 0;
    auto task=std::make_shared<Task>(Task{download,java_.CaptureValue(result)});
    java_.DeleteLocalReference(result);
    auto object=java_.NewHostInstance(kTask,task);if(!object)return 0;
    const auto host_path=resolve_(path);
    {std::lock_guard lock(mutex_);++active_;
     downloads_.erase(std::remove_if(downloads_.begin(),downloads_.end(),[](const auto &w){return w.expired();}),downloads_.end());
     downloads_.push_back(download);}
    const uint32_t n=started_.fetch_add(1)+1;
    if(n<=8||n%100==0)std::fprintf(stderr,"Octo Http: download #%u %s -> %s\n",n,url.c_str(),path.c_str());
    std::thread([this,download,url,path,host_path]{
      Run(*download,url,path,host_path);
      std::lock_guard lock(mutex_);if(--active_==0)idle_.notify_all();
    }).detach();
    return object;
  }

  static void Fail(Download &d,bool io,const std::string &code,const std::string &message){
    std::lock_guard lock(d.mutex);d.io_error=io;if(!io)d.error_code=code;d.error_message=message;d.complete=true;
  }

 public:
  // Shared with the native Linux player, which has no Android Java objects.
  static void Run(Download &d,std::string url,const std::string &path,const std::optional<std::filesystem::path> &host_path){
    if(!host_path){Fail(d,true,"","Octo Http: path is outside the writable storage: "+path);Report(d,url);return;}
    for(int redirect=0;redirect<5;++redirect){
      std::string location;
      if(!Fetch(d,url,*host_path,location))break;
      if(location.empty())break;
      url=location;
    }
    Report(d,url);
  }

 private:
  static void Report(Download &d,const std::string &url){
    std::lock_guard lock(d.mutex);
    if(d.io_error||d.error_code||(d.status&&d.status!=200))
      std::fprintf(stderr,"Octo Http: %s failed status=%d io=%d %s\n",url.c_str(),d.status,int(d.io_error),d.error_message.value_or("").c_str());
  }

  // One request. Returns true with `location` set to follow a redirect.
  static bool Fetch(Download &d,const std::string &url,const std::filesystem::path &host_path,std::string &location){
    static const std::string scheme="http://";
    if(url.compare(0,scheme.size(),scheme)){Fail(d,true,"","unsupported URL scheme: "+url);return false;}
    const auto slash=url.find('/',scheme.size());
    const std::string authority=url.substr(scheme.size(),slash==std::string::npos?std::string::npos:slash-scheme.size());
    const std::string target=slash==std::string::npos?"/":url.substr(slash);
    std::string host=authority,port="80";
    if(auto colon=authority.rfind(':');colon!=std::string::npos){host=authority.substr(0,colon);port=authority.substr(colon+1);}
    if(!detail::StartSockets()){Fail(d,true,"","socket startup failed");return false;}
    addrinfo hints{};hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_STREAM;addrinfo *found=nullptr;
    if(getaddrinfo(host.c_str(),port.c_str(),&hints,&found)||!found){Fail(d,true,"","cannot resolve "+host);return false;}
    detail::Socket s=detail::kInvalidSocket;
    for(auto *ai=found;ai;ai=ai->ai_next){
      s=socket(ai->ai_family,ai->ai_socktype,ai->ai_protocol);if(s==detail::kInvalidSocket)continue;
      if(connect(s,ai->ai_addr,int(ai->ai_addrlen))==0)break;
      detail::CloseSocket(s);s=detail::kInvalidSocket;
    }
    freeaddrinfo(found);
    if(s==detail::kInvalidSocket){Fail(d,true,"","cannot connect to "+authority);return false;}
    struct Closer{detail::Socket s;~Closer(){detail::CloseSocket(s);}} closer{s};
    const std::string request="GET "+target+" HTTP/1.1\r\nHost: "+authority+"\r\nUser-Agent: octo-http\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n";
    for(size_t sent=0;sent<request.size();){
      const int n=send(s,request.data()+sent,int(request.size()-sent),0);
      if(n<=0){Fail(d,true,"","send failed");return false;}sent+=size_t(n);
    }
    std::string buffer;char chunk[64*1024];
    auto receive=[&]()->bool{
      if(d.abort){Fail(d,true,"","aborted");return false;}
      const int n=recv(s,chunk,int(sizeof chunk),0);if(n<=0)return false;buffer.append(chunk,size_t(n));return true;};
    size_t header_end;
    while((header_end=buffer.find("\r\n\r\n"))==std::string::npos)
      if(!receive()){if(!d.complete)Fail(d,true,"","connection closed before headers");return false;}
    const std::string headers=buffer.substr(0,header_end);buffer.erase(0,header_end+4);
    int status=0;std::sscanf(headers.c_str(),"HTTP/%*d.%*d %d",&status);
    int64_t length=-1;bool chunked=false;
    size_t pos=headers.find("\r\n");
    while(pos!=std::string::npos&&pos<headers.size()){
      const size_t next=headers.find("\r\n",pos+2);
      std::string line=headers.substr(pos+2,next==std::string::npos?std::string::npos:next-pos-2);pos=next;
      const auto colon=line.find(':');if(colon==std::string::npos)continue;
      std::string name=line.substr(0,colon),value=line.substr(colon+1);
      for(auto &c:name)c=char(std::tolower(static_cast<unsigned char>(c)));
      while(!value.empty()&&value.front()==' ')value.erase(0,1);
      if(name=="content-length")length=std::strtoll(value.c_str(),nullptr,10);
      else if(name=="transfer-encoding"&&value.find("chunked")!=std::string::npos)chunked=true;
      else if(name=="location")location=value;
    }
    {std::lock_guard lock(d.mutex);d.status=status;d.content_length=int32_t(length);}
    if(status>=300&&status<400&&!location.empty()){
      if(location.front()=='/')location=scheme+authority+location;return true;}
    location.clear();
    if(status!=200){std::lock_guard lock(d.mutex);d.complete=true;return false;}
    std::error_code ec;std::filesystem::create_directories(host_path.parent_path(),ec);
    std::ofstream out(host_path,std::ios::binary|std::ios::trunc);
    if(!out){Fail(d,true,"","cannot open "+host_path.u8string());return false;}
    detail::Md5 md5;int64_t written=0;
    auto emit=[&](const char *data,size_t size)->bool{
      out.write(data,std::streamsize(size));if(!out){Fail(d,true,"","write failed");return false;}
      md5.Update(reinterpret_cast<const uint8_t*>(data),size);written+=int64_t(size);
      std::lock_guard lock(d.mutex);d.written=int32_t(written);return true;};
    if(!chunked){
      for(;;){
        if(length>=0&&written+int64_t(buffer.size())>=length){if(!emit(buffer.data(),size_t(length-written)))return false;break;}
        if(!buffer.empty()&&!emit(buffer.data(),buffer.size()))return false;
        buffer.clear();
        if(!receive()){if(d.complete)return false;if(length>=0){Fail(d,true,"","connection closed after "+std::to_string(written)+" of "+std::to_string(length)+" bytes");return false;}break;}
      }
    }else{
      for(;;){
        size_t eol;
        while((eol=buffer.find("\r\n"))==std::string::npos)if(!receive()){if(!d.complete)Fail(d,true,"","truncated chunk header");return false;}
        const uint64_t size=std::strtoull(buffer.substr(0,eol).c_str(),nullptr,16);buffer.erase(0,eol+2);
        if(!size)break;
        while(buffer.size()<size+2)if(!receive()){if(!d.complete)Fail(d,true,"","truncated chunk");return false;}
        if(!emit(buffer.data(),size_t(size)))return false;buffer.erase(0,size_t(size)+2);
      }
    }
    out.close();if(!out){Fail(d,true,"","close failed");return false;}
    std::lock_guard lock(d.mutex);d.md5=md5.HexDigest();d.content_length=int32_t(length>=0?length:written);d.complete=true;
    return false;
  }

  jni::Host &java_;
  PathResolver resolve_;
  std::mutex mutex_;
  std::condition_variable idle_;
  size_t active_=0;
  std::vector<std::weak_ptr<Download>> downloads_;
  std::atomic<uint32_t> started_{0};
  std::atomic<bool> warned_callback_{false};
};

}  // namespace bunker::windows::octo_http
