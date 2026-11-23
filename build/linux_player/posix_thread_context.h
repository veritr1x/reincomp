#pragma once
#include "runtime.h"
#include <cerrno>
#include <climits>
#include <memory>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <system_error>

// Use one Linux key for the complete guest thread. Its destructor runs Android
// key callbacks before releasing the guest stack and TLS used by those callbacks.
class PosixThreadContext {
public:
    using Runner = std::function<addr_t(addr_t,addr_t)>;
    struct Local {
        PosixThreadContext &owner;
        std::map<unsigned,addr_t> values;
        std::shared_ptr<void> execution;
        size_t stackSize = 2 << 20;
        size_t guardSize = 4096;
        std::optional<uint64_t> inheritedMask;
        bool guestCreated = false;
    };
    explicit PosixThreadContext(Runner runner) : run(std::move(runner)) {
        if (auto error=pthread_key_create(&key,retire)) throw std::system_error(error,std::generic_category());
    }
    ~PosixThreadContext() {
        retireCurrent();
        pthread_key_delete(key);
    }
    void retireCurrent() {
        if (auto value=pthread_getspecific(key)) retire(value);
    }
    PosixThreadContext(const PosixThreadContext &) = delete;
    Local &local() {
        if (auto value=pthread_getspecific(key)) return *static_cast<Local *>(value);
        auto value=std::unique_ptr<Local>(new Local{*this});
        if (auto error=pthread_setspecific(key,value.get())) throw std::system_error(error,std::generic_category());
        return *value.release();
    }
protected:
    Runner run;
    std::mutex keyLock;
    int createKey(unsigned *out,addr_t destructor) {
        std::lock_guard guard(keyLock);
        if (destructors.size() >= PTHREAD_KEYS_MAX || nextKey == UINT_MAX) return EAGAIN;
        *out=nextKey++;
        destructors.emplace(*out,destructor);
        return 0;
    }
    int deleteKey(unsigned k) {
        std::lock_guard guard(keyLock);
        return destructors.erase(k) ? 0 : EINVAL;
    }
    int setValue(unsigned k,addr_t value) {
        std::lock_guard guard(keyLock);
        if (!destructors.count(k)) return EINVAL;
        if (value) local().values[k]=value;
        else local().values.erase(k);
        return 0;
    }
    addr_t getValue(unsigned k) {
        auto &values=local().values;
        std::lock_guard guard(keyLock);
        auto it=values.find(k);
        return it != values.end() && destructors.count(k) ? it->second : 0;
    }
private:
    pthread_key_t key;
    std::map<unsigned,addr_t> destructors;
    unsigned nextKey=0;
    static void retire(void *ptr) {
        std::unique_ptr<Local> value(static_cast<Local *>(ptr));
        auto &owner=value->owner;
        if (pthread_setspecific(owner.key,ptr)) std::terminate();
        for (int pass=0;pass<PTHREAD_DESTRUCTOR_ITERATIONS && !value->values.empty();++pass) {
            std::vector<unsigned> keys;
            for (const auto &entry:value->values) keys.push_back(entry.first);
            for (auto k:keys) {
                auto entry=value->values.extract(k);
                if (entry.empty()) continue;
                addr_t destructor=0;
                {
                    std::lock_guard guard(owner.keyLock);
                    auto it=owner.destructors.find(k);
                    if (it != owner.destructors.end()) destructor=it->second;
                }
                if (destructor) owner.run(destructor,entry.mapped());
            }
        }
        value->execution.reset();
        if (pthread_setspecific(owner.key,nullptr)) std::terminate();
    }
};
