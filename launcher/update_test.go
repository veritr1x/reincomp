package main

import (
	"context"
	"crypto/sha256"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"testing"
	"time"
)

type releaseTransport map[string]string

func (bodies releaseTransport) RoundTrip(r *http.Request) (*http.Response, error) {
	body, ok := bodies[r.URL.String()]
	if !ok {
		return nil, fmt.Errorf("unavailable: %s", r.URL)
	}
	return &http.Response{StatusCode: 200, Body: io.NopCloser(strings.NewReader(body)), Header: make(http.Header)}, nil
}

func TestUpdateWaitsForLunarTearExit(t *testing.T) {
	update := &launcherUpdate{release: windowsRelease{Version: "1.0.0"}, serverDone: make(chan error, 1)}
	window := &resourceWindow{busy: true, services: true, running: true, update: update, restart: make(chan struct{})}
	if window.stopped(errStoppedFromLauncher, nil) != update || window.stopped(errReturnToLauncher, errors.New("player test exit")) != update {
		t.Fatal("lost the accepted release during shutdown")
	}
	done := make(chan struct{})
	go func() {
		window.installUpdate(context.Background(), "", "", update)
		close(done)
	}()
	select {
	case <-done:
		t.Fatal("started the update before Lunar Tear exited")
	case <-time.After(50 * time.Millisecond):
	}
	update.serverDone <- errors.New("test exit status")
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("update did not handle the server exit")
	}
	if window.busy || window.running || window.services || window.update != nil ||
		!strings.Contains(window.status, "Lunar Tear exited with an error") || !strings.Contains(window.status, "game exited with an error") {
		t.Fatalf("failed shutdown left the launcher in an invalid state: %q", window.status)
	}
	select {
	case <-window.restart:
		t.Fatal("restart after failed shutdown")
	default:
	}
}

func TestWindowsUpdateChannel(t *testing.T) {
	const api = "https://api.github.com/repos/veritr1x/reincomp/releases/latest"
	const feed = "https://veritr1x.github.io/bunker/version.json"
	const manifest = "https://veritr1x.github.io/bunker/windows/runtime/runtime.json"
	const lock = "https://raw.githubusercontent.com/veritr1x/reincomp/v0.2.6/release.lock.json"
	exe := strings.Repeat("a", 64)
	runtime := `{"sourceExeSHA256":"` + exe + `"}`
	sha := fmt.Sprintf("%x", sha256.Sum256([]byte(runtime)))
	bodies := releaseTransport{api: `{"tag_name":"v0.2.6"}`, manifest: runtime,
		lock: `{"portable":{"bunkerVersion":"1.12.21","sha256":"` + exe + `"}}`}
	client := &http.Client{Transport: bodies}
	check := func(current string, install bool) {
		t.Helper()
		r, err := checkWindowsRelease(context.Background(), current, client)
		if err != nil || r.Version != "0.2.6" || r.installable() != install {
			t.Fatalf("current %s, release %+v, error %v", current, r, err)
		}
	}
	bodies[feed] = `{"version":"1.12.20","windows":true,"windowsRuntime":"` + sha + `"}`
	check("0.2.5", false) // Legacy Bunker-only feed.
	bodies[feed] = `{"version":"1.12.21","reincompVersion":"0.2.6","windows":true,"windowsRuntime":"` + sha + `"}`
	check("0.2.5", true)
	check("0.2.6", false)
	check("0.2.7", false)
	bodies[lock] = strings.Replace(bodies[lock], exe, strings.Repeat("b", 64), 1)
	check("0.2.5", false)
	bodies[manifest] += " "
	check("0.2.5", false)
	delete(bodies, feed)
	check("0.2.5", false)
	bodies[api] = `{"tag_name":"v0.2.7","prerelease":true}`
	if _, err := checkWindowsRelease(context.Background(), "0.2.5", client); err == nil {
		t.Fatal("accepted a prerelease")
	}
	if !newerVersion("0.2.10", "0.2.6") || newerVersion("0.2.5", "0.2.6") || newerVersion("invalid", "0.2.6") {
		t.Fatal("invalid ordering")
	}
}
