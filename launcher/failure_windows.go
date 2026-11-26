package main

import (
	"syscall"
	"unsafe"
)

func init() {
	showFailure = func(message string) bool {
		console, _, _ := syscall.NewLazyDLL("kernel32.dll").NewProc("GetConsoleWindow").Call()
		if console != 0 {
			return false
		}
		text, err := syscall.UTF16PtrFromString(failureText(message))
		if err != nil {
			return false
		}
		title, _ := syscall.UTF16PtrFromString("Nier Reincarnation")
		const iconError, setForeground = 0x10, 0x10000
		syscall.NewLazyDLL("user32.dll").NewProc("MessageBoxW").Call(0, uintptr(unsafe.Pointer(text)), uintptr(unsafe.Pointer(title)), iconError|setForeground)
		return true
	}
}
