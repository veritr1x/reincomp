"""Sample the instruction pointer (and a few return addresses) of native
threads in a running Windows process.

    python rip_sampler.py PID SECONDS OUT.json [--hz 500] [--tids 1,2]

Each sample suspends one thread, reads RIP and walks up to DEPTH stack words
looking for return addresses inside the main module, then resumes it. Output is
a JSON object of {tid: [[rip, ret1, ret2, ...], ...]} for symbolization with
symbolize_samples.py. Diagnostic only: it never writes target memory.
"""
import ctypes as C, ctypes.wintypes as W, json, sys, time, argparse

k32 = C.WinDLL('kernel32', use_last_error=True)
THREAD_ALL = 0x0002 | 0x0008 | 0x0010 | 0x0040  # suspend/resume, get context, query
PROCESS_VM_READ, PROCESS_QUERY = 0x0010, 0x0400
CONTEXT_CONTROL_INTEGER = 0x00100001 | 0x00100002

class M128(C.Structure):
    _fields_ = [('lo', C.c_uint64), ('hi', C.c_int64)]

class CONTEXT(C.Structure):
    _pack_ = 16
    _fields_ = ([(n, C.c_uint64) for n in ('P1Home', 'P2Home', 'P3Home', 'P4Home', 'P5Home', 'P6Home')] +
                [('ContextFlags', C.c_uint32), ('MxCsr', C.c_uint32)] +
                [(n, C.c_uint16) for n in ('SegCs', 'SegDs', 'SegEs', 'SegFs', 'SegGs', 'SegSs')] +
                [('EFlags', C.c_uint32)] +
                [(n, C.c_uint64) for n in ('Dr0', 'Dr1', 'Dr2', 'Dr3', 'Dr6', 'Dr7',
                                           'Rax', 'Rcx', 'Rdx', 'Rbx', 'Rsp', 'Rbp', 'Rsi', 'Rdi',
                                           'R8', 'R9', 'R10', 'R11', 'R12', 'R13', 'R14', 'R15', 'Rip')] +
                [('FltSave', C.c_byte * 512), ('VectorRegister', M128 * 26), ('VectorControl', C.c_uint64)] +
                [(n, C.c_uint64) for n in ('DebugControl', 'LastBranchToRip', 'LastBranchFromRip',
                                           'LastExceptionToRip', 'LastExceptionFromRip')])

class TE32(C.Structure):
    _fields_ = [('dwSize', W.DWORD), ('cntUsage', W.DWORD), ('th32ThreadID', W.DWORD),
                ('th32OwnerProcessID', W.DWORD), ('tpBasePri', W.LONG), ('tpDeltaPri', W.LONG), ('dwFlags', W.DWORD)]

class ME32(C.Structure):
    _fields_ = [('dwSize', W.DWORD), ('th32ModuleID', W.DWORD), ('th32ProcessID', W.DWORD),
                ('GlblcntUsage', W.DWORD), ('ProccntUsage', W.DWORD), ('modBaseAddr', C.c_void_p),
                ('modBaseSize', W.DWORD), ('hModule', W.HMODULE), ('szModule', C.c_char * 256),
                ('szExePath', C.c_char * 260)]

k32.OpenThread.restype = W.HANDLE
k32.OpenProcess.restype = W.HANDLE
k32.CreateToolhelp32Snapshot.restype = W.HANDLE


def threads(pid):
    snap = k32.CreateToolhelp32Snapshot(4, 0)
    te = TE32(); te.dwSize = C.sizeof(te); out = []
    ok = k32.Thread32First(snap, C.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            out.append(te.th32ThreadID)
        ok = k32.Thread32Next(snap, C.byref(te))
    k32.CloseHandle(snap)
    return out


def main_module(pid):
    snap = k32.CreateToolhelp32Snapshot(0x8 | 0x10, pid)
    me = ME32(); me.dwSize = C.sizeof(me)
    assert k32.Module32First(snap, C.byref(me)), C.get_last_error()
    base, size, name = me.modBaseAddr, me.modBaseSize, me.szExePath.decode(errors='replace')
    k32.CloseHandle(snap)
    return base, size, name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pid', type=int); ap.add_argument('seconds', type=float); ap.add_argument('out')
    ap.add_argument('--hz', type=float, default=500); ap.add_argument('--tids', default='')
    ap.add_argument('--depth', type=int, default=4096)
    a = ap.parse_args()
    base, size, exe = main_module(a.pid)
    proc = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY, False, a.pid)
    tids = [int(t) for t in a.tids.split(',') if t] or threads(a.pid)
    handles = {t: k32.OpenThread(THREAD_ALL, False, t) for t in tids}
    ctx = CONTEXT(); buf = (C.c_uint64 * a.depth)(); got = C.c_size_t()
    samples = {t: [] for t in tids}
    end = time.perf_counter() + a.seconds; period = 1.0 / a.hz
    while time.perf_counter() < end:
        for t, h in handles.items():
            if not h or k32.SuspendThread(h) == 0xFFFFFFFF:
                continue
            try:
                ctx.ContextFlags = CONTEXT_CONTROL_INTEGER
                if not k32.GetThreadContext(h, C.byref(ctx)):
                    continue
                chain = [ctx.Rip]
                if k32.ReadProcessMemory(proc, C.c_void_p(ctx.Rsp), buf, C.sizeof(buf), C.byref(got)) or got.value:
                    for i in range(got.value // 8):
                        v = buf[i]
                        if base <= v < base + size:
                            chain.append(v)
                            if len(chain) >= 12:
                                break
                samples[t].append(chain)
            finally:
                k32.ResumeThread(h)
        time.sleep(period)
    json.dump(dict(exe=exe, base=base, size=size, samples=samples), open(a.out, 'w'))
    print(json.dumps({t: len(s) for t, s in samples.items() if s}))


if __name__ == '__main__':
    main()
