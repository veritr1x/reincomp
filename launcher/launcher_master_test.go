package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestLauncherMasterFallsBackToThePackagedCopy(t *testing.T) {
	root, assets := t.TempDir(), t.TempDir()
	packaged := filepath.Join(root, "data", "import-master.bin.e")
	live := filepath.Join(assets, "assets", "release", masterName)
	for _, name := range []string{packaged, live} {
		if err := os.MkdirAll(filepath.Dir(name), 0755); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(packaged, []byte("packaged"), 0600); err != nil {
		t.Fatal(err)
	}
	// Back in the launcher after a stop, before any resources were imported.
	if got := launcherMaster(root, assets, "", false); got != packaged {
		t.Fatalf("no imported master: got %q, want the packaged copy", got)
	}
	// Marked ready, but the imported master has gone.
	if got := launcherMaster(root, assets, "", true); got != packaged {
		t.Fatalf("missing imported master: got %q, want the packaged copy", got)
	}
	if err := os.WriteFile(live, []byte("live"), 0600); err != nil {
		t.Fatal(err)
	}
	if got := launcherMaster(root, assets, "", true); got != live {
		t.Fatalf("imported master present: got %q", got)
	}
	if got := launcherMaster(root, assets, live, false); got != live {
		t.Fatalf("a given master is kept: got %q", got)
	}
}
