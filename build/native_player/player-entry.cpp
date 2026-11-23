#include <windows.h>
#include <cstdio>

// Match the GPU selection exports of Unity's Windows standalone entry point.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 1;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR commandLine, int show) {
    constexpr DWORD search = LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
    // Unity startup on Proton can occupy the translated IL2CPP image's range.
    // Load the guest runtime first so it can reserve both fixed-address images.
    if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") &&
        !LoadLibraryExW(L"GuestRuntime.dll", nullptr, search)) {
        std::fprintf(stderr, "Could not load GuestRuntime.dll (Windows error %lu)\n", GetLastError());
        return 1;
    }
    auto unity = LoadLibraryExW(L"UnityPlayer.dll", nullptr, search);
    using UnityMain = int (WINAPI *)(HINSTANCE, HINSTANCE, PWSTR, int);
    auto start = unity ? reinterpret_cast<UnityMain>(GetProcAddress(unity, "UnityMain")) : nullptr;
    if (!start) {
        std::fprintf(stderr, "Could not load UnityMain from UnityPlayer.dll (Windows error %lu)\n", GetLastError());
        return 1;
    }
    return start(instance, previous, commandLine, show);
}
