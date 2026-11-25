// From the repo root, build with MinGW:
// g++ -std=c++17 -O2 -DADDRESS_SIZE_BITS=64 -Ithird_party/remill-include build/native_player/tests/quest-skill-icon.cpp -o quest-skill-icon.exe
// Run with the paths to UnityPlayer.dll and the built EngineCalls.dll, in that order.
#include "remill/Arch/AArch64/Runtime/State.h"
#include <windows.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

using Call = void (*)(State &, void *);
using Bind = Call (*)(const char *, void *);

struct UnityObject { void *klass, *monitor, *cachedPtr; };
struct GuestFrame { uintptr_t savedFrame, savedCaller; };
static void *seenObject, *seenScale;
static unsigned calls;
static uint8_t seenEnabled;

static void setScale(void *object, void *scale) {
    ++calls;
    seenObject = object;
    seenScale = scale;
}
static uint8_t getActive(void *object) {
    ++calls;
    seenObject = object;
    return 1;
}
static void setEnabled(void *object, uint8_t enabled) {
    ++calls;
    seenObject = object;
    seenEnabled = enabled;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: quest-skill-icon UnityPlayer.dll EngineCalls.dll\n");
        return 1;
    }
    auto player = LoadLibraryExA(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    auto engine = LoadLibraryA(argv[2]);
    if (!player || !engine) return 2;
    SetEnvironmentVariableA("REINCOMP_NATIVE_CHECKPOINT", "1");
    auto bind = reinterpret_cast<Bind>(GetProcAddress(engine, "BindEngineCall"));
    if (!bind) return 3;
    auto method = [&](const char *name, uintptr_t rva) {
        auto address = reinterpret_cast<uintptr_t>(player) + rva;
        auto callback = bind(name, reinterpret_cast<void *>(address));
        assert(callback);
        return callback;
    };
    auto scale = method("UnityEngine.Transform::set_localScale_Injected", 0x992140);
    auto active = method("UnityEngine.Behaviour::get_isActiveAndEnabled", 0x919370);
    auto enable = method("UnityEngine.Behaviour::set_enabled", 0x919450);

    int nativeObject = 0;
    UnityObject live{nullptr, nullptr, &nativeObject}, destroyed{};
    float vector[3] = {1, 2, 3};
    GuestFrame skillIcon{0, 0x82e2b94c}, otherCaller{0, 0x82e2b950};
    GuestFrame *frames[] = {&skillIcon, &otherCaller, nullptr};
    for (auto receiver : {&live, &destroyed}) {
        for (auto link : {0x84d80734u, 0x84d80738u}) {
            for (auto frame : frames) {
                State s{};
                s.gpr.x0.qword = reinterpret_cast<uintptr_t>(receiver);
                s.gpr.x1.qword = reinterpret_cast<uintptr_t>(vector);
                s.gpr.x29.qword = reinterpret_cast<uintptr_t>(frame);
                s.gpr.x30.qword = link;
                calls = 0;
                scale(s, reinterpret_cast<void *>(setScale));
                bool skip = receiver == &destroyed && link == 0x84d80734 && frame == &skillIcon;
                assert(calls == (skip ? 0u : 1u));
                if (calls) assert(seenObject == receiver && seenScale == vector);
            }
        }
        // Graphic.SetAllDirty checks layout, material and vertices. An unrelated
        // caller still receives the native getter's result on either receiver.
        for (auto caller : {0x83d31d8cu, 0x83d31f30u, 0x83d31e94u, 0x83d31f34u}) {
            State s{};
            s.gpr.x0.qword = reinterpret_cast<uintptr_t>(receiver);
            s.gpr.x30.qword = caller;
            calls = 0;
            active(s, reinterpret_cast<void *>(getActive));
            bool forward = receiver == &live || caller == 0x83d31f34;
            assert(calls == unsigned(forward) && s.gpr.x0.qword == unsigned(forward));
            if (calls) assert(seenObject == receiver);
        }
        for (auto caller : {0x82e2ba10u, 0x82e2ba14u}) {
            for (uint8_t value : {0, 1}) {
                State s{};
                s.gpr.x0.qword = reinterpret_cast<uintptr_t>(receiver);
                s.gpr.x1.qword = value;
                s.gpr.x30.qword = caller;
                calls = 0;
                enable(s, reinterpret_cast<void *>(setEnabled));
                bool forward = receiver == &live || caller == 0x82e2ba14;
                assert(calls == unsigned(forward));
                if (calls) assert(seenObject == receiver && seenEnabled == value);
            }
        }
    }
}
