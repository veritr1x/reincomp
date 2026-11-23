#pragma once
#include "bridge.h"
#include "pthread_host.h"
#include <cerrno>
#include "posix_thread_context.h"
#include "signal_host.h"
#include <map>
#include <mutex>
#include <pthread.h>
#include <unistd.h>
#include <string>

class PosixThreads : public PosixThreadContext {
    using StackQuery = std::function<bool(pthread_t,addr_t &,size_t &)>;
    using MaskQuery = std::function<uint64_t()>;
    MaskQuery maskQuery;
    StackQuery stackQuery;
    std::mutex lock;

    std::map<addr_t,pthread_mutex_t> mutexes;
    std::map<addr_t,pthread_cond_t> conditions;
    std::map<addr_t,pthread_rwlock_t> rwlocks;
    using Wake = bunker::windows::SignalWake;
    std::multimap<addr_t,Wake *> signalWaiters;
    std::function<std::shared_ptr<Wake>()> waitChannel;
    std::function<bool()> signalPending;
    std::function<void()> dispatchSignal;

    int waitCondition(addr_t address,pthread_cond_t *condition,pthread_mutex_t *mutex) {
        auto wake=waitChannel ? waitChannel() : nullptr;
        if (!wake) return pthread_cond_wait(condition,mutex);
        const auto generation=wake->Generation();
        {
            std::lock_guard guard(lock);
            auto entry=signalWaiters.emplace(address,wake.get());
            if (int error=pthread_mutex_unlock(mutex)) { signalWaiters.erase(entry); return error; }
        }
        if (!signalPending()) wake->Wait(generation);
        {
            std::lock_guard guard(lock);
            auto [first,last]=signalWaiters.equal_range(address);
            for (auto it=first;it!=last;++it) if (it->second==wake.get()) { signalWaiters.erase(it); break; }
        }
        // A condition permits spurious wakes. The guest must own the mutex again
        // on return, but a stop handler must run while that mutex is released.
        if (signalPending()) dispatchSignal();
        return pthread_mutex_lock(mutex);
    }

    static int initMutex(pthread_mutex_t &mutex, int64_t type) {
        static constexpr int types[] = {PTHREAD_MUTEX_NORMAL,PTHREAD_MUTEX_RECURSIVE,PTHREAD_MUTEX_ERRORCHECK};
        if (type < 0 || type > 2) return EINVAL;
        pthread_mutexattr_t attr;
        int error = pthread_mutexattr_init(&attr);
        if (error) return error;
        error = pthread_mutexattr_settype(&attr,types[type]);
        if (!error) error = pthread_mutex_init(&mutex,&attr);
        pthread_mutexattr_destroy(&attr);
        return error;
    }
    pthread_mutex_t *mutexFor(addr_t addr) {
        std::lock_guard guard(lock);
        auto [it, fresh] = mutexes.try_emplace(addr);
        if (fresh) {
            uint16_t bits;
            std::memcpy(&bits,reinterpret_cast<void *>(addr),sizeof(bits));
            if (initMutex(it->second,bits >> 14)) { mutexes.erase(it); return nullptr; }
        }
        return &it->second;
    }
    int mutexOp(addr_t addr, int (*fn)(pthread_mutex_t *)) {
        auto mutex = mutexFor(addr);
        return mutex ? fn(mutex) : EINVAL;
    }
    static int initCondition(pthread_cond_t &cond, int64_t bits) {
        if (bits != 0 && bits != 2) return EINVAL;
        pthread_condattr_t attr;
        int error = pthread_condattr_init(&attr);
        if (error) return error;
        error = pthread_condattr_setclock(&attr,bits ? CLOCK_MONOTONIC : CLOCK_REALTIME);
        if (!error) error = pthread_cond_init(&cond,&attr);
        pthread_condattr_destroy(&attr);
        return error;
    }
    pthread_cond_t *conditionFor(addr_t addr) {
        std::lock_guard guard(lock);
        auto [it,fresh] = conditions.try_emplace(addr);
        if (fresh && initCondition(it->second,*reinterpret_cast<uint32_t *>(addr))) {
            conditions.erase(it); return nullptr;
        }
        return &it->second;
    }
public:
    void setSignalWait(std::function<std::shared_ptr<Wake>()> channel,
                       std::function<bool()> pending,std::function<void()> dispatch) {
        waitChannel=std::move(channel); signalPending=std::move(pending); dispatchSignal=std::move(dispatch);
    }
    explicit PosixThreads(Runner runner,StackQuery query={},MaskQuery mask={}) :
        PosixThreadContext(std::move(runner)),maskQuery(std::move(mask)),stackQuery(std::move(query)) {}
    std::function<void(State &)> lookup(const std::string &name) {
        using recomp::MakeBridge;
        using bunker::windows::AndroidThreadAttr;
        if (name == "pthread_key_create") return MakeBridge<int(unsigned *,addr_t)>([this](auto key,addr_t pc){return createKey(key,pc);});
        if (name == "pthread_key_delete") return MakeBridge<int(unsigned)>([this](unsigned key){return deleteKey(key);});
        if (name == "pthread_setspecific") return MakeBridge<int(unsigned,addr_t)>([this](auto key,auto value){return setValue(key,value);});
        if (name == "pthread_getspecific") return MakeBridge<addr_t(unsigned)>([this](auto key){return getValue(key);});
        if (name == "pthread_create") return MakeBridge<int(pthread_t *,const AndroidThreadAttr *,addr_t,addr_t)>(
            [this](pthread_t *out,const AndroidThreadAttr *attr,addr_t target,addr_t arg) {
                if (!target || !out) return EINVAL;
                if (attr && (attr->stack_base || attr->flags & ~1u || attr->sched_policy || attr->sched_priority)) return ENOTSUP;
                if (attr && attr->stack_size < 16384) return EINVAL;
                struct Start {
                    PosixThreads &owner;
                    addr_t target,arg;
                    size_t size,guard;
                    std::optional<uint64_t> mask;
                };
                auto start=std::unique_ptr<Start>(new Start{*this,target,arg,attr ? attr->stack_size : 1u<<20,
                    attr ? attr->guard_size : size_t(sysconf(_SC_PAGESIZE)),maskQuery ? std::optional(maskQuery()) : std::nullopt});
                pthread_attr_t native;
                int error=pthread_attr_init(&native);
                if (error) return error;
                if (attr && attr->flags & 1) error=pthread_attr_setdetachstate(&native,PTHREAD_CREATE_DETACHED);
                if (!error) {
                    // Transfer ownership before the child can run or exit.
                    auto raw=start.release();
                    error=pthread_create(out,&native,[](void *value) -> void * {
                        std::unique_ptr<Start> work(static_cast<Start *>(value));
                        auto &local=work->owner.local();
                        local.stackSize=work->size;
                        local.guardSize=work->guard;
                        local.inheritedMask=work->mask;
                        return reinterpret_cast<void *>(work->owner.run(work->target,work->arg));
                    },raw);
                    if (error) start.reset(raw);
                }
                pthread_attr_destroy(&native);
                return error;
            });
        if (name == "pthread_join") return MakeBridge<int(pthread_t,void **)>(pthread_join);
        if (name == "pthread_detach") return MakeBridge<int(pthread_t)>(pthread_detach);
        if (name == "pthread_setname_np") return MakeBridge<int(pthread_t,const char *)>(pthread_setname_np);
        if (name == "pthread_attr_setdetachstate") return MakeBridge<int(AndroidThreadAttr *,int)>([](auto attr,int state){
            if (state<0 || state>1) return EINVAL;
            attr->flags=(attr->flags & ~1u) | unsigned(state); return 0;
        });
        if (name == "pthread_attr_setstacksize") return MakeBridge<int(AndroidThreadAttr *,size_t)>([](auto attr,size_t size){
            if (size<16384) return EINVAL;
            attr->stack_size=size; return 0;
        });
        if (name == "pthread_self") return MakeBridge<pthread_t()>(pthread_self);
        if (name == "pthread_equal") return MakeBridge<int(pthread_t,pthread_t)>(pthread_equal);
        if (name == "pthread_attr_init") return MakeBridge<int(AndroidThreadAttr *)>([](auto attr){
            *attr={}; attr->stack_size=1<<20; attr->guard_size=sysconf(_SC_PAGESIZE); return 0;
        });
        if (name == "pthread_attr_destroy") return MakeBridge<int(AndroidThreadAttr *)>([](auto attr){*attr={}; return 0;});
        if (name == "pthread_getattr_np") return MakeBridge<int(pthread_t,AndroidThreadAttr *)>([this](auto thread,auto attr){
            addr_t base; size_t size;
            if (!stackQuery || !stackQuery(thread,base,size)) return ESRCH;
            *attr={}; attr->stack_base=base; attr->stack_size=size; attr->guard_size=sysconf(_SC_PAGESIZE);
            return 0;
        });
        if (name == "pthread_attr_getstack") return MakeBridge<int(const AndroidThreadAttr *,addr_t *,size_t *)>(
            [](auto attr,auto base,auto size){*base=attr->stack_base; *size=attr->stack_size; return 0;});
        if (name == "pthread_mutexattr_init") return MakeBridge<int(int64_t *)>([](auto attr) { *attr=0; return 0; });
        if (name == "pthread_mutexattr_settype") return MakeBridge<int(int64_t *,int)>([](auto attr,int type) {
            if (type < 0 || type > 2) return EINVAL;
            *attr=type; return 0;
        });
        if (name == "pthread_mutexattr_destroy") return MakeBridge<int(int64_t *)>([](auto attr) { *attr=-1; return 0; });
        if (name == "pthread_mutex_init") return MakeBridge<int(addr_t,const int64_t *)>([this](addr_t addr,auto attr) {
            std::lock_guard guard(lock);
            auto [it,fresh] = mutexes.try_emplace(addr);
            if (!fresh) return EBUSY;
            int error=initMutex(it->second,attr ? *attr : 0);
            if (error) mutexes.erase(it);
            return error;
        });
        if (name == "pthread_mutex_destroy") return MakeBridge<int(addr_t)>([this](addr_t addr) {
            std::lock_guard guard(lock);
            auto it=mutexes.find(addr);
            if (it==mutexes.end()) return 0;
            int error=pthread_mutex_destroy(&it->second);
            if (!error) mutexes.erase(it);
            return error;
        });
        if (name == "pthread_mutex_lock") return MakeBridge<int(addr_t)>([this](auto addr){return mutexOp(addr,pthread_mutex_lock);});
        if (name == "pthread_mutex_unlock") return MakeBridge<int(addr_t)>([this](auto addr){return mutexOp(addr,pthread_mutex_unlock);});
        if (name == "pthread_mutex_trylock") return MakeBridge<int(addr_t)>([this](auto addr){return mutexOp(addr,pthread_mutex_trylock);});
        if (name == "pthread_condattr_init") return MakeBridge<int(int64_t *)>([](auto attr){*attr=0; return 0;});
        if (name == "pthread_condattr_setclock") return MakeBridge<int(int64_t *,int)>([](auto attr,int clock) {
            if (clock != 0 && clock != 1) return EINVAL;
            *attr=clock << 1; return 0;
        });
        if (name == "pthread_condattr_destroy") return MakeBridge<int(int64_t *)>([](auto attr){*attr=-1; return 0;});
        if (name == "pthread_cond_init") return MakeBridge<int(addr_t,const int64_t *)>([this](auto addr,auto attr) {
            std::lock_guard guard(lock);
            auto [it,fresh] = conditions.try_emplace(addr);
            if (!fresh) return EBUSY;
            int error=initCondition(it->second,attr ? *attr : 0);
            if (error) conditions.erase(it);
            return error;
        });
        if (name == "pthread_cond_destroy") return MakeBridge<int(addr_t)>([this](auto addr) {
            std::lock_guard guard(lock);
            if (signalWaiters.count(addr)) return EBUSY;
            auto it=conditions.find(addr);
            if (it==conditions.end()) return 0;
            int error=pthread_cond_destroy(&it->second);
            if (!error) conditions.erase(it);
            return error;
        });
        if (name == "pthread_cond_wait") return MakeBridge<int(addr_t,addr_t)>([this](auto cond,auto mutex) {
            auto c=conditionFor(cond); auto m=mutexFor(mutex);
            return c && m ? waitCondition(cond,c,m) : EINVAL;
        });
        if (name == "pthread_cond_timedwait") return MakeBridge<int(addr_t,addr_t,const timespec *)>([this](auto cond,auto mutex,auto time) {
            static_assert(sizeof(timespec)==16);
            auto c=conditionFor(cond); auto m=mutexFor(mutex);
            return c && m ? pthread_cond_timedwait(c,m,time) : EINVAL;
        });
        if (name == "pthread_cond_signal" || name == "pthread_cond_broadcast")
            return MakeBridge<int(addr_t)>([this,all=name=="pthread_cond_broadcast"](auto addr) {
                auto cond=conditionFor(addr);
                if (!cond) return EINVAL;
                {
                    std::lock_guard guard(lock);
                    auto [first,last]=signalWaiters.equal_range(addr);
                    while (first!=last) {
                        first->second->Notify();
                        first=signalWaiters.erase(first);
                        if (!all) return 0;
                    }
                }
                return all ? pthread_cond_broadcast(cond) : pthread_cond_signal(cond);
            });
        if (name == "pthread_rwlock_init") return MakeBridge<int(addr_t,const int64_t *)>([this](auto addr,auto attr) {
            if (attr && *attr) return ENOTSUP;
            std::lock_guard guard(lock);
            auto [it,fresh]=rwlocks.try_emplace(addr);
            if (!fresh) return EBUSY;
            int error=pthread_rwlock_init(&it->second,nullptr);
            if (error) rwlocks.erase(it);
            return error;
        });
        if (name == "pthread_rwlock_destroy") return MakeBridge<int(addr_t)>([this](auto addr) {
            std::lock_guard guard(lock);
            auto it=rwlocks.find(addr);
            if (it==rwlocks.end()) return 0;
            int error=pthread_rwlock_destroy(&it->second);
            if (!error) rwlocks.erase(it);
            return error;
        });
        if (name == "pthread_rwlock_rdlock" || name == "pthread_rwlock_wrlock" || name == "pthread_rwlock_unlock") {
            auto operation = name == "pthread_rwlock_rdlock" ? pthread_rwlock_rdlock :
                name == "pthread_rwlock_wrlock" ? pthread_rwlock_wrlock : pthread_rwlock_unlock;
            return MakeBridge<int(addr_t)>([this,operation](auto addr) {
                pthread_rwlock_t *rw;
                {
                    std::lock_guard guard(lock);
                    auto [it,fresh]=rwlocks.try_emplace(addr);
                    if (fresh) {
                        int error=pthread_rwlock_init(&it->second,nullptr);
                        if (error) {rwlocks.erase(it); return error;}
                    }
                    rw=&it->second;
                }
                return operation(rw);
            });
        }
        return {};
    }
};
