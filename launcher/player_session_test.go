package main

import (
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
)

func calendarChild(dir string) func() (*exec.Cmd, error) {
	return func() (*exec.Cmd, error) {
		cmd := exec.Command(os.Args[0], "-test.run=^TestCalendarChild$")
		cmd.Env = append(os.Environ(), "REINCOMP_CALENDAR_TEST_DIR="+dir)
		return cmd, nil
	}
}

func TestStopLunarTearClosesPlayerAndReturnsToLauncher(t *testing.T) {
	dir := t.TempDir()
	menu := &resourceWindow{stopServices: make(chan struct{}, 1)}
	started := make(chan int, 1)
	session := &playerSession{started: func(pid int) { started <- pid }}
	oldClose := closePlayerWindow
	closePlayerWindow = func(pid int) bool {
		return os.WriteFile(filepath.Join(dir, strconv.Itoa(pid)), nil, 0600) == nil
	}
	defer func() { closePlayerWindow = oldClose }()
	done := make(chan error, 1)
	go func() { done <- session.play(calendarChild(dir), menu) }()
	select {
	case <-started:
		menu.stopServices <- struct{}{}
	case <-time.After(5 * time.Second):
		t.Fatal("player did not start")
	}
	select {
	case err := <-done:
		if !errors.Is(err, errReturnToLauncher) {
			t.Fatalf("Stop returned %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("Stop did not finish the game session")
	}
}

// A game that hangs in its shutdown is ended, and the stop still completes.
func TestStopLunarTearEndsAPlayerThatDoesNotClose(t *testing.T) {
	dir := t.TempDir()
	menu := &resourceWindow{stopServices: make(chan struct{}, 1)}
	started := make(chan int, 1)
	session := &playerSession{started: func(pid int) { started <- pid }}
	oldClose, oldWait := closePlayerWindow, playerCloseWait
	closePlayerWindow, playerCloseWait = func(int) bool { return true }, 200*time.Millisecond
	defer func() { closePlayerWindow, playerCloseWait = oldClose, oldWait }()
	done := make(chan error, 1)
	go func() { done <- session.play(calendarChild(dir), menu) }()
	var pid int
	select {
	case pid = <-started:
		menu.stopServices <- struct{}{}
	case <-time.After(5 * time.Second):
		t.Fatal("player did not start")
	}
	select {
	case err := <-done:
		if !errors.Is(err, errReturnToLauncher) {
			t.Fatalf("Stop returned %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("Stop did not finish while the game ignored its close request")
	}
	if process, err := os.FindProcess(pid); err == nil && process.Signal(syscall.Signal(0)) == nil {
		process.Kill()
		t.Fatal("the unresponsive game was left running")
	}
}

func TestFailedUpdateCanRetryWhileGameIsRunning(t *testing.T) {
	dir := t.TempDir()
	update := &launcherUpdate{release: windowsRelease{Version: "1.0.0"}}
	menu := &resourceWindow{busy: true, update: update, stopServices: make(chan struct{}, 1)}
	started := make(chan int, 1)
	session := &playerSession{started: func(pid int) { started <- pid }}
	oldClose := closePlayerWindow
	attempts := 0
	closePlayerWindow = func(pid int) bool {
		attempts++
		return attempts > 1 && os.WriteFile(filepath.Join(dir, strconv.Itoa(pid)), nil, 0600) == nil
	}
	defer func() { closePlayerWindow = oldClose }()
	done := make(chan error, 1)
	go func() { done <- session.play(calendarChild(dir), menu) }()
	select {
	case <-started:
	case err := <-done:
		t.Fatalf("could not start the player: %v", err)
	case <-time.After(5 * time.Second):
		t.Fatal("player did not start")
	}
	menu.stopServices <- struct{}{}
	for deadline := time.Now().Add(5 * time.Second); ; {
		menu.mu.Lock()
		pending, busy, status := menu.update, menu.busy, menu.status
		menu.mu.Unlock()
		if !busy {
			if pending != nil || !strings.Contains(status, "game window is not ready to close") {
				t.Fatalf("failed close left update %v, status %q", pending, status)
			}
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("launcher stayed busy after the game refused to close")
		}
		time.Sleep(10 * time.Millisecond)
	}
	menu.mu.Lock()
	menu.busy, menu.update = true, update
	menu.mu.Unlock()
	menu.stopServices <- struct{}{}
	select {
	case reason := <-done:
		if !errors.Is(reason, errReturnToLauncher) || reason != update || update.playerExit != nil {
			t.Fatalf("retry lost the update after closing the game: %v", reason)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("retry did not close the game")
	}
}

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
	oldClose := closePlayerWindow
	closePlayerWindow = func(pid int) bool {
		return os.WriteFile(filepath.Join(dir, strconv.Itoa(pid)), nil, 0600) == nil
	}
	defer func() { closePlayerWindow = oldClose }()
	s := &playerSession{requests: make(chan playerRequest), editorGone: make(chan struct{})}
	done := make(chan error, 1)
	go func() { done <- s.play(calendarChild(dir), nil) }()
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
