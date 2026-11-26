package main

import (
	"errors"
	"strings"
	"testing"
)

func TestGameStoppedNamesAHang(t *testing.T) {
	code := func(status uint32) func(error) (uint32, bool) {
		return func(error) (uint32, bool) { return status, true }
	}
	hung := gameStopped(errors.New("exit status 0xcfffffff"), code(hungExitStatus), `C:\game\data\game.log`).Error()
	if !strings.Contains(hung, "stopped responding") || strings.Contains(hung, "game.log") {
		t.Fatalf("a hang should be named and not point at the empty game.log: %s", hung)
	}
	other := gameStopped(errors.New("exit status 1"), code(1), `C:\game\data\game.log`).Error()
	if !strings.Contains(other, "exit status 1") || !strings.Contains(other, "game.log") {
		t.Fatalf("other failures keep the status and the log: %s", other)
	}
	if strings.Contains(failureText(hung), "could not start") || strings.Contains(failureText(other), "could not start") {
		t.Fatal("a game that ran did start")
	}
	if !strings.Contains(failureText("invalid player data"), "could not start") {
		t.Fatal("install failures keep the heading")
	}
}
