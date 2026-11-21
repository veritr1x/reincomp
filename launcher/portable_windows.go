package main

import (
	"fmt"
	"path/filepath"
	"syscall"
)

func init() {
	lockPlayerPackage = func(install string) (func(), error) {
		name, err := syscall.UTF16PtrFromString(filepath.Join(install, ".package-lock"))
		if err != nil {
			return nil, err
		}
		handle, err := syscall.CreateFile(name, syscall.GENERIC_READ|syscall.GENERIC_WRITE, syscall.FILE_SHARE_READ,
			nil, syscall.OPEN_ALWAYS, syscall.FILE_ATTRIBUTE_NORMAL, 0)
		if err != nil {
			return nil, fmt.Errorf("cannot open the portable player in %s: %w", install, err)
		}
		return func() { syscall.CloseHandle(handle) }, nil
	}
}
