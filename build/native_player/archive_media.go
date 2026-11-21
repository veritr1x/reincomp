// Build against the same Bunker server module as the APK. The desktop Archive
// calls these converters in place of the phone's Java NativeBridge.
package main

import (
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"lunar-tear/server/mobile"
)

func stageResources(mode, source, stage string) error {
	target := filepath.Join(stage, "revisions", "0")
	if mode == "stage-link" {
		raw, err := io.ReadAll(io.LimitReader(os.Stdin, 8193))
		if err != nil {
			return err
		}
		if len(raw) == 0 || len(raw) > 8192 {
			return fmt.Errorf("Enter an archive URL.")
		}
		result := make(chan string, 1)
		go func() { result <- mobile.ImportLink(string(raw), stage, -1) }()
		tick := time.NewTicker(time.Second)
		defer tick.Stop()
		for {
			select {
			case problem := <-result:
				if problem != "" {
					return fmt.Errorf("%s", problem)
				}
				fmt.Println("Verifying downloaded game files")
				return checkResourceFolder(target, "")
			case <-tick.C:
				fmt.Println(mobile.ImportProgress())
			}
		}
	}
	revision, err := resourceRevision(source)
	if err != nil {
		return err
	}
	if mode == "stage-in-place" {
		live := filepath.Join(filepath.Dir(stage), "assets", "revisions", "0")
		if rel, e := filepath.Rel(live, revision); e == nil && rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
			return fmt.Errorf("These files are already installed in this player. Choose the original extracted folder to use it in place.")
		}
	}
	rel, err := filepath.Rel(revision, target)
	if err == nil && rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
		return fmt.Errorf("Choose a folder outside the import staging directory.")
	}
	if mode == "stage-folder" {
		return checkResourceFolder(revision, target)
	}
	if err = checkResourceFolder(revision, ""); err != nil {
		return err
	}
	if err = os.MkdirAll(filepath.Dir(target), 0755); err != nil {
		return err
	}
	// Directory junctions work across local drives without Developer Mode.
	// Environment variables keep both paths out of PowerShell command syntax.
	cmd := exec.Command("powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
		"$ErrorActionPreference='Stop'; New-Item -ItemType Junction -Path $env:BUNKER_REVISION_LINK -Target $env:BUNKER_REVISION_SOURCE | Out-Null")
	cmd.Env = append(os.Environ(), "BUNKER_REVISION_LINK="+target, "BUNKER_REVISION_SOURCE="+revision)
	if out, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("Could not mount the folder in place: %s", strings.TrimSpace(string(out)))
	}
	return nil
}

func main() {
	args := os.Args[1:]
	if len(args) == 3 && (args[0] == "stage-folder" || args[0] == "stage-in-place" || args[0] == "stage-link") {
		if err := stageResources(args[0], args[1], args[2]); err != nil {
			fmt.Fprint(os.Stderr, err)
			os.Exit(1)
		}
		return
	}
	if len(args) == 2 && args[0] == "verify-resources" {
		if err := checkResourceArchive(args[1]); err != nil {
			fmt.Fprint(os.Stderr, err)
			os.Exit(1)
		}
		return
	}
	message := "Invalid Archive converter arguments"
	if len(args) >= 3 {
		switch args[0] {
		case "decode":
			if len(args) == 4 {
				if size, err := strconv.Atoi(args[3]); err == nil {
					message = mobile.Texture(args[1], args[2], size)
				}
			}
		case "sound":
			message = mobile.Audio(args[1], args[2])
		case "model":
			message = mobile.Model(args[1], args[2])
		case "motion":
			if len(args) == 4 {
				message = mobile.Motion(args[1], args[2], args[3])
			}
		case "scenario":
			message = mobile.Scenario(args[1], args[2])
		}
	}
	if message != "" {
		fmt.Fprint(os.Stderr, message)
		os.Exit(1)
	}
}
