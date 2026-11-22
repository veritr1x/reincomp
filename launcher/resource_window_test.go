package main

import (
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"testing"
)

func TestDirectGameHasRunningLauncher(t *testing.T) {
	root := t.TempDir()
	master := filepath.Join(root, masterName)
	if err := os.WriteFile(master, nil, 0600); err != nil {
		t.Fatal(err)
	}
	menu, err := startLauncher(root, "", root, master, "", true, true)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(menu.close)
	resp, err := http.Get(menu.homeURL + "state")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var state struct{ Ready, Running, Deployed bool }
	if err := json.NewDecoder(resp.Body).Decode(&state); err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != http.StatusOK || !state.Ready || !state.Running || !state.Deployed {
		t.Fatalf("direct launch state = %+v (HTTP %d)", state, resp.StatusCode)
	}
	req, err := http.NewRequest(http.MethodPost, menu.homeURL+"deploy", nil)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("Origin", "http://"+req.URL.Host)
	resp, err = http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusConflict {
		t.Fatalf("second Deploy returned HTTP %d", resp.StatusCode)
	}
}
