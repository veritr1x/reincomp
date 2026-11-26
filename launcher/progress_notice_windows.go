package main

import (
	"runtime"
	"syscall"
	"unsafe"
)

func init() {
	openProgressWindow = func(text string) (func(string), func()) {
		user := syscall.NewLazyDLL("user32.dll")
		kernel := syscall.NewLazyDLL("kernel32.dll")
		create, destroy := user.NewProc("CreateWindowExW"), user.NewProc("DestroyWindow")
		getMessage, translate, dispatch := user.NewProc("GetMessageW"), user.NewProc("TranslateMessage"), user.NewProc("DispatchMessageW")
		metrics, setText, postThread := user.NewProc("GetSystemMetrics"), user.NewProc("SetWindowTextW"), user.NewProc("PostThreadMessageW")
		sendMessage, stockObject := user.NewProc("SendMessageW"), syscall.NewLazyDLL("gdi32.dll").NewProc("GetStockObject")
		utf16 := func(s string) uintptr {
			p, _ := syscall.UTF16PtrFromString(s)
			return uintptr(unsafe.Pointer(p))
		}
		type window struct{ label, thread uintptr }
		ready := make(chan window, 1)
		go func() {
			// A window belongs to the thread that created it, which must also pump its messages.
			runtime.LockOSThread()
			defer runtime.UnlockOSThread()
			const width, height = 440, 120
			const overlapped, caption, visible, child = 0x00000000, 0x00C00000, 0x10000000, 0x40000000
			const center, centerImage, noPrefix = 0x1, 0x200, 0x80
			screenW, _, _ := metrics.Call(0)
			screenH, _, _ := metrics.Call(1)
			// The built-in dialog class needs no window procedure of our own.
			frame, _, _ := create.Call(0, utf16("#32770"), utf16("Nier Reincarnation"), overlapped|caption|visible,
				(screenW-width)/2, (screenH-height)/2, width, height, 0, 0, 0, 0)
			label, _, _ := create.Call(0, utf16("STATIC"), utf16(text), child|visible|center|centerImage|noPrefix,
				10, 10, width-36, height-60, frame, 0, 0, 0)
			const defaultGUIFont, setFont = 17, 0x0030
			font, _, _ := stockObject.Call(defaultGUIFont)
			sendMessage.Call(label, setFont, font, 1)
			thread, _, _ := kernel.NewProc("GetCurrentThreadId").Call()
			ready <- window{label, thread}
			var message [48]byte
			for frame != 0 {
				got, _, _ := getMessage.Call(uintptr(unsafe.Pointer(&message[0])), 0, 0, 0)
				if int32(got) <= 0 {
					break
				}
				translate.Call(uintptr(unsafe.Pointer(&message[0])))
				dispatch.Call(uintptr(unsafe.Pointer(&message[0])))
			}
			if frame != 0 {
				destroy.Call(frame)
			}
		}()
		shown := <-ready
		const quit = 0x0012
		return func(s string) {
				if shown.label != 0 {
					setText.Call(shown.label, utf16(s))
				}
			}, func() {
				postThread.Call(shown.thread, quit, 0, 0)
			}
	}
}
