package main

import (
	"errors"
	"fmt"
	"os/exec"
	"strings"
)

// Windows ends a process whose window the user closed while it was not
// responding with this status.
const hungExitStatus = 0xcfffffff

// gameStopped describes a game that ended with an error. A game that Windows
// closed for not responding wrote nothing to its output, so the message names
// the logs that do record what it was doing.
func gameStopped(e error, exitCode func(error) (uint32, bool), gameLog string) error {
	if code, ok := exitCode(e); ok && code == hungExitStatus {
		return errors.New("Windows closed the game because it had stopped responding (exit status 0xcfffffff). " +
			"To report it, start the game again, open Bunker with Ctrl+B and choose ⋮ → Export logs.")
	}
	return fmt.Errorf("The game stopped: %v. See %s.", e, gameLog)
}

func processExitCode(e error) (uint32, bool) {
	var exit *exec.ExitError
	if !errors.As(e, &exit) {
		return 0, false
	}
	return uint32(exit.ExitCode()), true
}

// failureText is what the failure window shows. A game that ran and then
// stopped did start, so only the other failures get the heading.
func failureText(message string) string {
	if strings.HasPrefix(message, "The game stopped") || strings.HasPrefix(message, "Windows closed the game") {
		return message
	}
	return "NieR Re[in]carnation could not start.\n\n" + message
}
