// Compiled into the pinned Bunker mobile package through the Windows Go overlay.
package mobile

import (
	"archive/zip"
	"context"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync/atomic"
	"unsafe"

	"golang.org/x/sync/errgroup"
	"golang.org/x/sys/windows"
)

// MEMORYSTATUSEX from the Windows SDK.
type memoryStatus struct {
	length, load                                                  uint32
	total, available                                              uint64
	pageFile, freePageFile, virtual, freeVirtual, extendedVirtual uint64
}

var memoryStatusEx = windows.NewLazySystemDLL("kernel32.dll").NewProc("GlobalMemoryStatusEx")

func systemMemory() memoryStatus {
	var status memoryStatus
	status.length = uint32(unsafe.Sizeof(status))
	if ok, _, _ := memoryStatusEx.Call(uintptr(unsafe.Pointer(&status))); ok == 0 {
		return memoryStatus{}
	}
	return status
}

func init() {
	physicalMemory = func() int64 { return int64(systemMemory().total) }
}

func solidBlockWorkers() int {
	// Each independent LZMA2 decoder needs a 64 MiB dictionary for this dump.
	limit := 4
	if available := systemMemory().available; available >= 4<<30 {
		limit = 8
	} else if available != 0 && available < 1<<30 {
		limit = 2
	}
	return min(limit, runtime.GOMAXPROCS(0))
}

type resourceWriter struct {
	buffer  []byte
	parent  string
	checked func(string, int64, uint32)
}

func newResourceWriter(checked func(string, int64, uint32)) *resourceWriter {
	return &resourceWriter{buffer: make([]byte, 256*1024), checked: checked}
}

func (w *resourceWriter) ensureParent(target string) error {
	parent := filepath.Dir(target)
	if w.parent == parent {
		return nil
	}
	if err := os.MkdirAll(parent, 0700); err != nil {
		return err
	}
	w.parent = parent
	return nil
}

type resourceZipEntry struct {
	rel  string
	file *zip.File
}

func writeResourceZip(stage string, entries []resourceZipEntry, opt extractOptions) error {
	if len(entries) == 0 {
		return nil
	}
	// Two writers must never target the same Windows path.
	seen := make(map[string]bool, len(entries))
	for _, entry := range entries {
		key := strings.ToLower(filepath.Clean(entry.rel))
		if seen[key] || !filepath.IsLocal(entry.rel) || strings.Contains(entry.rel, ":") {
			return fmt.Errorf("Invalid or duplicate resource path %q", entry.rel)
		}
		seen[key] = true
	}
	const batch = 16
	workers := min(16, runtime.GOMAXPROCS(0), (len(entries)+batch-1)/batch)
	if opt.link != nil {
		// The HTTP reader prefetches consecutive bytes and resumes completed entries.
		workers = 1
	}
	var next atomic.Int64
	group, ctx := errgroup.WithContext(context.Background())
	for range workers {
		group.Go(func() error {
			writer := newResourceWriter(opt.checked)
			for ctx.Err() == nil {
				start := int(next.Add(batch)) - batch
				if start >= len(entries) {
					break
				}
				for _, entry := range entries[start:min(start+batch, len(entries))] {
					if err := writer.writeEntry(stage, entry.rel, entry.file.Open, opt.link != nil, entry.file.CRC32); err != nil {
						return err
					}
				}
			}
			return nil
		})
	}
	return group.Wait()
}
