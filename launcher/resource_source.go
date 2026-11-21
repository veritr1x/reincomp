package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

var lockResourceFiles = func(string) (io.Closer, error) { return io.NopCloser(strings.NewReader("")), nil }

func revisionPaths(root string) (string, string) {
	parent := filepath.Join(root, "assets", "revisions")
	return filepath.Join(parent, "0"), filepath.Join(parent, ".previous-0")
}

// Remove only the mount itself when the previous selection was used in place.
func removeRevision(name string) error {
	info, err := os.Lstat(name)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return os.Remove(name)
	}
	return os.RemoveAll(name)
}

func recoverResourceRevision(root string) error {
	current, previous := revisionPaths(root)
	if _, err := os.Lstat(previous); errors.Is(err, os.ErrNotExist) {
		return nil
	} else if err != nil {
		return err
	}
	if _, err := os.Lstat(current); errors.Is(err, os.ErrNotExist) {
		return os.Rename(previous, current)
	} else if err != nil {
		return err
	}
	return removeRevision(previous)
}

func recoverPendingImport(root string) error {
	_, previous := revisionPaths(root)
	if _, err := os.Lstat(previous); errors.Is(err, os.ErrNotExist) {
		return nil
	} else if err != nil {
		return err
	}
	lock, err := lockResourceFiles(root)
	if err != nil {
		return err
	}
	defer lock.Close()
	return recoverResourceRevision(root)
}

func installResourceRevision(root, staged string) error {
	if err := recoverResourceRevision(root); err != nil {
		return err
	}
	current, previous := revisionPaths(root)
	if err := os.MkdirAll(filepath.Dir(current), 0755); err != nil {
		return err
	}
	replacing := true
	if err := os.Rename(current, previous); errors.Is(err, os.ErrNotExist) {
		replacing = false
	} else if err != nil {
		return err
	}
	if err := os.Rename(staged, current); err != nil {
		if replacing {
			if restore := os.Rename(previous, current); restore != nil {
				return errors.Join(err, restore)
			}
		}
		return err
	}
	return removeRevision(previous)
}

func importResourceSource(ctx context.Context, helper, server, root, master, mode, source string, progress func(string)) error {
	if err := os.MkdirAll(root, 0755); err != nil {
		return err
	}
	var stage string
	var err error
	if mode == "stage-link" {
		stage = filepath.Join(root, ".resource-download")
		progress("Downloading game files")
	} else {
		stage, err = os.MkdirTemp(root, ".resource-folder-")
		if err != nil {
			return err
		}
		defer os.RemoveAll(stage)
		progress("Verifying game files")
	}
	input := source
	if mode == "stage-link" {
		input = "-"
	}
	cmd := exec.CommandContext(ctx, helper, mode, input, stage)
	if mode == "stage-link" {
		cmd.Stdin = strings.NewReader(source)
	}
	hideHelperWindow(cmd)
	out, err := cmd.StdoutPipe()
	if err != nil {
		return err
	}
	var problem strings.Builder
	cmd.Stderr = &problem
	if err = cmd.Start(); err != nil {
		return err
	}
	lines := bufio.NewScanner(out)
	for lines.Scan() {
		var state struct{ Done, Total int64 }
		if json.Unmarshal(lines.Bytes(), &state) == nil {
			if state.Total > 0 {
				progress(fmt.Sprintf("Downloading game files · %d%%", state.Done*100/state.Total))
			}
		} else {
			progress(lines.Text())
		}
	}
	if err = cmd.Wait(); err != nil {
		if ctx.Err() != nil {
			return ctx.Err()
		}
		if problem.Len() != 0 {
			return errors.New(strings.TrimSpace(problem.String()))
		}
		return err
	}
	if err = lines.Err(); err != nil {
		return err
	}
	currentMaster := filepath.Join(root, "assets", "release", masterName)
	if !strings.EqualFold(filepath.Clean(master), currentMaster) {
		load := exec.CommandContext(ctx, server, "import", "-assets", root, "-master", master)
		hideHelperWindow(load)
		if out, err := load.CombinedOutput(); err != nil {
			return fmt.Errorf("Importing master data: %w (%s)", err, strings.TrimSpace(string(out)))
		}
	}
	if !regularFile(currentMaster) {
		return errors.New("The verified master data is missing.")
	}
	if err = installResourceRevision(root, filepath.Join(stage, "revisions", "0")); err != nil {
		return err
	}
	if mode == "stage-link" {
		_ = os.RemoveAll(stage)
	}
	return nil
}
