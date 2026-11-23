#include "bridge.h"
#include <algorithm>
#include <cstring>
#include <iterator>

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
Callback bindUnityMethod(const char *name, void *function) {
    auto it = std::lower_bound(std::begin(methods), std::end(methods), name,
        [](const EngineMethod &a, const char *b) { return std::strcmp(a.name,b) < 0; });
    return it != std::end(methods) && !std::strcmp(it->name,name) ? it->bind(function) : Callback{};
}
