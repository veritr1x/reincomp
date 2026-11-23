package main

import (
	"archive/zip"
	"bytes"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestInGameLogDownload(t *testing.T) {
	root := t.TempDir()
	logs := []string{"game/frame-state.log", "game/runtime.log", "game/managed-profile.tsv", "data/player.log", "data/server.log.1",
		"data/fsr31.log", "data/bundle-converter.log", "data/pods/patcher/last-patch.log", "game/Crashes/run-1/error.log"}
	files := append([]string{masterName, "data/update-error.txt", "data/server/game.db", "data/server/auth.key", "data/assets/unrelated.log", "data/resource-link.txt"}, logs...)
	for _, name := range files {
		file := filepath.Join(root, name)
		if err := os.MkdirAll(filepath.Dir(file), 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(file, []byte(name), 0600); err != nil {
			t.Fatal(err)
		}
	}
	menu, err := startLauncher(root, "", root, filepath.Join(root, masterName), "", true, true)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(menu.close)
	resp, err := http.Get(menu.homeURL + "logs.zip")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 || resp.Header.Get("Content-Type") != "application/zip" || !strings.Contains(resp.Header.Get("Content-Disposition"), ".zip") {
		t.Fatalf("download: %s %v", resp.Status, resp.Header)
	}
	raw, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatal(err)
	}
	z, err := zip.NewReader(bytes.NewReader(raw), int64(len(raw)))
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range logs {
		f, err := z.Open(name)
		if err != nil {
			t.Fatal(err)
		}
		b, err := io.ReadAll(f)
		f.Close()
		if err != nil || string(b) != name {
			t.Fatalf("bad data for %s: %v", name, err)
		}
	}
	if _, err := z.Open("info.txt"); err != nil {
		t.Fatal(err)
	}
	if _, err := z.Open("data/last-update-error.log"); err != nil {
		t.Fatal(err)
	}
	if len(z.File) != len(logs)+2 {
		t.Fatalf("unexpected ZIP entry count: %d", len(z.File))
	}
}
