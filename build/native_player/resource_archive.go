package main

import (
	"archive/zip"
	"context"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"io"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strings"
	"sync/atomic"
	"time"

	"github.com/bodgit/sevenzip"
	"golang.org/x/sync/errgroup"
)

// Names, uncompressed sizes and CRCs of the Global Android revision mounted
// by Lunar Tear. Changing the archive format or compression preserves the ID.
const globalResourceIndex = "f9a08cb736cd499659d6e924ad0b90feb34add759f4049cf35a2771b16bb78d4"

type resourceEntry struct {
	name string
	size uint64
	crc  uint32
}

func checkResourceArchive(filename string) error {
	var files []resourceEntry
	add := func(name string, size uint64, crc uint32, mode os.FileMode) error {
		if mode.IsDir() {
			return nil
		}
		name = "/" + strings.ReplaceAll(name, "\\", "/")
		i := strings.Index(name, "/revisions/0/")
		if i < 0 {
			return nil
		}
		name = name[i+1:]
		if !mode.IsRegular() || path.Clean(name) != name {
			return fmt.Errorf("invalid resource path %q", name)
		}
		files = append(files, resourceEntry{name, size, crc})
		return nil
	}
	f, err := os.Open(filename)
	if err != nil {
		return err
	}
	defer f.Close()
	stat, err := f.Stat()
	if err != nil {
		return err
	}
	var magic [6]byte
	if _, err = io.ReadFull(f, magic[:]); err != nil {
		return err
	}
	if string(magic[:]) == "7z\xbc\xaf\x27\x1c" {
		archive, err := sevenzip.NewReader(f, stat.Size())
		if err != nil {
			return err
		}
		for _, item := range archive.File {
			if err = add(item.Name, item.UncompressedSize, item.CRC32, item.Mode()); err != nil {
				return err
			}
		}
	} else {
		archive, err := zip.NewReader(f, stat.Size())
		if err != nil {
			return err
		}
		for _, item := range archive.File {
			if err = add(item.Name, item.UncompressedSize64, item.CRC32, item.Mode()); err != nil {
				return err
			}
		}
	}
	return checkResourceIndex(files)
}

func checkResourceIndex(files []resourceEntry) error {
	sort.Slice(files, func(i, j int) bool { return files[i].name < files[j].name })
	h := sha256.New()
	var fields [12]byte
	for _, file := range files {
		io.WriteString(h, file.name)
		h.Write([]byte{0})
		binary.LittleEndian.PutUint64(fields[:], file.size)
		binary.LittleEndian.PutUint32(fields[8:], file.crc)
		h.Write(fields[:])
	}
	if fmt.Sprintf("%x", h.Sum(nil)) != globalResourceIndex {
		return fmt.Errorf("These game files do not contain the complete Global Android revision.")
	}
	return nil
}

// Android accepts the folder containing revisions/0. Also recognize the
// installed server root or revision 0 itself when chosen in Explorer.
func resourceRevision(folder string) (string, error) {
	root, err := filepath.Abs(folder)
	if err != nil {
		return "", err
	}
	candidates := []string{filepath.Join(root, "revisions", "0"), filepath.Join(root, "assets", "revisions", "0")}
	if filepath.Base(root) == "0" && filepath.Base(filepath.Dir(root)) == "revisions" {
		candidates = append(candidates, root)
	}
	for _, candidate := range candidates {
		for _, leaf := range []string{"android", ""} {
			if info, err := os.Stat(filepath.Join(candidate, leaf, "list.bin")); err == nil && info.Mode().IsRegular() {
				return filepath.EvalSymlinks(candidate)
			}
		}
	}
	return "", fmt.Errorf("Choose the extracted folder containing revisions/0.")
}

func checkResourceFolder(revision, copyTo string) error {
	const readers = 6
	var results [readers][]resourceEntry
	var count atomic.Int64
	group, ctx := errgroup.WithContext(context.Background())
	jobs := make(chan string, readers)
	for i := range results {
		group.Go(func() error {
			buffer := make([]byte, 256*1024)
			for rel := range jobs {
				entry, err := readResourceFile(revision, rel, copyTo, buffer)
				if err != nil {
					return err
				}
				results[i] = append(results[i], entry)
				count.Add(1)
			}
			return nil
		})
	}
	nextReport := time.Now().Add(time.Second)
	walkErr := filepath.WalkDir(revision, func(filename string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(revision, filename)
		if err != nil {
			return err
		}
		if d.IsDir() {
			if copyTo != "" {
				return os.MkdirAll(filepath.Join(copyTo, rel), 0755)
			}
			return nil
		}
		if !d.Type().IsRegular() {
			return fmt.Errorf("Unexpected resource file %s", rel)
		}
		if time.Now().After(nextReport) {
			fmt.Printf("Checking game files · %d files\n", count.Load())
			nextReport = time.Now().Add(time.Second)
		}
		select {
		case jobs <- rel:
			return nil
		case <-ctx.Done():
			return ctx.Err()
		}
	})
	close(jobs)
	if err := group.Wait(); err != nil {
		return err
	}
	if walkErr != nil {
		return walkErr
	}
	var files []resourceEntry
	for _, entries := range results {
		files = append(files, entries...)
	}
	return checkResourceIndex(files)
}

func readResourceFile(revision, rel, copyTo string, buffer []byte) (resourceEntry, error) {
	in, err := os.Open(filepath.Join(revision, rel))
	if err != nil {
		return resourceEntry{}, err
	}
	defer in.Close()
	checksum := crc32.NewIEEE()
	var dest io.Writer = checksum
	var out *os.File
	if copyTo != "" {
		out, err = os.OpenFile(filepath.Join(copyTo, rel), os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0644)
		if err != nil {
			return resourceEntry{}, err
		}
		dest = io.MultiWriter(out, checksum)
	}
	n, err := io.CopyBuffer(dest, in, buffer)
	if out != nil {
		closeErr := out.Close()
		if err == nil {
			err = closeErr
		}
	}
	return resourceEntry{"revisions/0/" + filepath.ToSlash(rel), uint64(n), checksum.Sum32()}, err
}
