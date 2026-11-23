package main

import (
	"archive/zip"
	"fmt"
	"io"
	"io/fs"
	"net/http"
	"os"
	"regexp"
	"strings"
	"time"
)

var logFilename = regexp.MustCompile(`(?i)\.log(\.[0-9]+)?(\.gz)?$`)

func diagnosticFile(dir, name string) bool {
	if logFilename.MatchString(name) {
		return true
	}
	if dir == "data" && (name == "update-error.txt" || name == "upscale-log.txt") {
		return true
	}
	if dir == "game" {
		switch name {
		case "frame-cpu.tsv", "gc-pauses.tsv", "managed-profile.tsv", "render-profile.tsv",
			"field-inflation.tsv", "field-load-queue.tsv", "field-metadata.tsv", "managed-enums.tsv",
			"movie-metadata.tsv", "posted-work-samples.tsv", "slow-callbacks.tsv", "story-work.tsv",
			"transition-samples.tsv", "weapon-query-check.tsv", "frame-release.csv", "render-submit.csv",
			"render-thread.csv", "boundary.txt", "native-root-test.txt", "enum-requests.txt", "field-store-check.txt",
			"liveness-check.txt", "managed-methods.txt":
			return true
		}
	}
	return false
}

func writeLogArchive(out io.Writer, root string) error {
	install, err := os.OpenRoot(root)
	if err != nil {
		return err
	}
	defer install.Close()
	z := zip.NewWriter(out)
	for _, dir := range []string{".", "logs", "data", "data/logs", "data/server", "data/pods", "data/pods/patcher", "game", "game/logs", "game/Crashes"} {
		stat, err := install.Lstat(dir)
		if os.IsNotExist(err) {
			continue
		}
		if err != nil {
			return err
		}
		if !stat.IsDir() {
			continue
		}
		nested := dir == "logs" || dir == "data/logs" || dir == "game/logs" || dir == "game/Crashes"
		err = fs.WalkDir(install.FS(), dir, func(name string, entry fs.DirEntry, err error) error {
			if os.IsNotExist(err) {
				return nil
			}
			if err != nil {
				return err
			}
			if name == dir {
				return nil
			}
			if entry.IsDir() && !nested {
				return fs.SkipDir
			}
			if !entry.Type().IsRegular() || !diagnosticFile(dir, entry.Name()) {
				return nil
			}
			file, err := install.Open(name)
			if os.IsNotExist(err) { // A log can rotate while the game is running.
				return nil
			}
			if err != nil {
				return err
			}
			defer file.Close()
			return addLogFile(z, name, file)
		})
		if err != nil {
			return err
		}
	}
	info, err := z.CreateHeader(&zip.FileHeader{Name: "info.txt", Method: zip.Deflate, Modified: time.Now()})
	if err != nil {
		return err
	}
	bunker, _ := fs.ReadFile(install.FS(), "pods/VERSION")
	bunkerVersion, _, _ := strings.Cut(string(bunker), "\n")
	if _, err = fmt.Fprintf(info, "reincomp %s\n%s\nExported %s\n\n"+
		"game/frame-state.log records frame timings and memory at five-second intervals during gameplay.\n"+
		"Match memory and gpuMemory rows to timings by frame number. Memory is in MiB. -1 means unavailable.\n"+
		"Compare GPU usage with its budget for the same adapter and segment. Local memory can be system RAM on integrated GPUs.\n",
		version, strings.TrimSpace(bunkerVersion), time.Now().Format(time.RFC3339)); err != nil {
		return err
	}
	return z.Close()
}

func addLogFile(z *zip.Writer, name string, file *os.File) error {
	stat, err := file.Stat()
	if err != nil {
		return err
	}
	if !stat.Mode().IsRegular() {
		return nil
	}
	entry, err := z.CreateHeader(&zip.FileHeader{Name: name, Method: zip.Deflate, Modified: stat.ModTime()})
	if err == nil {
		// Read the bytes present at export time even if the writer keeps appending.
		_, err = io.Copy(entry, io.LimitReader(file, stat.Size()))
	}
	return err
}

func serveLogArchive(out http.ResponseWriter, r *http.Request, root string) {
	if r.Method != http.MethodGet {
		http.Error(out, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	f, err := os.CreateTemp("", "reincomp-logs-*.zip")
	if err == nil {
		defer os.Remove(f.Name())
		defer f.Close()
		err = writeLogArchive(f, root)
	}
	if err != nil {
		http.Error(out, "Could not export logs: "+err.Error(), http.StatusInternalServerError)
		return
	}
	name := "reincomp-logs-" + time.Now().Format("20060102-150405") + ".zip"
	out.Header().Set("Content-Type", "application/zip")
	out.Header().Set("Content-Disposition", `attachment; filename="`+name+`"`)
	http.ServeContent(out, r, name, time.Time{}, f)
}
