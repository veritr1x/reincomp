package main

import (
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestNativeSetupImportsBeforeDeploy(t *testing.T) {
	output := filepath.Join(t.TempDir(), "helper-commands")
	pipe, err := os.Create(output)
	if err != nil {
		t.Fatal(err)
	}
	defer pipe.Close()
	w := &resourceWindow{input: pipe, deploy: make(chan struct{}), startServices: make(chan struct{}, 1)}
	w.nativeSetup("setup-state")
	w.nativeSetup("setup-deploy")
	w.nativeSetup("setup-folder")
	w.nativeSetup("setup-choose")
	w.nativeSetup("setup-state")
	if w.deployed || !w.busy {
		t.Fatal("expected just one active file picker before resources are verified")
	}
	w.verified, w.busy = true, false
	w.nativeSetup("setup-state")
	w.nativeSetup("setup-deploy")
	w.nativeSetup("setup-deploy")
	w.nativeSetup("setup-state")
	select {
	case <-w.deploy:
	default:
		t.Fatal("verified resources did not deploy")
	}
	if len(w.startServices) != 1 || !w.deployed {
		t.Fatal("expected one service start")
	}
	got, err := os.ReadFile(output)
	if err != nil {
		t.Fatal(err)
	}
	want := "setup 1\t\nchoose-folder\nsetup 0\t\nsetup 3\tGame files are ready.\nsetup 0\tStarting the game…\n"
	if string(got) != want {
		t.Fatalf("native setup commands = %q, want %q", got, want)
	}
}

func TestServerPortPreference(t *testing.T) {
	root := t.TempDir()
	data := filepath.Join(root, "data")
	if err := os.Mkdir(data, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(data, "launcher.json"), []byte(`{"theme":"dark","updates":false}`), 0600); err != nil {
		t.Fatal(err)
	}
	master := filepath.Join(root, masterName)
	if err := os.WriteFile(master, nil, 0600); err != nil {
		t.Fatal(err)
	}
	menu, err := startLauncher(root, "", root, master, "", true, true, nil)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(menu.close)
	for _, tc := range []struct {
		body          string
		status, saved int
	}{
		{"", 200, 38080},
		{`{"assetPort":11945}`, 200, 11945},
		{`{"assetPort":8079}`, 400, 11945},
		{`{"assetPort":65536}`, 400, 11945},
		{`{"assetPort":8080.5}`, 400, 11945},
		{`{"assetPort":65535}`, 200, 65535},
		{`{"assetPort":8080}`, 200, 8080},
		{`{"assetPort":38080}`, 200, 38080},
	} {
		method := http.MethodPost
		if tc.body == "" {
			method = http.MethodGet
		}
		req, err := http.NewRequest(method, menu.homeURL+"preferences", strings.NewReader(tc.body))
		if err != nil {
			t.Fatal(err)
		}
		req.Header.Set("Origin", "http://"+req.URL.Host)
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		if resp.StatusCode == http.StatusOK {
			var prefs launcherPreferences
			if err := json.NewDecoder(resp.Body).Decode(&prefs); err != nil || prefs.AssetPort != tc.saved {
				t.Errorf("response for %q: port %d, error %v", tc.body, prefs.AssetPort, err)
			}
		}
		resp.Body.Close()
		prefs := readLauncherPreferences(data)
		if resp.StatusCode != tc.status || prefs.AssetPort != tc.saved || prefs.Theme != "dark" || prefs.Updates {
			t.Fatalf("%q: HTTP %d, preferences %+v", tc.body, resp.StatusCode, prefs)
		}
	}
}

func TestSavedAssetPortFormats(t *testing.T) {
	data := t.TempDir()
	for raw, want := range map[string]int{`"11945"`: 11945, "12080": 12080, `"bad"`: 38080, "65536": 38080} {
		if err := os.WriteFile(filepath.Join(data, "launcher.json"), []byte(`{"theme":"dark","assetPort":`+raw+`}`), 0600); err != nil {
			t.Fatal(err)
		}
		prefs := readLauncherPreferences(data)
		if prefs.AssetPort != want || prefs.Theme != "dark" {
			t.Errorf("stored port %s: got %+v, want port %d and dark theme", raw, prefs, want)
		}
	}
}

func TestDirectGameHasRunningLauncher(t *testing.T) {
	root := t.TempDir()
	master := filepath.Join(root, masterName)
	if err := os.WriteFile(master, nil, 0600); err != nil {
		t.Fatal(err)
	}
	menu, err := startLauncher(root, "", root, master, "", true, true, nil)
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
	for _, test := range []struct {
		action string
		want   int
	}{{"stop", http.StatusNoContent}, {"import-save", http.StatusConflict}, {"stop", http.StatusConflict}} {
		req, err := http.NewRequest(http.MethodPost, menu.homeURL+test.action, nil)
		if err != nil {
			t.Fatal(err)
		}
		req.Header.Set("Origin", "http://"+req.URL.Host)
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		resp.Body.Close()
		if resp.StatusCode != test.want || len(menu.stopServices) != 1 {
			t.Fatalf("%s after Deploy returned HTTP %d, queued stops %d", test.action, resp.StatusCode, len(menu.stopServices))
		}
	}
}
