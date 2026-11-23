package main

import (
	"bufio"
	"context"
	"crypto/sha256"
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
	Version, Bunker, Runtime, Executable string
}

func (r windowsRelease) installable() bool {
	return releaseTag.MatchString(r.Bunker) && updateHash.MatchString(r.Runtime) && updateHash.MatchString(r.Executable)
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

func latestWindowsRelease(ctx context.Context) (windowsRelease, error) {
	return checkWindowsRelease(ctx, version, &http.Client{Timeout: 5 * time.Second})
}

func readReleaseJSON(ctx context.Context, client *http.Client, url string, value any, digest string) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return err
	}
	resp, err := client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("Release information returned HTTP %d", resp.StatusCode)
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, 256<<10))
	if err != nil {
		return err
	}
	if digest != "" && fmt.Sprintf("%x", sha256.Sum256(body)) != digest {
		return errors.New("Windows runtime manifest checksum mismatch")
	}
	return json.Unmarshal(body, value)
}

func checkWindowsRelease(ctx context.Context, current string, client *http.Client) (windowsRelease, error) {
	var github struct {
		Tag               string `json:"tag_name"`
		Draft, Prerelease bool
	}
	if err := readReleaseJSON(ctx, client, "https://api.github.com/repos/veritr1x/reincomp/releases/latest", &github, ""); err != nil {
		return windowsRelease{}, err
	}
	if !strings.HasPrefix(github.Tag, "v") || !releaseTag.MatchString(github.Tag) || github.Draft || github.Prerelease {
		return windowsRelease{}, errors.New("Invalid reincomp release")
	}
	release := windowsRelease{Version: strings.TrimPrefix(github.Tag, "v")}
	if !newerVersion(release.Version, current) {
		return release, nil
	}
	// The Pages publisher and GitHub release can go live at different times.
	// Keep the release notes available while a matching rebuild is unavailable.
	var feed struct {
		Version, ReincompVersion, WindowsRuntime string
		Windows                                  bool
	}
	if err := readReleaseJSON(ctx, client, "https://veritr1x.github.io/bunker/version.json", &feed, ""); err != nil ||
		!feed.Windows || feed.ReincompVersion != release.Version || !updateHash.MatchString(feed.WindowsRuntime) {
		return release, nil
	}
	var lock struct {
		Portable struct{ BunkerVersion, SHA256 string }
	}
	if err := readReleaseJSON(ctx, client, "https://raw.githubusercontent.com/veritr1x/reincomp/"+github.Tag+"/release.lock.json", &lock, ""); err != nil ||
		!releaseTag.MatchString(feed.Version) || feed.Version != lock.Portable.BunkerVersion || !updateHash.MatchString(lock.Portable.SHA256) {
		return release, nil
	}
	var manifest struct{ SourceExeSHA256 string }
	if err := readReleaseJSON(ctx, client, "https://veritr1x.github.io/bunker/windows/runtime/runtime.json", &manifest, feed.WindowsRuntime); err == nil && manifest.SourceExeSHA256 == lock.Portable.SHA256 {
		release.Bunker, release.Runtime, release.Executable = feed.Version, feed.WindowsRuntime, lock.Portable.SHA256
	}
	return release, nil
}

func canUpdatePortable(root, exe string) bool {
	return filepath.Clean(root) == filepath.Join(filepath.Dir(exe), ".bunker-player") &&
		regularFile(filepath.Join(root, "data", "game-sound.apk")) && regularFile(filepath.Join(root, "data", "original-master.bin.e"))
}

func startPlayerUpdate(ctx context.Context, root, exe string, release windowsRelease, progress func(string)) error {
	if !canUpdatePortable(root, exe) || !newerVersion(release.Version, version) || !release.installable() {
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
	cmd := exec.CommandContext(ctx, python, append(args, "build", root, candidate, release.Bunker, release.Runtime, release.Executable)...)
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
	if digest != release.Executable {
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
