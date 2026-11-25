package main

import (
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func makePortable(t *testing.T, exe, version, extra string, corrupt bool) {
	t.Helper()
	var payload []byte
	var members []playerFile
	names := []string{"game/WindowsPlayer.exe", "server/bunker-server.exe", "data/import-master.bin.e"}
	if extra != "" {
		names = append(names, extra)
	}
	format := 1
	for _, name := range names {
		b := []byte(version + name)
		hash := fmt.Sprintf("%x", sha256.Sum256(b))
		member := playerFile{Path: name, Size: int64(len(b)), Chunks: []string{hash}}
		if name == "game/GuestRuntime.dll" {
			var buf bytes.Buffer
			w := gzip.NewWriter(&buf)
			if _, err := w.Write(b); err != nil {
				t.Fatal(err)
			}
			if err := w.Close(); err != nil {
				t.Fatal(err)
			}
			b = buf.Bytes()
			format = 2
			member.Compression = "gzip"
			member.StoredSize = int64(len(b))
		}
		if corrupt && name == names[len(names)-1] {
			b[len(b)-1] ^= 1
		}
		payload = append(payload, b...)
		members = append(members, member)
	}
	manifest, err := json.Marshal(map[string]any{"format": format, "files": members})
	if err != nil {
		t.Fatal(err)
	}
	footer := []byte(packageMagic)
	footer = binary.LittleEndian.AppendUint32(footer, uint32(len(manifest)))
	footer = binary.LittleEndian.AppendUint64(footer, uint64(len(payload)))
	all := append([]byte("MZ"), payload...)
	all = append(all, manifest...)
	if err = os.WriteFile(exe, append(all, footer...), 0600); err != nil {
		t.Fatal(err)
	}
}

func TestPortableInstallAndRetry(t *testing.T) {
	if runtime.GOOS != "windows" {
		t.Skip("uses Windows player instance locking")
	}
	outer := t.TempDir()
	exe := filepath.Join(outer, "Nier Reincarnation.exe")
	makePortable(t, exe, "v1", "", false)
	first, err := playerInstallation(exe)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := playerInstallation(exe); err == nil {
		first.close()
		t.Fatal("two concurrent installations")
	}
	first.close()
	save := filepath.Join(first.root, "data", "save")
	if err := os.WriteFile(save, []byte("keep"), 0600); err != nil {
		t.Fatal(err)
	}
	for _, tc := range []struct {
		path    string
		corrupt bool
	}{{"../outside", false}, {"data/save", false}, {"", true}, {"game/windowsplayer.exe", false}, {"game/GuestRuntime.dll", true}} {
		makePortable(t, exe, "v2", tc.path, tc.corrupt)
		if inst, err := playerInstallation(exe); err == nil {
			inst.close()
			t.Fatalf("accepted invalid package %+v", tc)
		}
		b, err := os.ReadFile(filepath.Join(first.root, "game", "WindowsPlayer.exe"))
		if err != nil || string(b) != "v1game/WindowsPlayer.exe" {
			t.Fatal("damaged previous installation", err)
		}
	}
	makePortable(t, exe, "v2", "game/GuestRuntime.dll", false)
	second, err := playerInstallation(exe)
	if err != nil {
		t.Fatal(err)
	}
	defer second.close()
	for name, want := range map[string]string{save: "keep", filepath.Join(second.root, "game", "WindowsPlayer.exe"): "v2game/WindowsPlayer.exe", filepath.Join(second.root, "game", "GuestRuntime.dll"): "v2game/GuestRuntime.dll"} {
		b, err := os.ReadFile(name)
		if err != nil || string(b) != want {
			t.Fatalf("%s: %q, %v", name, b, err)
		}
	}
}

func TestPortableInstallClearsAbandonedStaging(t *testing.T) {
	previous := lockPlayerPackage
	lockPlayerPackage = func(string) (func(), error) { return func() {}, nil }
	defer func() { lockPlayerPackage = previous }()
	outer := t.TempDir()
	exe := filepath.Join(outer, "Nier Reincarnation.exe")
	makePortable(t, exe, "v1", "", false)
	// A launcher that was closed mid-install leaves its staging folder behind.
	install := filepath.Join(outer, ".bunker-player")
	abandoned := filepath.Join(install, ".install-3287437195", "game")
	if err := os.MkdirAll(abandoned, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(install, ".owner"), []byte("Bunker portable player 1"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(abandoned, "WindowsPlayer.exe"), []byte("partial"), 0600); err != nil {
		t.Fatal(err)
	}
	inst, err := playerInstallation(exe)
	if err != nil {
		t.Fatal(err)
	}
	defer inst.close()
	items, err := os.ReadDir(install)
	if err != nil {
		t.Fatal(err)
	}
	for _, item := range items {
		if strings.HasPrefix(item.Name(), ".install-") {
			t.Fatalf("staging folder %s was left behind", item.Name())
		}
	}
	if b, err := os.ReadFile(filepath.Join(install, "game", "WindowsPlayer.exe")); err != nil || string(b) != "v1game/WindowsPlayer.exe" {
		t.Fatalf("installed player: %q, %v", b, err)
	}
}
