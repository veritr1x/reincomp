package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"testing"
	"time"
)

// The helper exits on its own close signal, like a player handling WM_CLOSE.
func TestCalendarChild(t *testing.T) {
	dir := os.Getenv("REINCOMP_CALENDAR_TEST_DIR")
	if dir == "" {
		return
	}
	pid := strconv.Itoa(os.Getpid())
	if err := os.WriteFile(filepath.Join(dir, "player"), []byte(pid), 0600); err != nil {
		t.Fatal(err)
	}
	for end := time.Now().Add(10 * time.Second); time.Now().Before(end); {
		if _, err := os.Stat(filepath.Join(dir, pid)); err == nil {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("test player never received its close signal")
}

func TestPlayerRestartsAroundCalendarEdit(t *testing.T) {
	dir := t.TempDir()
	makePlayer := func() (*exec.Cmd, error) {
		cmd := exec.Command(os.Args[0], "-test.run=^TestCalendarChild$")
		cmd.Env = append(os.Environ(), "REINCOMP_CALENDAR_TEST_DIR="+dir)
		return cmd, nil
	}
	oldClose := closePlayerWindow
	closePlayerWindow = func(pid int) bool {
		return os.WriteFile(filepath.Join(dir, strconv.Itoa(pid)), nil, 0600) == nil
	}
	defer func() { closePlayerWindow = oldClose }()
	s := &playerSession{requests: make(chan playerRequest), editorGone: make(chan struct{})}
	done := make(chan error, 1)
	go func() { done <- s.play(makePlayer) }()
	waitPlayer := func(previous string) string {
		t.Helper()
		for end := time.Now().Add(5 * time.Second); time.Now().Before(end); {
			pid, err := os.ReadFile(filepath.Join(dir, "player"))
			if err == nil && len(pid) != 0 && string(pid) != previous {
				return string(pid)
			}
			time.Sleep(10 * time.Millisecond)
		}
		t.Fatal("replacement player did not start")
		return ""
	}
	request := func(stop bool) {
		t.Helper()
		r := playerRequest{stop: stop, result: make(chan error, 1)}
		select {
		case s.requests <- r:
		case <-time.After(5 * time.Second):
			t.Fatal("launcher did not receive the editor request")
		}
		select {
		case err := <-r.result:
			if err != nil {
				t.Fatal(err)
			}
		case <-time.After(5 * time.Second):
			t.Fatal("launcher did not complete the editor request")
		}
	}
	first := waitPlayer("")
	request(true)
	request(false)
	second := waitPlayer(first)
	request(true)
	close(s.editorGone)
	third := waitPlayer(second)
	if err := os.WriteFile(filepath.Join(dir, third), nil, 0600); err != nil {
		t.Fatal(err)
	}
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("a normal game close did not end the launcher")
	}
}
