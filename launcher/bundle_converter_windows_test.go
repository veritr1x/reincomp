package main

import (
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// Use the Python interpreter and scripts from a prepared player. All state for
// this test is written in a new directory, including the deliberately bad input.
func TestConverterSurvivesEditorStartupFailure(t *testing.T) {
	root := os.Getenv("REINCOMP_TEST_PLAYER_ROOT")
	if root == "" {
		t.Skip("set REINCOMP_TEST_PLAYER_ROOT to a prepared Windows player")
	}
	data := t.TempDir()
	stop, err := startBundleConverter(root, data)
	if err != nil {
		t.Fatal(err)
	}
	defer stop()
	state := filepath.Join(data, "editor.json")
	stopEditor := servePods(root, filepath.Join(root, "pods"), data, filepath.Join(data, "missing-assets"),
		"test", "http://127.0.0.1:1", state, nil)
	defer stopEditor()

	waitFile := func(path string) []byte {
		t.Helper()
		for end := time.Now().Add(20 * time.Second); time.Now().Before(end); {
			if b, err := os.ReadFile(path); err == nil && len(b) > 0 {
				return b
			}
			time.Sleep(20 * time.Millisecond)
		}
		t.Fatalf("timed out waiting for %s", filepath.Base(path))
		return nil
	}
	var editor struct{ Error string }
	if err := json.Unmarshal(waitFile(state), &editor); err != nil || editor.Error == "" {
		t.Fatal("expected missing editor assets to prevent Pod Programs from starting")
	}
	// Checked rejection of a request proves the converter is still serving the
	// player after the editor has exited, without requiring an original game asset.
	raw := []byte("invalid bundle")
	key := fmt.Sprintf("%x-00000000", sha256.Sum256(raw))
	cache := filepath.Join(data, "desktop-bundles-v2")
	request := filepath.Join(cache, "pending", key+".request")
	if err := os.MkdirAll(filepath.Dir(request), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(request+".part", raw, 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Rename(request+".part", request); err != nil {
		t.Fatal(err)
	}
	if msg := string(waitFile(filepath.Join(cache, key+".error"))); !strings.Contains(msg, "Unexpected file header") {
		t.Fatalf("unexpected conversion result: %s", msg)
	}
}
