// LLVM Support links its Unix process helpers even when every compiler stage
// runs in-process. A dedicated browser worker has no child process to reap.
#include <cerrno>
#include <sys/resource.h>
#include <sys/types.h>

extern "C" pid_t wait4(pid_t, int *, int, struct rusage *) {
    errno = ECHILD;
    return -1;
}
