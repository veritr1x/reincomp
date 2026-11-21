package main

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"syscall"
)

func init() {
	lockResourceFiles = func(root string) (io.Closer, error) {
		if err := os.MkdirAll(root, 0755); err != nil {
			return nil, err
		}
		name := filepath.Join(root, ".resource-import.lock")
		path, err := syscall.UTF16PtrFromString(name)
		if err != nil {
			return nil, err
		}
		handle, err := syscall.CreateFile(path, syscall.GENERIC_READ|syscall.GENERIC_WRITE, 0, nil, syscall.OPEN_ALWAYS, syscall.FILE_ATTRIBUTE_NORMAL, 0)
		if err != nil {
			return nil, fmt.Errorf("Could not lock game files for import: %w", err)
		}
		return os.NewFile(uintptr(handle), name), nil
	}
}
