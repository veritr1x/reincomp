#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <cstdlib>
#include <fstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace bunker::windows {
// IANA zone of the host, as Android's TimeZone.getDefault().getID() reports.
inline std::string HostTimeZoneId(){
  if(const char *tz=std::getenv("TZ");tz&&*tz){std::string id=tz[0]==':'?tz+1:tz;if(!id.empty()&&id.find('/')!=std::string::npos)return id;}
#ifdef _WIN32
  // Windows names its zones by registry key; Windows' bundled ICU maps them.
  DYNAMIC_TIME_ZONE_INFORMATION info{};
  if(GetDynamicTimeZoneInformation(&info)!=TIME_ZONE_ID_INVALID&&info.TimeZoneKeyName[0]){
    using Map=int32_t(*)(const char16_t*,int32_t,const char*,char16_t*,int32_t,int*);
    if(auto icu=LoadLibraryW(L"icu.dll")){
      if(auto map=reinterpret_cast<Map>(reinterpret_cast<void*>(GetProcAddress(icu,"ucal_getTimeZoneIDForWindowsID")))){
        char16_t id[128]{};int status=0;
        const auto length=map(reinterpret_cast<const char16_t*>(info.TimeZoneKeyName),-1,nullptr,id,127,&status);
        if(status<=0&&length>0){std::string out;for(int32_t i=0;i<length;++i)out.push_back(char(id[i]));return out;}
      }
    }
  }
#else
  std::error_code error;const auto link=std::filesystem::read_symlink("/etc/localtime",error).generic_string();
  if(!error){const auto at=link.find("zoneinfo/");if(at!=std::string::npos)return link.substr(at+9);}
  std::ifstream file("/etc/timezone");std::string line;if(std::getline(file,line)&&!line.empty())return line;
#endif
  return "UTC";
}
}
