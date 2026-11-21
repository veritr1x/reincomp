package main

import (
	"archive/zip"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"path"
	"sort"
	"strings"

	"github.com/bodgit/sevenzip"
)

// Names, uncompressed sizes and CRCs of the Global Android revision mounted
// by Lunar Tear. Changing the archive format or compression preserves the ID.
const globalResourceIndex = "f9a08cb736cd499659d6e924ad0b90feb34add759f4049cf35a2771b16bb78d4"

func checkResourceArchive(filename string) error {
	type entry struct {
		name string
		size uint64
		crc  uint32
	}
	var files []entry
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
		files = append(files, entry{name, size, crc})
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
		return fmt.Errorf("This archive does not contain the complete Global Android revision. Choose the original resource dump or its ZIP equivalent.")
	}
	return nil
}
