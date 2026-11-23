#pragma once

#include <windows.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <atomic>
#include <cstdio>

namespace frame_memory {

inline double mib(unsigned long long bytes) { return static_cast<double>(bytes) / (1024 * 1024); }

// These counters work with Unity's OpenGL renderer as well as the FSR pass.
// Keep driver discovery and queries off the game thread. At most one sample
// can be outstanding, even when the driver is slow or the game is closing.
struct GpuSampler {
    SRWLOCK lock = SRWLOCK_INIT;
    PTP_WORK work = nullptr;
    std::atomic<bool> busy{false};
    bool stopped = false;
    FILE *log = nullptr;
    int frame = 0;
    double qpcMs = 0;
    HMODULE dxgi = nullptr;
    decltype(&CreateDXGIFactory1) createFactory = nullptr;
    IDXGIFactory1 *factory = nullptr;
};
static GpuSampler gpu;

inline void CALLBACK sampleGpu(PTP_CALLBACK_INSTANCE, void *, PTP_WORK) {
    static bool initialized = false;
    if (!initialized) {
        initialized = true;
        gpu.dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (gpu.dxgi)
            gpu.createFactory = reinterpret_cast<decltype(gpu.createFactory)>(GetProcAddress(gpu.dxgi, "CreateDXGIFactory1"));
    }
    if (gpu.factory && !gpu.factory->IsCurrent()) {
        gpu.factory->Release();
        gpu.factory = nullptr;
    }
    if (!gpu.factory && gpu.createFactory)
        gpu.createFactory(IID_PPV_ARGS(&gpu.factory));

    bool reported = false;
    for (UINT index = 0; gpu.factory; ++index) {
        IDXGIAdapter1 *adapter = nullptr;
        if (gpu.factory->EnumAdapters1(index, &adapter) != S_OK) break;
        DXGI_ADAPTER_DESC1 desc{};
        IDXGIAdapter3 *usage = nullptr;
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&usage)))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
            const bool hasLocal = SUCCEEDED(usage->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local));
            const bool hasNonlocal = SUCCEEDED(usage->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal));
            if (hasLocal || hasNonlocal) {
                // DXGI reports per-process usage and budget per adapter and segment.
                // Local memory may be system RAM on an integrated GPU.
                std::fprintf(gpu.log, "gpuMemory frame=%d qpcMs=%.4f adapter=%08lx:%08lx vendor=%04x device=%04x "
                    "localUsedMiB=%.1f localBudgetMiB=%.1f nonlocalUsedMiB=%.1f nonlocalBudgetMiB=%.1f\n",
                    gpu.frame, gpu.qpcMs, static_cast<unsigned long>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart,
                    desc.VendorId, desc.DeviceId, hasLocal ? mib(local.CurrentUsage) : -1.0, hasLocal ? mib(local.Budget) : -1.0,
                    hasNonlocal ? mib(nonlocal.CurrentUsage) : -1.0, hasNonlocal ? mib(nonlocal.Budget) : -1.0);
                reported = true;
            }
            usage->Release();
        }
        adapter->Release();
    }
    if (!reported) std::fprintf(gpu.log, "gpuMemory frame=%d qpcMs=%.4f unavailable\n", gpu.frame, gpu.qpcMs);
    std::fflush(gpu.log);
    gpu.busy.store(false, std::memory_order_release);
}

inline void record(FILE *log, int frame, double qpcMs, unsigned long long managedUsed, unsigned long long managedHeap) {
    PROCESS_MEMORY_COUNTERS_EX process{};
    process.cb = sizeof(process);
    const bool hasProcess = GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&process), sizeof(process));
    MEMORYSTATUSEX system{};
    system.dwLength = sizeof(system);
    const bool hasSystem = GlobalMemoryStatusEx(&system);
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    std::fprintf(log, "memory frame=%d qpcMs=%.4f utc=%04u-%02u-%02uT%02u:%02u:%02u.%03uZ "
        "privateMiB=%.1f workingSetMiB=%.1f availableRamMiB=%.1f "
        "availableCommitMiB=%.1f managedUsedMiB=%.1f managedHeapMiB=%.1f\n", frame, qpcMs,
        utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds,
        hasProcess ? mib(process.PrivateUsage) : -1.0, hasProcess ? mib(process.WorkingSetSize) : -1.0,
        hasSystem ? mib(system.ullAvailPhys) : -1.0, hasSystem ? mib(system.ullAvailPageFile) : -1.0, mib(managedUsed), mib(managedHeap));

    AcquireSRWLockExclusive(&gpu.lock);
    if (!gpu.stopped && !gpu.busy.load(std::memory_order_acquire)) {
        if (!gpu.work) gpu.work = CreateThreadpoolWork(sampleGpu, nullptr, nullptr);
        if (gpu.work) {
            gpu.frame = frame;
            gpu.qpcMs = qpcMs;
            gpu.log = log;
            gpu.busy.store(true, std::memory_order_release);
            SubmitThreadpoolWork(gpu.work);
        } else {
            std::fprintf(log, "gpuMemory frame=%d qpcMs=%.4f unavailable\n", frame, qpcMs);
        }
    }
    ReleaseSRWLockExclusive(&gpu.lock);
}

inline void shutdown() {
    AcquireSRWLockExclusive(&gpu.lock);
    gpu.stopped = true;
    const auto work = gpu.work;
    gpu.work = nullptr;
    ReleaseSRWLockExclusive(&gpu.lock);
    if (!work) return;
    WaitForThreadpoolWorkCallbacks(work, FALSE);
    CloseThreadpoolWork(work);
    if (gpu.factory) gpu.factory->Release();
    if (gpu.dxgi) FreeLibrary(gpu.dxgi);
}

} // namespace frame_memory
