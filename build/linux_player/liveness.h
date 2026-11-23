#pragma once
#include "bridge.h"
#include <array>
#include <map>
#include <mutex>

// The guest keeps the three callbacks in its liveness state. The engine can
// have multiple states, and a callback can re-enter Unity, so bind the matching
// native callbacks for each synchronous begin, traversal, and end invocation.
class LinuxLiveness {
    struct Callbacks { addr_t objects, started, stopped; };
    inline static thread_local const Callbacks *current = nullptr;
    struct Scope {
        const Callbacks *previous;
        explicit Scope(const Callbacks &callbacks) : previous(current) { current=&callbacks; }
        ~Scope() { current=previous; }
    };
    std::array<addr_t,3> markers{};
    std::mutex mutex;
    std::map<addr_t,Callbacks> states;
public:
    template<class Bind> void install(Bind bind) {
        markers[0]=bind(recomp::MakeBridge<void(void **,int,void *)>([](void **objects,int count,void *user) {
            if (!current || !current->objects) throw std::logic_error("unbound Unity liveness object callback");
            reinterpret_cast<void(*)(void **,int,void *)>(current->objects)(objects,count,user);
        }));
        for (unsigned i=1; i<markers.size(); ++i) {
            markers[i]=bind(recomp::MakeBridge<void()>([i]{
                if (!current) throw std::logic_error("unbound Unity liveness world callback");
                auto fn=i==1 ? current->started : current->stopped;
                if (!fn) throw std::logic_error("null Unity liveness world callback");
                reinterpret_cast<void(*)()>(fn)();
            }));
        }
    }
    template<class Run> addr_t begin(std::array<addr_t,6> args,Run run) {
        Callbacks callbacks{args[2],args[4],args[5]};
        Scope scope(callbacks);
        args[2]=callbacks.objects ? markers[0] : 0;
        args[4]=callbacks.started ? markers[1] : 0;
        args[5]=callbacks.stopped ? markers[2] : 0;
        auto state=run(args);
        if (state) {
            std::lock_guard lock(mutex);
            if (!states.emplace(state,callbacks).second) throw std::logic_error("duplicate Unity liveness state");
        }
        return state;
    }
    template<class Run> addr_t visit(addr_t state,bool ending,Run run) {
        Callbacks callbacks{};
        {
            std::lock_guard lock(mutex);
            callbacks=states.at(state);
            if (ending) states.erase(state);
        }
        Scope scope(callbacks);
        return run();
    }
};
