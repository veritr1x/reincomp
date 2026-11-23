#pragma once
#include <windows.h>
#include <dinput.h>
#include <xinput.h>
extern "C" {
#include <hidsdi.h>
}

namespace desktop_gamepad {
inline WORD hatDirection(unsigned direction) {
    static constexpr WORD hats[] = {
        XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT,
        XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_DPAD_DOWN,
        XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT,
        XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_UP
    };
    return direction < 8 ? hats[direction] : 0;
}
// DirectInput exposes the sticks and buttons, but the touch coordinates are
// in Sony's full HID input reports. USB and Bluetooth share the same payload.
struct DualSenseReport {
    int finger = -1;
    POINT last{};
    XINPUT_GAMEPAD pad{};
    bool clickDown = false, clickEdge = false, waitForRelease = false;

    bool takeClick(bool resumed) {
        if (resumed) waitForRelease = clickDown;
        const bool tap = !resumed && !waitForRelease && (clickDown || clickEdge);
        clickEdge = false;
        return tap;
    }

    bool sample(const BYTE *report, DWORD bytes, POINT &delta) {
        const BYTE *p;
        if (bytes == 64 && report[0] == 1) p = report + 1;
        else if (bytes == 78 && report[0] == 0x31) p = report + 2;
        else return false;
        // Bluetooth enhanced mode can stop updating the legacy joystick report.
        // Keep using the buttons and sticks from the same physical controller.
        auto axis = [](BYTE v) { return SHORT(int(v) * 257 - 32768); };
        pad = {};
        pad.sThumbLX = axis(p[0]); pad.sThumbLY = axis(255 - p[1]);
        pad.sThumbRX = axis(p[2]); pad.sThumbRY = axis(255 - p[3]);
        pad.bLeftTrigger = p[4]; pad.bRightTrigger = p[5];
        pad.wButtons = hatDirection(p[7] & 15);
        if (p[7] & 0x20) pad.wButtons |= XINPUT_GAMEPAD_A;
        if (p[7] & 0x40) pad.wButtons |= XINPUT_GAMEPAD_B;
        if (p[7] & 0x80) pad.wButtons |= XINPUT_GAMEPAD_Y;
        if (p[8] & 0x20) pad.wButtons |= XINPUT_GAMEPAD_BACK; // Options
        // The mechanical click is independent of finger contact. Remember a
        // short click even if its release arrives in the same batch of reports.
        const bool down = (p[9] & 0x02) != 0;
        if (!down) waitForRelease = false;
        if (down && !clickDown) clickEdge = true;
        clickDown = down;
        const BYTE *a = p + 32, *b = p + 36;
        if ((a[0] ^ b[0]) & 0x80) { // Exactly one contact, in either slot.
            const BYTE *t = (a[0] & 0x80) ? b : a;
            POINT at{t[1] | ((t[2] & 15) << 8), (t[2] >> 4) | (t[3] << 4)};
            if (at.x < 1920 && at.y < 1080) {
                if (finger == t[0]) { delta.x += at.x - last.x; delta.y += at.y - last.y; }
                finger = t[0];
                last = at;
                return true;
            }
        }
        finger = -1;
        return true;
    }
};

struct DualSenseTouchpad {
    HANDLE file = INVALID_HANDLE_VALUE;
    OVERLAPPED io{};
    bool pending = false, requestedFull = false;
    BYTE input[128]{};
    USHORT reportLength{}, featureLength{};
    DualSenseReport report;
    ULONGLONG lastPoll{}, lastFull{};

    void close() {
        if (file != INVALID_HANDLE_VALUE) {
            if (pending) {
                DWORD ignored;
                CancelIoEx(file, &io);
                GetOverlappedResult(file, &io, &ignored, TRUE);
            }
            CloseHandle(file);
        }
        if (io.hEvent) CloseHandle(io.hEvent);
        *this = {};
    }
    void open(IDirectInputDevice8W *device) {
        DIPROPGUIDANDPATH path{};
        path.diph = {sizeof(path), sizeof(path.diph), 0, DIPH_DEVICE};
        if (FAILED(device->GetProperty(DIPROP_GUIDANDPATH, &path.diph))) return;
        file = CreateFileW(path.wszPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        HIDP_CAPS caps{};
        PHIDP_PREPARSED_DATA data{};
        if (HidD_GetPreparsedData(file, &data)) {
            if (HidP_GetCaps(data, &caps) == HIDP_STATUS_SUCCESS) {
                reportLength = caps.InputReportByteLength;
                featureLength = caps.FeatureReportByteLength;
            }
            HidD_FreePreparsedData(data);
        }
        io.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!io.hEvent || reportLength < 64 || reportLength > sizeof(input)) close();
    }
    bool poll(POINT &delta, XINPUT_GAMEPAD &pad, bool &click) {
        if (file == INVALID_HANDLE_VALUE) return false;
        const auto now = GetTickCount64();
        const bool resumed = !lastPoll || now-lastPoll > 150;
        lastPoll = now;
        for (int i = 0; i < 64; ++i) {
            DWORD bytes = 0;
            if (pending) {
                if (!GetOverlappedResult(file, &io, &bytes, FALSE)) {
                    if (GetLastError() != ERROR_IO_INCOMPLETE) close();
                    break;
                }
                pending = false;
            } else {
                ResetEvent(io.hEvent);
                if (!ReadFile(file, input, reportLength, &bytes, &io)) {
                    if (GetLastError() == ERROR_IO_PENDING) pending = true;
                    else close();
                    break;
                }
            }
            if (report.sample(input, bytes, delta)) lastFull = now;
            else if (bytes && input[0] == 1 && reportLength > 64 && !requestedFull) {
                // Reading the firmware feature switches Sony's Bluetooth
                // simple mode to full input without setting rumble or LEDs.
                requestedFull = true;
                BYTE firmware[512]{0x20};
                if (featureLength && featureLength <= sizeof(firmware))
                    HidD_GetFeature(file, firmware, featureLength);
            }
        }
        if (resumed) { delta = {}; report.finger = -1; }
        if (!lastFull || now-lastFull > 250) return false;
        pad = report.pad;
        click = report.takeClick(resumed);
        return true;
    }
};
static DualSenseTouchpad touchpad;
}
