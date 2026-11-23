#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace controller_cursor {
struct RingImage {
    HDC dc{};
    HBITMAP bitmap{};
    HGDIOBJ previous{};
    int radius{};

    void clear() {
        if (previous) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        previous = nullptr;
        bitmap = nullptr;
        dc = nullptr;
        radius = 0;
    }
    ~RingImage() { clear(); }

    bool prepare(int size) {
        if (radius == size) return true;
        clear();
        const int side = 2 * size + 1;
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), side, -side, 1, 32, BI_RGB};
        void *bits = nullptr;
        dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap) return false;
        previous = SelectObject(dc, bitmap);
        if (!previous) return false;
        auto pixels = static_cast<uint32_t *>(bits);
        for (int y = -size; y <= size; ++y) for (int x = -size; x <= size; ++x) {
            const float distance = std::fabs(std::hypot(float(x), float(y)) - size * .72f);
            const float edge = std::clamp(size * .24f + .5f - distance, 0.f, 1.f);
            const float white = std::clamp(size * .12f + .5f - distance, 0.f, 1.f);
            // UpdateLayeredWindow expects premultiplied BGRA.
            const uint32_t gray = uint32_t(std::lround(20 * edge + 235 * white));
            *pixels++ = (uint32_t(std::lround(255 * edge)) << 24) | (gray * 0x010101u);
        }
        radius = size;
        return true;
    }
};

static bool display(HWND game, POINT client, BYTE opacity) {
    static HWND ring, owner;
    static RingImage image;
    static RECT lastBounds{};
    static POINT lastSource{};
    static BYTE lastAlpha;
    if (!opacity) {
        if (lastAlpha && IsWindow(ring)) ShowWindow(ring, SW_HIDE);
        lastAlpha = 0;
        return false;
    }
    if (!IsWindow(ring) || owner != game) {
        if (IsWindow(ring)) DestroyWindow(ring);
        // Unity can present its GPU surface above painted child windows. Give
        // the compositor the ring's pixels directly in a click-through owned popup.
        ring = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            L"STATIC", L"ReincompControllerCursor", WS_POPUP | WS_DISABLED,
            0, 0, 0, 0, game, nullptr, GetModuleHandleW(nullptr), nullptr);
        owner = game;
        lastAlpha = 0;
        if (!ring) return false;
    }
    static const auto getDpi = reinterpret_cast<UINT(WINAPI *)(HWND)>(
        GetProcAddress(GetModuleHandleW(L"user32"), "GetDpiForWindow"));
    const int radius = getDpi ? std::max(14, MulDiv(16, getDpi(game), 96)) : 16;
    RECT bounds{}, square{client.x-radius, client.y-radius, client.x+radius+1, client.y+radius+1};
    RECT clipped{};
    if (!GetClientRect(game, &bounds) || !IntersectRect(&clipped, &bounds, &square)) return false;
    POINT source{clipped.left-square.left, clipped.top-square.top};
    MapWindowPoints(game, nullptr, reinterpret_cast<POINT *>(&clipped), 2);
    if (opacity == lastAlpha && EqualRect(&clipped, &lastBounds) &&
        source.x == lastSource.x && source.y == lastSource.y && image.radius == radius) return true;
    if (!image.prepare(radius)) return false;
    SIZE size{clipped.right-clipped.left, clipped.bottom-clipped.top};
    POINT dest{clipped.left, clipped.top};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, opacity, AC_SRC_ALPHA};
    if (!UpdateLayeredWindow(ring, nullptr, &dest, &size, image.dc, &source, 0, &blend, ULW_ALPHA)) return false;
    if (!lastAlpha) SetWindowPos(ring, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    lastBounds = clipped;
    lastSource = source;
    lastAlpha = opacity;
    return true;
}
}
