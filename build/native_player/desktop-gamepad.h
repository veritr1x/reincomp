#pragma once
#include <windows.h>
#include <dinput.h>
#include <xinput.h>
#include <algorithm>
#include <atomic>

namespace desktop_gamepad {
// Sony's Windows HID mapping uses X/Y and Z/Rz for the two sticks.
// Cross and Circle are HID buttons 2 and 3. The hat is in hundredths of a degree.
inline XINPUT_GAMEPAD playStationState(const DIJOYSTATE &input){
    auto axis=[](LONG value,bool invert){
        value=std::clamp<LONG>(value,0,65535);
        return static_cast<SHORT>(invert?32767-value:value-32768);
    };
    XINPUT_GAMEPAD state{};
    state.sThumbLX=axis(input.lX,false);state.sThumbLY=axis(input.lY,true);
    state.sThumbRX=axis(input.lZ,false);state.sThumbRY=axis(input.lRz,true);
    if(input.rgbButtons[1]&0x80)state.wButtons|=XINPUT_GAMEPAD_A;
    if(input.rgbButtons[2]&0x80)state.wButtons|=XINPUT_GAMEPAD_B;
    if(input.rgdwPOV[0]<36000){
        static constexpr WORD hat[]={
            XINPUT_GAMEPAD_DPAD_UP,XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_DPAD_RIGHT,
            XINPUT_GAMEPAD_DPAD_RIGHT,XINPUT_GAMEPAD_DPAD_RIGHT|XINPUT_GAMEPAD_DPAD_DOWN,
            XINPUT_GAMEPAD_DPAD_DOWN,XINPUT_GAMEPAD_DPAD_DOWN|XINPUT_GAMEPAD_DPAD_LEFT,
            XINPUT_GAMEPAD_DPAD_LEFT,XINPUT_GAMEPAD_DPAD_LEFT|XINPUT_GAMEPAD_DPAD_UP};
        state.wButtons|=hat[((input.rgdwPOV[0]+2250)/4500)%8];
    }
    return state;
}

static IDirectInput8W *api;
static IDirectInputDevice8W *device;
static std::atomic<bool> devicesChanged{true};

static void closeDevice(){
    if(device){device->Unacquire();device->Release();device=nullptr;}
}
static void shutdown(){
    closeDevice();
    if(api){api->Release();api=nullptr;}
    devicesChanged.store(true);
}
static BOOL CALLBACK openController(const DIDEVICEINSTANCEW *info,void *context){
    if(LOWORD(info->guidProduct.Data1)!=0x054c)return DIENUM_CONTINUE;
    switch(HIWORD(info->guidProduct.Data1)){
        case 0x05c4:case 0x09cc:case 0x0ba0: // DualShock 4
        case 0x0ce6:case 0x0df2:             // DualSense and DualSense Edge
            break;
        default:return DIENUM_CONTINUE;
    }
    if(FAILED(api->CreateDevice(info->guidInstance,&device,nullptr)))return DIENUM_CONTINUE;
    if(FAILED(device->SetDataFormat(&c_dfDIJoystick)) ||
       FAILED(device->SetCooperativeLevel(static_cast<HWND>(context),DISCL_FOREGROUND|DISCL_NONEXCLUSIVE))){
        closeDevice();return DIENUM_CONTINUE;
    }
    for(DWORD offset:{DIJOFS_X,DIJOFS_Y,DIJOFS_Z,DIJOFS_RZ}){
        DIPROPRANGE range{{sizeof(DIPROPRANGE),sizeof(DIPROPHEADER),offset,DIPH_BYOFFSET},0,65535};
        if(FAILED(device->SetProperty(DIPROP_RANGE,&range.diph))){closeDevice();return DIENUM_CONTINUE;}
    }
    return DIENUM_STOP;
}
static bool readPlayStation(HWND window,XINPUT_GAMEPAD &state){
    // Enumerate at first use and on hardware changes. Device discovery can
    // take longer than a frame and does not belong in the per-frame poll.
    if(devicesChanged.exchange(false) && !device){
        if(!api && FAILED(DirectInput8Create(GetModuleHandleW(nullptr),DIRECTINPUT_VERSION,
            IID_IDirectInput8W,reinterpret_cast<void **>(&api),nullptr)))return false;
        api->EnumDevices(DI8DEVCLASS_GAMECTRL,openController,window,DIEDFL_ATTACHEDONLY);
    }
    if(!device)return false;
    DIJOYSTATE input{};
    device->Poll();
    auto result=device->GetDeviceState(sizeof(input),&input);
    if(result==DIERR_INPUTLOST || result==DIERR_NOTACQUIRED){
        result=device->Acquire();
        if(result==DIERR_OTHERAPPHASPRIO)return false;
        if(SUCCEEDED(result)){
            device->Poll();
            result=device->GetDeviceState(sizeof(input),&input);
        }
    }
    if(result==DIERR_OTHERAPPHASPRIO)return false;
    if(FAILED(result)){closeDevice();devicesChanged.store(true);return false;}
    state=playStationState(input);
    return true;
}
}
