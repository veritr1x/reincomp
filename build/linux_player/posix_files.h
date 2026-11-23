#pragma once
#include "bridge.h"
#include "filesystem.h"
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

namespace linux_files {
inline thread_local int guestErrno;

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
    if (name=="open") return MakeBridge<int(const char *,int,unsigned)>(openFile);
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
        if (flags & MAP_FIXED) {guestErrno=ENOTSUP; return MAP_FAILED;}
        return checked(mmap(p,n,prot & ~PROT_EXEC,flags,fd,off));
    });
    if (name=="munmap") return MakeBridge<int(void *,size_t)>([](void *p,size_t n){return checked(munmap(p,n));});
    return {};
}
}
