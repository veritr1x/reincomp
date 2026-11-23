package main

import (
	"fmt"
	"os/exec"
	"strconv"
	"syscall"
	"time"
	"unsafe"
)

func init() {
	hideHelperWindow = func(cmd *exec.Cmd) { cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true} }
	closePlayerWindow = closeUnityWindow
	applyPlayerDisplay = func(pid int, size string, borderless, resize bool) error {
		width, height, err := renderDimensions(size)
		if err != nil {
			return err
		}
		w, _ := strconv.Atoi(width)
		h, _ := strconv.Atoi(height)
		var flags uintptr
		if borderless {
			flags |= 1
		}
		if resize {
			flags |= 2
		}
		send := syscall.NewLazyDLL("user32.dll").NewProc("SendMessageTimeoutW")
		// Bunker can accept a Save while Unity's splash HWND is visible but
		// before the player has installed its display message handler.
		for deadline := time.Now().Add(8 * time.Second); time.Now().Before(deadline); time.Sleep(100 * time.Millisecond) {
			if window := unityWindow(pid); window != 0 {
				var accepted uintptr
				result, _, _ := send.Call(window, 0x8000+0x371, flags, uintptr(uint32(w)<<16|uint32(h)),
					2, 400, uintptr(unsafe.Pointer(&accepted)))
				if result != 0 && accepted == 1 {
					return nil
				}
			}
		}
		return fmt.Errorf("display settings saved for next launch; the running player did not acknowledge the change")
	}
	displayMode = func() (int, int, int) {
		user := syscall.NewLazyDLL("user32.dll")
		caps := syscall.NewLazyDLL("gdi32.dll").NewProc("GetDeviceCaps")
		dc, _, _ := user.NewProc("GetDC").Call(0)
		if dc == 0 {
			return 0, 0, 0
		}
		defer user.NewProc("ReleaseDC").Call(0, dc)
		// DESKTOPHORZRES and DESKTOPVERTRES give physical pixels at any DPI.
		width, _, _ := caps.Call(dc, 118)
		height, _, _ := caps.Call(dc, 117)
		refresh, _, _ := caps.Call(dc, 116)
		return int(width), int(height), int(refresh)
	}
}

func closeUnityWindow(pid int) bool {
	window := unityWindow(pid)
	if window == 0 {
		return false
	}
	ok, _, _ := syscall.NewLazyDLL("user32.dll").NewProc("PostMessageW").Call(window, 0x0010, 0, 0)
	return ok != 0
}

func unityWindow(pid int) uintptr {
	user := syscall.NewLazyDLL("user32.dll")
	class, _ := syscall.UTF16PtrFromString("UnityWndClass")
	find := user.NewProc("FindWindowExW")
	for _, container := range []uintptr{playerParent.Load(), 0} {
		var after uintptr
		for {
			window, _, _ := find.Call(container, after, uintptr(unsafe.Pointer(class)), 0)
			if window == 0 {
				break
			}
			var owner uint32
			user.NewProc("GetWindowThreadProcessId").Call(window, uintptr(unsafe.Pointer(&owner)))
			if int(owner) == pid {
				parent, _, _ := user.NewProc("GetWindow").Call(window, 4) // GW_OWNER
				if container == 0 && parent != 0 {
					after = window
					continue
				}
				return window
			}
			after = window
		}
	}
	return 0
}
