package main

import (
	"bufio"
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

var hideHelperWindow = func(*exec.Cmd) {}
var playerParent atomic.Uintptr

type launcherPreferences struct {
	Theme       string `json:"theme"`
	Updates     bool   `json:"updates"`
	AutoInstall bool   `json:"autoInstall"`
	Skipped     string `json:"skipped,omitempty"`
}

func readLauncherPreferences(data string) launcherPreferences {
	p := launcherPreferences{Theme: "system", Updates: true}
	b, err := os.ReadFile(filepath.Join(data, "launcher.json"))
	if err == nil {
		json.Unmarshal(b, &p)
	}
	if p.Theme != "dark" && p.Theme != "light" {
		p.Theme = "system"
	}
	return p
}

type resourceWindow struct {
	handle                                      string
	homeURL                                     string
	input                                       io.WriteCloser
	server                                      *http.Server
	done                                        chan struct{}
	cancel                                      context.CancelFunc
	mu                                          sync.Mutex
	status                                      string
	busy, verified, deployed, services, running bool
	deploy                                      chan struct{}
	restart                                     chan struct{}
	startServices, stopServices                 chan struct{}
	checkURL, checkToken                        string
	assetRoot                                   string
}

func (w *resourceWindow) send(line string) { fmt.Fprintln(w.input, line) }

// The caller holds mu and has checked that the game files are ready.
func (w *resourceWindow) launch(path string) {
	if !w.services {
		w.services = true
		w.startServices <- struct{}{}
	}
	if path != "" {
		w.send("program " + path)
		return
	}
	w.deployed = true
	w.status = "Starting the game…"
	close(w.deploy)
}

// A Win32 setup window uses the same pickers, import worker and deploy channel
// when WebView2 cannot initialize (for example, in a fresh Proton prefix).
func (w *resourceWindow) nativeSetup(action string) {
	w.mu.Lock()
	defer w.mu.Unlock()
	canChoose := !w.busy && !w.deployed && !w.services
	canDeploy := !w.busy && !w.deployed && w.verified
	switch action {
	case "setup-choose", "setup-folder":
		if canChoose {
			w.busy = true
			if action == "setup-folder" {
				w.send("choose-folder")
			} else {
				w.send("choose")
			}
		}
	case "setup-deploy":
		if canDeploy {
			w.launch("")
		}
	case "setup-state":
		flags := 0
		if canChoose {
			flags |= 1
		}
		if canDeploy {
			flags |= 2
		}
		message := strings.Join(strings.Fields(w.status), " ")
		if message == "" && canDeploy {
			message = "Game files are ready."
		}
		w.send(fmt.Sprintf("setup %d\t%s", flags, message))
	}
}

func (w *resourceWindow) close() {
	w.cancel()
	w.server.Close()
	if w.input != nil {
		playerParent.Store(0)
		w.input.Close()
		<-w.done
	}
}

// Only the local WebView receives this random URL. The file picker returns
// the original path, so even a 20 GB resource dump stays on disk.
func startLauncher(root, server, assets, master, dump string, ready, inGameOnly bool) (*resourceWindow, error) {
	if ready && !regularFile(master) {
		master = filepath.Join(assets, "assets", "release", masterName)
	}
	if !regularFile(master) {
		return nil, fmt.Errorf("The Windows player is missing its packaged Global master data.")
	}
	// A direct game launch needs the template only if the player opens Ctrl+B.
	resourcePage := sync.OnceValues(func() ([]byte, error) {
		render := exec.Command(filepath.Join(root, "pods", "python", "python.exe"), "-I", "-X", "utf8", filepath.Join(root, "game", "resource_setup.py"), root, assets, version)
		hideHelperWindow(render)
		return render.Output()
	})
	if !inGameOnly {
		if _, err := resourcePage(); err != nil {
			return nil, fmt.Errorf("could not open Bunker's launcher template: %w", err)
		}
	}
	listener, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		return nil, err
	}
	transferred := false
	defer func() {
		if !transferred {
			listener.Close()
		}
	}()
	token := make([]byte, 24)
	if _, err = rand.Read(token); err != nil {
		return nil, err
	}
	prefix := "/" + hex.EncodeToString(token) + "/"
	addr := listener.Addr().String()
	ctx, cancel := context.WithCancel(context.Background())
	w := &resourceWindow{homeURL: "http://" + addr + prefix, done: make(chan struct{}), deploy: make(chan struct{}), cancel: cancel, verified: ready, assetRoot: assets,
		deployed: inGameOnly, services: inGameOnly, running: inGameOnly}
	w.restart = make(chan struct{})
	exe, _ := os.Executable()
	portableUpdate := canUpdatePortable(root, exe)
	var available windowsRelease
	var updating bool
	updateError := filepath.Join(root, "data", "update-error.txt")
	if msg, e := os.ReadFile(updateError); e == nil {
		w.status = "Update failed: " + string(msg)
		os.Remove(updateError)
	}
	w.startServices = make(chan struct{}, 1)
	w.stopServices = make(chan struct{}, 1)
	manual := filepath.Join(root, "data", "manual-deploy")
	save := filepath.Join(root, "data", "server", "game.db")
	opened := make(chan string, 1)
	managed := filepath.Join(root, "data", "assets")
	lastLink := filepath.Join(root, "data", "resource-link.txt")
	importFile := func(mode, path string) {
		w.mu.Lock()
		w.busy = true
		current := w.assetRoot
		w.status = "Checking game files"
		w.mu.Unlock()
		helper := filepath.Join(root, "game", "archive-media.exe")
		progress := func(s string) { w.mu.Lock(); w.status = s; w.mu.Unlock() }
		useMaster := master
		if live := filepath.Join(current, "assets", "release", masterName); regularFile(live) {
			useMaster = live
		}
		lock, err := lockResourceFiles(managed)
		if err == nil {
			defer lock.Close()
			err = recoverResourceRevision(managed)
		}
		if err == nil && mode == "picked" {
			verify := exec.CommandContext(ctx, helper, "verify-resources", path)
			hideHelperWindow(verify)
			var out []byte
			out, err = verify.CombinedOutput()
			if err != nil && len(out) > 0 {
				err = fmt.Errorf("%s", strings.TrimSpace(string(out)))
			}
			if err == nil {
				err = importArchive(ctx, server, managed, useMaster, path, progress)
			}
		} else if err == nil {
			err = importResourceSource(ctx, helper, server, managed, useMaster, mode, path, progress)
		}
		w.mu.Lock()
		w.busy = false
		if err == nil {
			w.assetRoot = managed
			w.verified = true
			w.status = "Game files are ready."
		} else {
			w.verified = resourcesReady(current)
			w.status = err.Error()
		}
		w.mu.Unlock()
	}
	w.server = &http.Server{ReadHeaderTimeout: 5 * time.Second, Handler: http.HandlerFunc(func(out http.ResponseWriter, r *http.Request) {
		if r.Host != addr {
			http.NotFound(out, r)
			return
		}
		if r.Method == http.MethodGet && r.URL.Path == "/static/css/automata.css" {
			http.ServeFile(out, r, filepath.Join(root, "pods", "app", "web", "static", "css", "automata.css"))
			return
		}
		if r.Method == http.MethodGet && r.URL.Path == "/bunker/state" {
			out.Header().Set("Content-Type", "application/json")
			io.WriteString(out, `{"live":false}`)
			return
		}
		if !strings.HasPrefix(r.URL.Path, prefix) {
			http.NotFound(out, r)
			return
		}
		out.Header().Set("Cache-Control", "no-store")
		out.Header().Set("Referrer-Policy", "no-referrer")
		out.Header().Set("X-Frame-Options", "DENY")
		action := strings.TrimPrefix(r.URL.Path, prefix)
		switch action {
		case "preferences":
			w.mu.Lock()
			defer w.mu.Unlock()
			data := filepath.Join(root, "data")
			p := readLauncherPreferences(data)
			if r.Method == http.MethodPost {
				if r.Header.Get("Origin") != "http://"+addr {
					http.Error(out, "Forbidden", 403)
					return
				}
				var change struct {
					Theme       *string
					Updates     *bool
					AutoInstall *bool
					Skipped     *string
				}
				if json.NewDecoder(http.MaxBytesReader(out, r.Body, 1024)).Decode(&change) != nil {
					http.Error(out, "Invalid preferences", 400)
					return
				}
				if change.Theme != nil {
					switch *change.Theme {
					case "system", "dark", "light":
						p.Theme = *change.Theme
					default:
						http.Error(out, "Invalid theme", 400)
						return
					}
				}
				if change.Updates != nil {
					p.Updates = *change.Updates
				}
				if change.AutoInstall != nil {
					p.AutoInstall = *change.AutoInstall
				}
				if change.Skipped != nil {
					if *change.Skipped != "" && !releaseTag.MatchString(*change.Skipped) {
						http.Error(out, "Invalid release version", 400)
						return
					}
					p.Skipped = *change.Skipped
				}
				raw, _ := json.Marshal(p)
				path := filepath.Join(data, "launcher.json")
				err := os.WriteFile(path+".part", raw, 0600)
				if err == nil {
					err = os.Rename(path+".part", path)
				}
				if err != nil {
					http.Error(out, err.Error(), 500)
					return
				}
			} else if r.Method != http.MethodGet {
				http.Error(out, "Method", 405)
				return
			}
			out.Header().Set("Content-Type", "application/json")
			json.NewEncoder(out).Encode(p)
		case "update":
			if r.Method != http.MethodGet {
				http.Error(out, "Method", 405)
				return
			}
			release, err := latestWindowsRelease(r.Context())
			if err != nil {
				http.Error(out, "Unable to check for Windows updates.", 502)
				return
			}
			w.mu.Lock()
			available = release
			w.mu.Unlock()
			out.Header().Set("Content-Type", "application/json")
			json.NewEncoder(out).Encode(map[string]any{"version": release.Version, "current": version, "available": newerVersion(release.Version, version), "portable": portableUpdate && release.installable()})
		case "":
			if r.Method != http.MethodGet {
				http.Error(out, "Method", 405)
				return
			}
			page, err := resourcePage()
			if err != nil {
				http.Error(out, "Could not render the Bunker launcher.", http.StatusInternalServerError)
				return
			}
			out.Header().Set("Content-Type", "text/html; charset=utf-8")
			out.Write(page)
		case "state":
			if r.Method != http.MethodGet {
				http.Error(out, "Method", 405)
				return
			}
			w.mu.Lock()
			defer w.mu.Unlock()
			out.Header().Set("Content-Type", "application/json")
			var size int64
			if s, err := os.Stat(save); err == nil {
				size = s.Size()
			}
			json.NewEncoder(out).Encode(map[string]any{"message": w.status, "ready": w.verified, "busy": w.busy, "deployed": w.deployed,
				"services": w.services, "running": w.running, "saveBytes": size, "archiveBytes": archiveSize(root), "autoDeploy": !regularFile(manual),
				"portableUpdate": portableUpdate, "updating": updating})
		case "log":
			if r.Method != http.MethodGet {
				http.Error(out, "Method", 405)
				return
			}
			out.Header().Set("Content-Type", "text/plain; charset=utf-8")
			f, err := os.Open(filepath.Join(root, "data", "server.log"))
			if os.IsNotExist(err) {
				io.WriteString(out, "Lunar Tear has not written a log yet.")
				return
			}
			if err != nil {
				http.Error(out, "Unable to read the Lunar Tear log.", 500)
				return
			}
			defer f.Close()
			io.Copy(out, f)
		case "last-resource-link":
			if r.Method != http.MethodGet {
				http.Error(out, "Method not allowed", 405)
				return
			}
			out.Header().Set("Cache-Control", "no-store")
			if previous, e := os.ReadFile(lastLink); e == nil {
				out.Write(previous)
			}
		case "install-update", "choose", "choose-folder", "choose-in-place", "resource-link", "deploy", "program", "auto-deploy", "import-save", "export-save", "import-master", "stop", "check":
			if r.Method != http.MethodPost || r.Header.Get("Origin") != "http://"+addr {
				http.Error(out, "Forbidden", 403)
				return
			}
			w.mu.Lock()
			defer w.mu.Unlock()
			if w.busy || (w.deployed && action != "stop") {
				http.Error(out, "Busy", 409)
				return
			}
			switch action {
			case "install-update":
				if w.services || !portableUpdate || !available.installable() || !newerVersion(available.Version, version) {
					http.Error(out, "Stop Lunar Tear and check for a newer Windows release first.", 409)
					return
				}
				w.busy, updating = true, true
				w.status = "Preparing Windows update…"
				go func(release windowsRelease) {
					progress := func(msg string) { w.mu.Lock(); w.status = msg; w.mu.Unlock() }
					err := startPlayerUpdate(ctx, root, exe, release, progress)
					w.mu.Lock()
					defer w.mu.Unlock()
					if err != nil {
						w.status = err.Error()
						w.busy, updating = false, false
						return
					}
					w.status = "Restarting into the updated player…"
					close(w.restart)
				}(available)
			case "stop":
				if !w.running {
					http.Error(out, "Lunar Tear is not running.", 409)
					return
				}
				w.busy = true
				w.status = "Stopping Lunar Tear…"
				w.stopServices <- struct{}{}
			case "check":
				if w.checkURL == "" {
					http.Error(out, "Lunar Tear is not running.", 409)
					return
				}
				req, e := http.NewRequestWithContext(ctx, http.MethodGet, w.checkURL+"/status", nil)
				if e != nil {
					http.Error(out, e.Error(), 500)
					return
				}
				req.Header.Set("X-Bunker-Token", w.checkToken)
				client := http.Client{Timeout: 3 * time.Second}
				resp, e := client.Do(req)
				if e != nil {
					http.Error(out, e.Error(), 502)
					return
				}
				defer resp.Body.Close()
				var result struct{ State string }
				if resp.StatusCode != 200 || json.NewDecoder(resp.Body).Decode(&result) != nil || result.State == "" {
					http.Error(out, "Lunar Tear did not report its state.", 502)
					return
				}
				w.status = "Lunar Tear · " + result.State
			case "auto-deploy":
				var err error
				if regularFile(manual) {
					err = os.Remove(manual)
				} else {
					err = os.WriteFile(manual, nil, 0600)
				}
				if err != nil {
					http.Error(out, err.Error(), 500)
					return
				}
			case "choose", "choose-folder", "choose-in-place", "resource-link", "import-save", "export-save", "import-master":
				if w.services {
					http.Error(out, "Close and reopen the launcher to change files while Lunar Tear is stopped.", 409)
					return
				}
				if action == "export-save" && !regularFile(save) {
					http.Error(out, "There is no save to export yet.", 409)
					return
				}
				if action == "resource-link" {
					data, e := io.ReadAll(http.MaxBytesReader(out, r.Body, 8192))
					link := strings.TrimSpace(string(data))
					u, parseErr := url.Parse(link)
					if e != nil || parseErr != nil || (u.Scheme != "https" && u.Scheme != "http") || u.Hostname() == "" || u.User != nil {
						http.Error(out, "Enter an HTTP or HTTPS archive link.", 400)
						return
					}
					if e = os.WriteFile(lastLink, []byte(link), 0600); e != nil {
						http.Error(out, e.Error(), 500)
						return
					}
					w.busy = true
					go importFile("stage-link", link)
					break
				}
				w.busy = true
				w.send(action)
			case "deploy", "program":
				if !w.verified {
					http.Error(out, "Choose and verify the resources first.", 409)
					return
				}
				path := r.URL.Query().Get("path")
				if action == "program" && !programPath(path) {
					http.Error(out, "Unknown Pod Programs page.", 400)
					return
				}
				if action == "deploy" {
					path = ""
				}
				w.launch(path)
			}
			out.WriteHeader(http.StatusNoContent)
		default:
			http.NotFound(out, r)
		}
	})}
	if inGameOnly {
		transferred = true
		go w.server.Serve(listener)
		return w, nil
	}
	cmd := exec.Command(filepath.Join(root, "game", "resource-window.exe"), root, w.homeURL)
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		cancel()
		return nil, err
	}
	w.input, err = cmd.StdinPipe()
	if err != nil {
		cancel()
		return nil, err
	}
	cmd.Stderr = os.Stderr
	if err = cmd.Start(); err != nil {
		cancel()
		w.input.Close()
		return nil, err
	}
	go w.server.Serve(listener)
	go func() {
		lines := bufio.NewScanner(stdout)
		for lines.Scan() {
			action, value, _ := strings.Cut(lines.Text(), "\t")
			switch action {
			case "setup-state", "setup-choose", "setup-folder", "setup-deploy":
				w.nativeSetup(action)
			case "window":
				if handle, err := strconv.ParseUint(value, 10, 64); err == nil && handle != 0 {
					opened <- value
				}
			case "picked", "picked-folder", "picked-in-place":
				mode := map[string]string{"picked": "picked", "picked-folder": "stage-folder", "picked-in-place": "stage-in-place"}[action]
				go importFile(mode, value)
			case "import-master":
				w.mu.Lock()
				current := w.assetRoot
				w.mu.Unlock()
				load := exec.CommandContext(ctx, server, "import", "-assets", current, "-master", value)
				hideHelperWindow(load)
				out, err := load.CombinedOutput()
				w.mu.Lock()
				if err == nil {
					w.status = "Master data imported."
					w.verified = resourcesReady(current)
				} else {
					w.status = strings.TrimSpace(string(out))
				}
				w.busy = false
				w.mu.Unlock()
			case "import-save", "export-save":
				var err error
				if action == "import-save" {
					err = saves(root, "", value)
				} else {
					err = saves(root, value, "")
				}
				w.mu.Lock()
				if action == "import-save" {
					w.status = "Save restored. Your previous save is in data/server.before-import."
				} else {
					w.status = "Save backup complete."
				}
				if err != nil {
					w.status = err.Error()
				}
				w.busy = false
				w.mu.Unlock()
			case "cancel":
				w.mu.Lock()
				w.busy = false
				w.mu.Unlock()
			}
		}
		cancel()
		cmd.Wait()
		close(w.done)
	}()
	select {
	case w.handle = <-opened:
	case <-w.done:
		w.close()
		return nil, fmt.Errorf("The Bunker resource window could not open.")
	// Wine can spend over 30 seconds initializing its desktop on the first run.
	case <-time.After(90 * time.Second):
		w.close()
		return nil, fmt.Errorf("The Bunker resource window timed out.")
	}
	if dump != "" && !ready {
		w.mu.Lock()
		w.busy = true
		w.mu.Unlock()
		go importFile("picked", dump)
	}
	transferred = true
	handle, _ := strconv.ParseUint(w.handle, 10, 64)
	playerParent.Store(uintptr(handle))
	return w, nil
}

var releaseTag = regexp.MustCompile(`^v?[0-9]+\.[0-9]+\.[0-9]+$`)

func archiveSize(root string) int64 {
	if stat, err := os.Stat(filepath.Join(root, "data", "archive", "archive.db")); err == nil {
		return stat.Size()
	}
	return 0
}

func programPath(path string) bool {
	switch path {
	case "/", "/bunker/archive", "/patcher", "/users", "/backups", "/display", "/language", "/gacha", "/mods", "/gift", "/decks", "/items", "/costumes", "/weapons", "/upgrades", "/memoirs", "/story":
		return true
	}
	return false
}
