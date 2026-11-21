package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestInstalledWindowsRelease(t *testing.T) {
	root := t.TempDir()
	for _, folder := range []string{"pods", "game"} {
		if err := os.Mkdir(filepath.Join(root, folder), 0755); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(filepath.Join(root, "pods", "VERSION"), []byte("Bunker 1.12.14\n"), 0644); err != nil {
		t.Fatal(err)
	}
	if got := installedWindowsVersion(root); got != "1.12.15" {
		t.Fatal(got)
	}
	if err := os.WriteFile(filepath.Join(root, "game", "windows-version.txt"), []byte("1.12.19\n"), 0644); err != nil {
		t.Fatal(err)
	}
	current := installedWindowsVersion(root)
	if current != "1.12.19" || !newerVersion("1.12.100", current) || newerVersion("1.12.19", current) || newerVersion("1.12.9", current) || newerVersion("invalid", current) {
		t.Fatal("Unexpected update eligibility for", current)
	}
}
