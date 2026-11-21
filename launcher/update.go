package main

import (
	"bufio"
	"context"
	_ "embed"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"
)

//go:embed update_player.py
var updateHelper string

var updateHash = regexp.MustCompile("^[a-f0-9]{64}$")

type windowsRelease struct {
	Version string `json:"version"`
	Windows bool   `json:"windows"`
	Runtime string `json:"windowsRuntime"`
}

func newerVersion(next, current string) bool {
	if !releaseTag.MatchString(next) || !releaseTag.MatchString(current) {
		return false
	}
	parts := strings.Split(strings.TrimPrefix(current, "v"), ".")
	for i, n := range strings.Split(strings.TrimPrefix(next, "v"), ".") {
		a, err := strconv.ParseUint(n, 10, 32)
		b, other := strconv.ParseUint(parts[i], 10, 32)
		if err != nil || other != nil {
			return false
		}
		if a != b {
			return a > b
		}
	}
	return false
}

func installedWindowsVersion(root string) string {
	if raw, err := os.ReadFile(filepath.Join(root, "game", "windows-version.txt")); err == nil {
		if v := strings.TrimSpace(string(raw)); releaseTag.MatchString(v) {
			return strings.TrimPrefix(v, "v")
		}
	}
	// Older kits carry the mobile SDK version, which can precede the
	// Windows release that used it. This updater first shipped after 1.12.15.
	raw, _ := os.ReadFile(filepath.Join(root, "pods", "VERSION"))
	v := strings.TrimPrefix(strings.TrimSpace(string(raw)), "Bunker ")
	if newerVersion(v, "1.12.15") {
		return v
	}
	return "1.12.15"
}

func latestWindowsRelease(ctx context.Context) (windowsRelease, error) {
	var release windowsRelease
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, "https://veritr1x.github.io/bunker/version.json", nil)
	if err != nil {
		return release, err
	}
	client := http.Client{Timeout: 5 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		return release, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 || json.NewDecoder(io.LimitReader(resp.Body, 65536)).Decode(&release) != nil ||
		!release.Windows || !releaseTag.MatchString(release.Version) || !updateHash.MatchString(release.Runtime) {
		return windowsRelease{}, errors.New("The published Windows update information is unavailable.")
	}
	return release, nil
}

func canUpdatePortable(root, exe string) bool {
	return filepath.Clean(root) == filepath.Join(filepath.Dir(exe), ".bunker-player") &&
		regularFile(filepath.Join(root, "data", "game-sound.apk")) && regularFile(filepath.Join(root, "data", "original-master.bin.e"))
}

func startPlayerUpdate(ctx context.Context, root, exe string, release windowsRelease, progress func(string)) error {
	if !canUpdatePortable(root, exe) || !newerVersion(release.Version, installedWindowsVersion(root)) || !updateHash.MatchString(release.Runtime) {
		return errors.New("No newer compatible Windows release is available.")
	}
	stage, err := os.MkdirTemp(filepath.Dir(exe), ".bunker-update-")
	if err != nil {
		return err
	}
	candidate := filepath.Join(stage, "player.exe")
	started := false
	defer func() {
		if !started {
			os.RemoveAll(stage)
		}
	}()
	python := filepath.Join(root, "pods", "python", "python.exe")
	args := []string{"-I", "-B", "-u", "-X", "utf8", "-c", updateHelper}
	cmd := exec.CommandContext(ctx, python, append(args, "build", root, candidate, release.Version, release.Runtime)...)
	stop, err := cmd.StdinPipe()
	if err != nil {
		return err
	}
	defer stop.Close()
	// Let Python shut down the isolated browser and remove its temporary
	// profile if the player closes the launcher during a download.
	cmd.Cancel = stop.Close
	cmd.WaitDelay = 10 * time.Second
	hideHelperWindow(cmd)
	lines, err := cmd.StdoutPipe()
	if err != nil {
		return err
	}
	var diagnostic strings.Builder
	cmd.Stderr = &diagnostic
	if err = cmd.Start(); err != nil {
		return err
	}
	var digest string
	scanner := bufio.NewScanner(lines)
	for scanner.Scan() {
		line := scanner.Text()
		if hash, ok := strings.CutPrefix(line, "VERIFIED "); ok && updateHash.MatchString(hash) {
			digest = hash
		} else {
			progress(line)
		}
	}
	if err = cmd.Wait(); err != nil {
		return fmt.Errorf("Update failed: %s (%w)", strings.TrimSpace(diagnostic.String()), err)
	}
	if err = scanner.Err(); err != nil {
		return err
	}
	if digest == "" {
		return errors.New("The update did not pass verification.")
	}
	if err = ctx.Err(); err != nil {
		return err
	}
	// The running EXE is locked on Windows. Wait for this process to exit
	// before replacing it, including when it was launched with a custom name.
	applyArgs := append(args, "apply", root, candidate, exe, digest, strconv.Itoa(os.Getpid()))
	apply := exec.Command(python, append(applyArgs, os.Args[1:]...)...)
	hideHelperWindow(apply)
	if err = apply.Start(); err != nil {
		return err
	}
	started = true
	return apply.Process.Release()
}
