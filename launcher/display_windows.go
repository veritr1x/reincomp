package main

import (
	"fmt"
	"os/exec"
	"strconv"
	"syscall"
	"unsafe"
)

func init() {
	hideHelperWindow = func(cmd *exec.Cmd) { cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true} }
	closePlayerWindow = closeUnityWindow
	resizePlayerWindow = func(pid int, size string) error {
		width, height, err := renderDimensions(size)
		if err != nil {
			return err
		}
		w, _ := strconv.Atoi(width)
		h, _ := strconv.Atoi(height)
		window := unityWindow(pid)
		if window != 0 {
			var accepted uintptr
			// Wait only for the window procedure to queue Unity's main-thread change.
			result, _, _ := syscall.NewLazyDLL("user32.dll").NewProc("SendMessageTimeoutW").Call(
				window, 0x8000+0x36e, uintptr(w), uintptr(h), 2, 1500, uintptr(unsafe.Pointer(&accepted)))
			if result != 0 && accepted == 1 {
				return nil
			}
		}
		return fmt.Errorf("saved %s for next launch; the running player did not acknowledge the resize", size)
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
