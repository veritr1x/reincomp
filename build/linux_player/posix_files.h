#pragma once
#include "bridge.h"
#include "filesystem.h"
#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <iterator>
#include <map>
#include <mutex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

namespace linux_files {
inline thread_local int guestErrno;

// Boehm releases heap pages by replacing them at the same address with
// anonymous PROT_NONE pages. Only memory reserved by this guest may be replaced.
class Mappings {
    std::mutex lock;
    std::map<uintptr_t,uintptr_t> ranges;
    const size_t page=static_cast<size_t>(sysconf(_SC_PAGESIZE));

    bool endOf(uintptr_t start,size_t length,uintptr_t &end) const {
        if (!length || length>SIZE_MAX-(page-1)) return false;
        size_t rounded=(length+page-1)&~(page-1);
        if (rounded>UINTPTR_MAX-start) return false;
        end=start+rounded;
        return true;
    }
    bool owns(uintptr_t start,uintptr_t end) const {
        auto next=ranges.upper_bound(start);
        return next!=ranges.begin() && std::prev(next)->second>=end;
    }
    void remember(uintptr_t start,uintptr_t end) {
        auto next=ranges.lower_bound(start);
        if (next!=ranges.begin()) {
            auto prev=std::prev(next);
            if (prev->second>=start) {
                start=prev->first; end=std::max(end,prev->second);
                ranges.erase(prev);
            }
        }
        while (next!=ranges.end() && next->first<=end) {
            end=std::max(end,next->second);
            next=ranges.erase(next);
        }
        ranges.emplace(start,end);
    }
public:
    void *map(void *address,size_t length,int prot,int flags,int fd,off_t offset) {
        std::lock_guard guard(lock);
        uintptr_t start=uintptr_t(address),end;
        if (!endOf(start,length,end)) { guestErrno=EINVAL; return MAP_FAILED; }
        if ((flags&MAP_FIXED) && !owns(start,end))
            flags=(flags&~MAP_FIXED)|MAP_FIXED_NOREPLACE;
        void *result=::mmap(address,length,prot&~PROT_EXEC,flags,fd,offset);
        if (result==MAP_FAILED) {guestErrno=errno; return result;}
        if ((flags&MAP_FIXED_NOREPLACE) && result!=address) {
            ::munmap(result,length);
            guestErrno=EEXIST;
            return MAP_FAILED;
        }
        remember(uintptr_t(result),uintptr_t(result)+(end-start));
        return result;
    }
    int unmap(void *address,size_t length) {
        std::lock_guard guard(lock);
        uintptr_t start=uintptr_t(address),end;
        if (!endOf(start,length,end) || !owns(start,end)) { guestErrno=EINVAL; return -1; }
        if (::munmap(address,length)) {guestErrno=errno; return -1;}
        auto it=std::prev(ranges.upper_bound(start));
        auto [lo,hi]=*it;
        ranges.erase(it);
        if (lo<start) ranges.emplace(lo,start);
        if (hi>end) ranges.emplace(end,hi);
        return 0;
    }
};
inline Mappings mappings;

template<class T> T checked(T result) {
    if (result == T(-1)) guestErrno=errno;
    return result;
}
inline int openFile(const char *path, int flags, unsigned mode) {
    using namespace bunker::windows::linux_flags;
    int common = O_ACCMODE|O_CREAT|O_EXCL|O_TRUNC|O_APPEND|O_NONBLOCK|O_CLOEXEC|O_SYNC;
    if (flags & ~(common|directory|no_follow|large_file)) { guestErrno=EINVAL; return -1; }
    int native = (flags & common) | ((flags & directory) ? O_DIRECTORY : 0) | ((flags & no_follow) ? O_NOFOLLOW : 0);
    return checked(::open(path,native,mode));
}
inline void copyStat(const struct stat &s,bunker::windows::AndroidStat *out) {
    using bunker::windows::AndroidStat;
    AndroidStat wire{};
    wire.device=s.st_dev; wire.inode=s.st_ino; wire.mode=s.st_mode; wire.links=s.st_nlink;
    wire.uid=s.st_uid; wire.gid=s.st_gid; wire.rdevice=s.st_rdev; wire.size=s.st_size;
    wire.block_size=s.st_blksize; wire.blocks=s.st_blocks;
    wire.access_time={s.st_atim.tv_sec,s.st_atim.tv_nsec};
    wire.modification_time={s.st_mtim.tv_sec,s.st_mtim.tv_nsec};
    wire.change_time={s.st_ctim.tv_sec,s.st_ctim.tv_nsec};
    recomp::WriteGuestMemory(reinterpret_cast<addr_t>(out),&wire,sizeof(wire));
}
inline std::function<void(State &)> lookup(const std::string &name) {
    using recomp::MakeBridge;
    using bunker::windows::AndroidStat;
    if (name=="__errno") return MakeBridge<int *()>([]{return &guestErrno;});
    if (name=="clock_gettime" || name=="clock_getres") {
        auto clock = name=="clock_gettime" ? clock_gettime : clock_getres;
        return MakeBridge<int(int,timespec *)>([clock](int id,auto out){return checked(clock(id,out));});
    }
    if (name=="usleep") return MakeBridge<int(unsigned)>([](unsigned us){return checked(usleep(us));});
    if (name=="nanosleep") return MakeBridge<int(const timespec *,timespec *)>([](auto request,auto remaining){
        return checked(::nanosleep(request,remaining));
    });
    if (name=="open") return MakeBridge<int(const char *,int,unsigned)>(openFile);
    if (name=="mkdir") return MakeBridge<int(const char *,unsigned)>([](auto path,unsigned mode){return checked(::mkdir(path,mode));});
    if (name=="rmdir") return MakeBridge<int(const char *)>([](auto path){return checked(::rmdir(path));});
    if (name=="rename") return MakeBridge<int(const char *,const char *)>([](auto src,auto dest){return checked(::rename(src,dest));});
    if (name=="unlink") return MakeBridge<int(const char *)>([](auto path){return checked(::unlink(path));});
    if (name=="readlink") return MakeBridge<ssize_t(const char *,char *,size_t)>([](auto path,auto out,size_t size){return checked(::readlink(path,out,size));});
    if (name=="getcwd") return MakeBridge<char *(char *,size_t)>([](auto path,size_t size){
        auto result=::getcwd(path,size);
        if (!result) guestErrno=errno;
        return result;
    });
    if (name=="opendir") return MakeBridge<DIR *(const char *)>([](auto path){
        auto result=::opendir(path);
        if (!result) guestErrno=errno;
        return result;
    });
    if (name=="readdir") return MakeBridge<dirent *(DIR *)>([](auto dir){
        // Bionic arm64 and glibc x86-64 both expose the Linux dirent64 layout.
        static_assert(sizeof(dirent)==280 && offsetof(dirent,d_off)==8 && offsetof(dirent,d_reclen)==16 &&
                      offsetof(dirent,d_type)==18 && offsetof(dirent,d_name)==19);
        errno=0;
        auto result=::readdir(dir);
        if (!result && errno) guestErrno=errno;
        return result;
    });
    if (name=="closedir") return MakeBridge<int(DIR *)>([](auto dir){return checked(::closedir(dir));});
    if (name=="close") return MakeBridge<int(int)>([](int fd){return checked(::close(fd));});
    if (name=="dup") return MakeBridge<int(int)>([](int fd){return checked(dup(fd));});
    if (name=="dup2") return MakeBridge<int(int,int)>([](int fd,int target){return checked(dup2(fd,target));});
    if (name=="pipe") return MakeBridge<int(int *)>([](int *fds){return checked(pipe(fds));});
    if (name=="fcntl") return MakeBridge<int(int,int,addr_t)>([](int fd,int cmd,addr_t arg) {
        switch (cmd) {
            case F_GETFD: return checked(fcntl(fd,F_GETFD));
            case F_SETFD: case F_DUPFD: case F_DUPFD_CLOEXEC: return checked(fcntl(fd,cmd,int(arg)));
            default: guestErrno=ENOTSUP; return -1;
        }
    });
    if (name=="read") return MakeBridge<ssize_t(int,void *,size_t)>([](int fd,void *p,size_t n){return checked(::read(fd,p,n));});
    if (name=="write") return MakeBridge<ssize_t(int,const void *,size_t)>([](int fd,const void *p,size_t n){return checked(::write(fd,p,n));});
    if (name=="ftruncate") return MakeBridge<int(int,off_t)>([](int fd,off_t n){return checked(ftruncate(fd,n));});
    if (name=="mprotect") return MakeBridge<int(void *,size_t,int)>([](void *p,size_t n,int prot){return checked(mprotect(p,n,prot & ~PROT_EXEC));});
    if (name=="lseek" || name=="lseek64") return MakeBridge<off_t(int,off_t,int)>([](int fd,off_t off,int mode){return checked(lseek(fd,off,mode));});
    if (name=="access") return MakeBridge<int(const char *,int)>([](auto p,int mode){return checked(access(p,mode));});
    if (name=="fstat") return MakeBridge<int(int,AndroidStat *)>([](int fd,auto out) {
        struct stat st{};
        int error=checked(fstat(fd,&st));
        if (!error) copyStat(st,out);
        return error;
    });
    if (name=="stat" || name=="lstat") return MakeBridge<int(const char *,AndroidStat *)>([follow=name=="stat"](auto p,auto out) {
        struct stat st{};
        int error=checked(follow ? stat(p,&st) : lstat(p,&st));
        if (!error) copyStat(st,out);
        return error;
    });
    if (name=="mmap" || name=="mmap64") return MakeBridge<void *(void *,size_t,int,int,int,off_t)>([](void *p,size_t n,int prot,int flags,int fd,off_t off) {
        return mappings.map(p,n,prot,flags,fd,off);
    });
    if (name=="munmap") return MakeBridge<int(void *,size_t)>([](void *p,size_t n){return mappings.unmap(p,n);});
    return {};
}
}
