#include "bridge.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string_view>

using P = void *;
using Callback = std::function<void(State &)>;
template<class R, class... A> static Callback invoke(void *fn) {
    return recomp::MakeBridge<R(A...)>(reinterpret_cast<R(*)(A...)>(fn));
}
struct EngineMethod {
    const char *name;
    unsigned windowsRva; // Linux Unity supplies its own function pointers at registration.
    Callback (*bind)(void *);
};
static const EngineMethod methods[] = {
#include "../native_player/abi-2019.4.29f1/engine-calls.inc"
};
extern "C" const char16_t *il2cpp_string_chars(P);
extern "C" int32_t il2cpp_string_length(P);
extern "C" P il2cpp_string_new_utf16(const char16_t *,int32_t);

static std::u16string packagedUrl(std::u16string_view url,const std::filesystem::path &streaming) {
    const auto jar=url.find(u"jar:file:");
    if (jar==url.npos || (jar && url.substr(0,jar)!=u"http://" && url.substr(0,jar)!=u"https://")) return {};
    constexpr std::u16string_view marker=u"!/assets/";
    const auto pos=url.find(marker,jar);
    if (pos==url.npos) return {};
    auto name=url.substr(pos+marker.size());
    name=name.substr(0,name.find_first_of(u"?#"));
    if (name.empty() || name.front()==u'/' || name.find_first_of(u"\\:%")!=name.npos || name.find(char16_t{})!=name.npos) return {};
    std::filesystem::path relative(name);
    for (const auto &part:relative) if (part=="." || part=="..") return {};
    auto path=streaming/relative;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path,ec)) return {};
    // Unity 2019's local file handler passes the path straight to open(2).
    // Percent-encoding an @ or a space changes the filename it actually opens.
    return u"file://"+path.u16string();
}

Callback bindUnityMethod(const char *name, void *function, unsigned assetPort, const std::filesystem::path &streaming) {
    if (!std::strcmp(name,"UnityEngine.Networking.UnityWebRequest::SetUrl")) {
        const auto local="http://127.0.0.1:"+std::to_string(assetPort);
        return [setUrl=reinterpret_cast<int32_t(*)(P,P)>(function),base=std::u16string(local.begin(),local.end()),streaming](State &s) {
            P url=reinterpret_cast<P>(s.gpr.x1.qword);
            if (url) {
                constexpr std::u16string_view source=u"http://127.0.0.1:38080";
                std::u16string_view text(il2cpp_string_chars(url),il2cpp_string_length(url));
                auto adjusted=packagedUrl(text,streaming);
                if (base!=source && text.substr(0,source.size())==source &&
                    (text.size()==source.size() || text[source.size()]==u'/' || text[source.size()]==u'?' || text[source.size()]==u'#')) {
                    adjusted=base;
                    adjusted.append(text.substr(source.size()));
                }
                if (!adjusted.empty()) url=il2cpp_string_new_utf16(adjusted.data(),static_cast<int32_t>(adjusted.size()));
            }
            s.gpr.x0.qword=uint32_t(setUrl(reinterpret_cast<P>(s.gpr.x0.qword),url));
        };
    }
    auto it = std::lower_bound(std::begin(methods), std::end(methods), name,
        [](const EngineMethod &a, const char *b) { return std::strcmp(a.name,b) < 0; });
    return it != std::end(methods) && !std::strcmp(it->name,name) ? it->bind(function) : Callback{};
}
