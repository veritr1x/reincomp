package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"testing"
)

func TestFailedResourceReplacementKeepsPlayableRevision(t *testing.T) {
	root := t.TempDir()
	current, previous := revisionPaths(root)
	if err := os.MkdirAll(current, 0755); err != nil {
		t.Fatal(err)
	}
	marker := filepath.Join(current, "list.bin")
	if err := os.WriteFile(marker, []byte("installed catalog"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := installResourceRevision(root, filepath.Join(root, "missing")); err == nil {
		t.Fatal("missing import succeeded")
	}
	if buf, err := os.ReadFile(marker); err != nil || string(buf) != "installed catalog" {
		t.Fatalf("original catalog: %q, %v", buf, err)
	}
	if err := os.Rename(current, previous); err != nil {
		t.Fatal(err)
	}
	if err := recoverPendingImport(root); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(marker); err != nil {
		t.Fatal(err)
	}
}

func TestReplacingMountedRevisionPreservesSource(t *testing.T) {
	root := t.TempDir()
	source := t.TempDir()
	if err := os.WriteFile(filepath.Join(source, "original"), []byte("source"), 0644); err != nil {
		t.Fatal(err)
	}
	current, _ := revisionPaths(root)
	if err := os.MkdirAll(filepath.Dir(current), 0755); err != nil {
		t.Fatal(err)
	}
	if runtime.GOOS == "windows" {
		cmd := exec.Command("powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
			"$ErrorActionPreference = 'Stop'; New-Item -ItemType Junction -Path $env:TEST_MOUNT -Target $env:TEST_SOURCE | Out-Null")
		cmd.Env = append(os.Environ(), "TEST_MOUNT="+current, "TEST_SOURCE="+source)
		if output, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("mounting test revision: %v (%s)", err, output)
		}
	} else if err := os.Symlink(source, current); err != nil {
		t.Fatal(err)
	}
	newRevision := filepath.Join(root, "candidate")
	if err := os.Mkdir(newRevision, 0755); err != nil {
		t.Fatal(err)
	}
	if err := installResourceRevision(root, newRevision); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(source, "original")); err != nil {
		t.Fatal(err)
	}
}
