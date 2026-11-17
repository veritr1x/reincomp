#pragma once
// Diagnostic entry probes: BUNKER_PROBE_FILE names a text file of
// "ADDRESS NAME" lines (guest addresses, hex). Each listed guest function logs
// its entry (x0..x2, LR) and then runs normally. Register before
// recomp::PublishGuestOverrides().
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "../runtime/runtime.h"

namespace bunker::windows::guest_probes {

inline void AddFromEnvironment(){
  const char *path=std::getenv("BUNKER_PROBE_FILE");
  if(!path||!*path)return;
  std::ifstream in(path);
  if(!in){std::fprintf(stderr,"Guest probes: cannot read %s\n",path);return;}
  std::string line;int count=0;
  while(std::getline(in,line)){
    std::istringstream row(line);std::string address,name;
    if(!(row>>address))continue;std::getline(row>>std::ws,name);
    const addr_t entry=std::strtoull(address.c_str(),nullptr,16);if(!entry)continue;
    // Optional trailing "@sN": also print xN as an il2cpp System.String
    // (int32 length at +0x10, UTF-16 at +0x14; ASCII shown, others as '?').
    // Optional trailing "@eN": xN points at an Il2CppExceptionWrapper (as in
    // __cxa_throw); print the wrapped managed exception's class, _message and
    // inner exception (Il2CppClass name/namespace at +0x10/+0x18).
    int string_register=-1;bool exception_mode=false;
    if(name.size()>3&&name[name.size()-3]=='@'&&(name[name.size()-2]=='s'||name[name.size()-2]=='e')&&name.back()>='0'&&name.back()<='7'){exception_mode=name[name.size()-2]=='e';string_register=name.back()-'0';name.resize(name.size()-3);}
    auto hits=std::make_shared<std::atomic<uint64_t>>(0);
    recomp::AddGuestProbe(entry,[entry,name,hits,string_register,exception_mode](State &s){
      const auto n=hits->fetch_add(1);
      if(!exception_mode&&name.find("@a")==std::string::npos&&!(n<16||(n&(n-1))==0))return;
      if(exception_mode&&!(n<256||(n&(n-1))==0))return;
      std::string text;
      auto read_string=[](addr_t object){std::string out;if(!object)return std::string("null");int32_t length=0;std::memcpy(&length,reinterpret_cast<const void*>(object+0x10),4);
        out="\"";for(int32_t i=0;i<length&&i<300;++i){uint16_t c;std::memcpy(&c,reinterpret_cast<const void*>(object+0x14+2*i),2);out+=c>=0x20&&c<0x7f?char(c):'?';}out+=length>300?"...\"":"\"";return out;};
      auto read_cstring=[](addr_t p){std::string out;if(!p)return out;for(int i=0;i<128;++i){char c;std::memcpy(&c,reinterpret_cast<const void*>(p+i),1);if(!c)break;out+=c;}return out;};
      auto describe=[&](addr_t object){if(!object)return std::string("null");addr_t klass;std::memcpy(&klass,reinterpret_cast<const void*>(object),8);std::string out="?";
        if(klass){addr_t nm,ns;std::memcpy(&nm,reinterpret_cast<const void*>(klass+0x10),8);std::memcpy(&ns,reinterpret_cast<const void*>(klass+0x18),8);out=read_cstring(ns)+"."+read_cstring(nm);}
        addr_t message;std::memcpy(&message,reinterpret_cast<const void*>(object+0x18),8);return out+" message="+read_string(message);};
      if(exception_mode){
        addr_t wrapper;std::memcpy(&wrapper,reinterpret_cast<const uint8_t*>(&s.gpr)+string_register*16+8,8);
        addr_t object=0;if(wrapper)std::memcpy(&object,reinterpret_cast<const void*>(wrapper),8);
        text=" exception="+describe(object);
        if(object){addr_t inner;std::memcpy(&inner,reinterpret_cast<const void*>(object+0x28),8);if(inner)text+=" inner="+describe(inner);}
      }else if(string_register>=0){
        addr_t object;std::memcpy(&object,reinterpret_cast<const uint8_t*>(&s.gpr)+string_register*16+8,8);
        if(!object)text=" str=null";
        else{int32_t length=0;std::memcpy(&length,reinterpret_cast<const void*>(object+0x10),4);
          text=" str=\"";for(int32_t i=0;i<length&&i<200;++i){uint16_t c;std::memcpy(&c,reinterpret_cast<const void*>(object+0x14+2*i),2);text+=c>=0x20&&c<0x7f?char(c):'?';}
          text+=length>200?"...\"":"\"";}
      }
      float s0=0,s1=0;std::memcpy(&s0,&s.simd.v[0],4);std::memcpy(&s1,&s.simd.v[1],4);
      // Optional "@mN" suffix: dump 96 bytes at xN and 96 at the pointer it holds first.
      // Optional "@aN": xN is a managed float[] (length +0x18, data +0x20);
      // print every 16th element when the length is exactly 128.
      const auto at_a=name.rfind("@a");
      if(at_a!=std::string::npos&&at_a+2<name.size()){
        const int reg=std::atoi(name.c_str()+at_a+2)&31;addr_t array;std::memcpy(&array,reinterpret_cast<const uint8_t*>(&s.gpr)+reg*16+8,8);
        if(array<0x10000||array>0x7fffffffffffULL||(array&7))return;
        int32_t length;std::memcpy(&length,reinterpret_cast<const void*>(array+0x18),4);if(length!=128)return;
        static std::atomic<unsigned> shown{0};if(shown.fetch_add(1)>=12)return;
        for(int k=0;k<128;k+=16){float f;std::memcpy(&f,reinterpret_cast<const void*>(array+0x20+4*k),4);char b[32];std::snprintf(b,sizeof b," [%d]=%g",k,double(f));text+=b;}
        std::fprintf(stderr,"Probe: %s array=%#llx LR=%#llx%s\n",name.c_str(),(unsigned long long)array,(unsigned long long)s.gpr.x30.qword,text.c_str());return;
      }
      // Optional "@cN": xN is a managed Color[] (length +0x18, 16-byte elements
      // at +0x20); print elements 0, 64, 127, 128, 192, 255 when present.
      const auto at_c=name.rfind("@c");
      if(at_c!=std::string::npos&&at_c+2<name.size()){
        const int reg=std::atoi(name.c_str()+at_c+2)&31;addr_t array;std::memcpy(&array,reinterpret_cast<const uint8_t*>(&s.gpr)+reg*16+8,8);
        if(n>=8||array<0x10000||array>0x7fffffffffffULL||(array&7))return;
        int32_t length;std::memcpy(&length,reinterpret_cast<const void*>(array+0x18),4);
        char b[64];std::snprintf(b,sizeof b," length=%d",length);text+=b;
        for(int k:{0,64,127,128,192,255}){if(k>=length)break;float f[4];std::memcpy(f,reinterpret_cast<const void*>(array+0x20+16*k),16);
          std::snprintf(b,sizeof b," [%d]=(%g,%g,%g,%g)",k,double(f[0]),double(f[1]),double(f[2]),double(f[3]));text+=b;}
        std::fprintf(stderr,"Probe: %s array=%#llx LR=%#llx%s\n",name.c_str(),(unsigned long long)array,(unsigned long long)s.gpr.x30.qword,text.c_str());return;
      }
      const auto at_m=name.rfind("@m");
      if(at_m!=std::string::npos&&at_m+2<name.size()&&n<4){
        const int reg=std::atoi(name.c_str()+at_m+2)&31;
        addr_t base;std::memcpy(&base,reinterpret_cast<const uint8_t*>(&s.gpr)+reg*16+8,8);
        auto dump=[&](addr_t p,const char*label){text+=std::string(" ")+label+"=";if(p<0x10000||p>0x7fffffffffffULL||(p&3)){text+="unreadable";return;}
          for(int k=0;k<24;++k){uint32_t w;std::memcpy(&w,reinterpret_cast<const void*>(p+4*k),4);float f;std::memcpy(&f,&w,4);char b[40];std::snprintf(b,sizeof b,"%s%08x(%g)",k?" ":"",w,double(f));text+=b;}};
        dump(base,"mem");addr_t first=0;if(base>=0x10000&&base<0x7fffffffffffULL)std::memcpy(&first,reinterpret_cast<const void*>(base),8);dump(first,"deref");
      }
      std::fprintf(stderr,"Probe: %s (%#llx) hit=%llu x0=%#llx x1=%#llx x2=%#llx s0=%g s1=%g LR=%#llx%s\n",name.c_str(),(unsigned long long)entry,(unsigned long long)n+1,
        (unsigned long long)s.gpr.x0.qword,(unsigned long long)s.gpr.x1.qword,(unsigned long long)s.gpr.x2.qword,double(s0),double(s1),(unsigned long long)s.gpr.x30.qword,text.c_str());
    });
    ++count;
  }
  std::fprintf(stderr,"Guest probes: %d entries from %s\n",count,path);
}

}  // namespace bunker::windows::guest_probes
