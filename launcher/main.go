// reincomp starts the native game: it prepares the player's own game APK on
// first run, starts Bunker's Lunar Tear server, then the game, and stops the
// server when the game closes.
//
// Folder layout, next to this program:
//
//	game/     bunker-game-bootstrap.exe and its DLLs (angle/, Firebase, gRPC)
//	server/   bunker-server.exe, from Bunker's releases
//	pods/     optional: what serves the Bunker's pages, shown in the game with Ctrl+B
//	*.apk     the player's game APK (any name)
//	data/     made here: the prepared APK, its libraries, saves and game storage
//
// Nothing of the game is part of this program.
package main

import (
	"archive/zip"
	"bufio"
	"context"
	"crypto/rand"
	"crypto/sha256"
	_ "embed"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"hash/crc32"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

//go:embed VERSION
var embeddedVersion string

var version = strings.TrimSpace(embeddedVersion)

// The game APK this port was made from.
const gameAPK = "9ded0797ee607b6892b531ba3a02f4c4510774f5fe098ccada78ccff881c9209"

// Two settings in the APK's assets/bin/Data/globalgamemanagers change, one byte
// each; no code changes. Swappy (Android frame pacing) has nothing to pace
// against here, and -nosound switches audio off.
const settingsAsset = "assets/bin/Data/globalgamemanagers"

var settings = []struct {
	name     string
	offset   int
	old, new byte
}{
	{"AudioManager.m_DisableAudio", 223600, 0, 1},
	{"PlayerSettings.androidUseSwappy", 219900, 1, 0},
}

var libraries = []string{"libil2cpp.so", "libunity.so"}

// The master data file the server reads.
const masterName = "20240404193219.bin.e"

func main() {
	assets := flag.String("assets", "", "folder with the imported game files the server serves (default: data/assets)")
	fps := flag.String("fps", "", "frame rate target: a number or game (default: Bunker Display, otherwise 144 on the Windows player)")
	render := flag.String("render", "", "render size, such as 3840x2160 (default: the window's)")
	silent := flag.Bool("nosound", false, "turn the game's sound off")
	exportSave := flag.String("export-save", "", "write a backup of the save to this .zip, then stop")
	importSave := flag.String("import-save", "", "replace the save with this backup (.zip, or a game.db), then stop")
	prepareOnly := flag.Bool("prepare", false, "prepare the game files, then stop")
	fullscreen := flag.Bool("fullscreen", false, "start the native player in borderless fullscreen")
	launcher := flag.Bool("launcher", false, "open Bunker before starting the game")
	flag.Parse()
	sound = !*silent
	self, e := os.Executable()
	if e == nil {
		var installed installation
		installed, e = playerInstallation(self)
		if e == nil {
			defer installed.close()
			if *exportSave != "" || *importSave != "" {
				e = saves(installed.root, *exportSave, *importSave)
			} else {
				e = run(installed.root, filepath.Dir(self), *assets, *fps, *render, *prepareOnly, *fullscreen, *launcher)
			}
		}
	}
	if e != nil {
		fmt.Fprintln(os.Stderr, "\n"+e.Error())
		if !*prepareOnly {
			fmt.Fprintln(os.Stderr, "\nPress Enter to close.")
			bufio.NewReader(os.Stdin).ReadString('\n')
		}
		os.Exit(1)
	}
}

// sound: the game's audio plays. Without it the APK is prepared with audio
// switched off and the game is muted.
var sound bool

func renderDimensions(value string) (string, string, error) {
	width, height, ok := strings.Cut(value, "x")
	w, ew := strconv.ParseUint(width, 10, 16)
	h, eh := strconv.ParseUint(height, 10, 16)
	if !ok || ew != nil || eh != nil || w == 0 || h == 0 {
		return "", "", fmt.Errorf("invalid render size %q, expected WIDTHxHEIGHT", value)
	}
	return width, height, nil
}

func run(root, resourceDir, assets, fps, render string, prepareOnly, fullscreen, showLauncher bool) error {
	var e error
	data := filepath.Join(root, "data")
	fmt.Printf("re[in]comp %s\n", version)
	game := filepath.Join(root, "game", "bunker-game-bootstrap.exe")
	player := filepath.Join(root, "game", "WindowsPlayer.exe")
	_, nativeErr := os.Stat(player)
	desktop := nativeErr == nil
	if desktop {
		game = player
	}
	server := filepath.Join(root, "server", "bunker-server.exe")
	for _, f := range []string{game, server} {
		if _, e := os.Stat(f); e != nil {
			return fmt.Errorf("%s is missing. Unzip the whole release into one folder.", f)
		}
	}
	var apk, native string
	if !desktop {
		apk, native, e = prepare(root, data)
		if e != nil {
			return e
		}
	}
	if prepareOnly {
		fmt.Println("Game files are ready.")
		return nil
	}
	if !desktop && fps == "" {
		fps = "165"
	}
	if assets == "" {
		if err := recoverPendingImport(filepath.Join(root, "data", "assets")); err != nil {
			return err
		}
		assets = locateResources(root, resourceDir)
	}
	var setup *resourceWindow
	defer func() {
		if setup != nil {
			setup.close()
		}
	}()
	view := filepath.Join(data, "bunker-view.json")
	os.Remove(view)
	readyFiles := resourcesReady(assets)
	if desktop && readyFiles && !showLauncher && !regularFile(filepath.Join(data, "manual-deploy")) {
		prefs := readLauncherPreferences(data)
		exe, _ := os.Executable()
		if regularFile(filepath.Join(data, "update-error.txt")) {
			showLauncher = true
		} else if prefs.Updates && prefs.AutoInstall && canUpdatePortable(root, exe) {
			ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
			release, err := latestWindowsRelease(ctx)
			cancel()
			showLauncher = err == nil && release.installable() && release.Version != prefs.Skipped && newerVersion(release.Version, version)
		}
	}
	if !readyFiles || desktop && (showLauncher || regularFile(filepath.Join(data, "manual-deploy"))) {
		dump, master := resourceInputs(resourceDir, root)
		if desktop && regularFile(filepath.Join(root, "game", "resource-window.exe")) {
			setup, e = startLauncher(root, server, assets, master, dump, readyFiles, false, nil)
			if errors.Is(e, context.Canceled) {
				return nil
			}
			if e != nil {
				return e
			}
		} else if e = importArchive(context.Background(), server, assets, master, dump, func(s string) { fmt.Println(s) }); e != nil {
			return e
		}
	}

	for {
		if setup != nil {
			select {
			case <-setup.done:
				return nil
			case <-setup.restart:
				return nil
			case <-setup.startServices:
				setup.mu.Lock()
				assets = setup.assetRoot
				setup.mu.Unlock()
			}
		}
		err := serveAndPlay(root, data, assets, game, server, view, native, apk, fps, render, desktop, fullscreen, setup)
		// The in-game menu lives inside Unity. Restore a standalone launcher
		// to show update progress or allow file changes after a manual stop.
		if errors.Is(err, errReturnToLauncher) {
			var update *launcherUpdate
			errors.As(err, &update)
			if setup != nil {
				setup.close()
			}
			setup, e = startLauncher(root, server, assets, "", "", resourcesReady(assets), false, update)
			if e != nil {
				return e
			}
			continue
		}
		if !errors.Is(err, errStoppedFromLauncher) {
			return err
		}
		setup.mu.Lock()
		setup.services = false
		setup.running = false
		setup.busy = false
		setup.checkURL = ""
		setup.checkToken = ""
		setup.send("reset-program")
		setup.status = "Lunar Tear is stopped."
		setup.mu.Unlock()
	}
}

var errStoppedFromLauncher = errors.New("Lunar Tear stopped from the launcher")
var errReturnToLauncher = errors.New("return to the launcher after stopping the game")

func serveAndPlay(root, data, assets, game, server, view, native, apk, fps, render string, desktop, fullscreen bool, setup *resourceWindow) (result error) {
	// The Bunker's pages (Pod Programs) are served when the pods folder is there.
	if err := installBundledNotices(root, assets); err != nil {
		return err
	}
	pods := filepath.Join(root, "pods")
	if _, e := os.Stat(filepath.Join(pods, "app", "desktop_runtime.py")); e != nil {
		if desktop {
			return errors.New("The packaged Bunker pages are missing.")
		}
		pods = ""
	}

	fmt.Println("Starting the server...")
	assetPort := defaultWindowsAssetPort
	if desktop {
		assetPort = readLauncherPreferences(data).AssetPort
	}
	portOffset := strconv.Itoa(assetPort - baseAssetPort)
	serve := exec.Command(server, "-data", filepath.Join(data, "server"), "-assets", assets, "-port-offset", portOffset, "-stdin")
	hideHelperWindow(serve)
	token := ""
	if pods != "" {
		secret := make([]byte, 24)
		if _, e := rand.Read(secret); e != nil {
			return e
		}
		token = hex.EncodeToString(secret)
		serve.Args = append(serve.Args, "-control", "127.0.0.1:0")
		serve.Env = append(os.Environ(), "BUNKER_CONTROL_TOKEN="+token)
	}
	hold, e := serve.StdinPipe() // closing it stops the server, also when this program dies
	if e != nil {
		return e
	}
	out, e := serve.StdoutPipe()
	if e != nil {
		return e
	}
	log, e := os.Create(filepath.Join(data, "server.log"))
	if e != nil {
		return e
	}
	defer log.Close()
	serve.Stderr = log
	if e := serve.Start(); e != nil {
		return fmt.Errorf("The server did not start: %v", e)
	}
	defer func() {
		hold.Close()
		if update, ok := result.(*launcherUpdate); ok {
			// Reopen the launcher immediately so it can show which service
			// is still stopping. The updater waits for the actual process exit.
			go func() { update.serverDone <- serve.Wait() }()
		} else {
			serve.Wait()
		}
	}()
	ready, control := make(chan string, 1), make(chan string, 1)
	go func() {
		lines := bufio.NewReader(out)
		line, _ := lines.ReadString('\n')
		ready <- line
		if pods != "" { // the second line names the control endpoint
			var answer struct{ Control string }
			line, _ = lines.ReadString('\n')
			json.Unmarshal([]byte(line), &answer)
			control <- answer.Control
		}
		io.Copy(io.Discard, out)
	}()
	select {
	case line := <-ready:
		if !strings.Contains(line, "\"game\"") {
			return fmt.Errorf("The server did not start. See %s.", log.Name())
		}
	case <-time.After(3 * time.Minute):
		return fmt.Errorf("The server took too long to start. See %s.", log.Name())
	}

	var session *playerSession
	if desktop && pods != "" {
		session, e = openPlayerSession(token)
		if e != nil {
			return e
		}
		defer session.close()
		// Unity needs the converter even if the optional Pod Programs fail to start.
		stopConverter, err := startBundleConverter(root, data)
		if err != nil {
			return fmt.Errorf("start Windows asset converter: %w", err)
		}
		defer stopConverter()
	}
	if pods != "" {
		var address string
		select {
		case address = <-control:
		case <-time.After(30 * time.Second):
		}
		stop := servePods(root, pods, data, assets, token, address, view, session)
		defer stop()
		if setup != nil {
			setup.mu.Lock()
			setup.checkURL, setup.checkToken = address, token
			setup.mu.Unlock()
		}
	}
	if setup != nil {
		// Pre-game editor requests may pause/resume Lunar Tear or change display
		// settings. The player starts only through the launcher's Deploy action.
		setup.mu.Lock()
		setup.running = true
		setup.mu.Unlock()
		for waiting := true; waiting; {
			select {
			case <-setup.done:
				return nil
			case <-setup.stopServices:
				return setup.stopped(errStoppedFromLauncher, nil)
			case <-setup.deploy:
				waiting = false
			case request := <-session.requests:
				request.result <- nil
			}
		}
	}

	if desktop {
		helper := filepath.Join(root, "game", "octo_cache.py")
		if regularFile(helper) {
			// Keep another launcher for this install from replacing a cache entry
			// between the player's private-write check and opening its descriptor.
			cacheLock, err := lockResourceFiles(data)
			if err != nil {
				return fmt.Errorf("Could not lock this player's game cache: %w", err)
			}
			defer cacheLock.Close()
			var closed, stop <-chan struct{}
			if setup != nil {
				closed, stop = setup.done, setup.stopServices
				setup.mu.Lock()
				setup.status = "Checking game files for cache sharing…"
				setup.mu.Unlock()
			}
			cache := exec.Command(filepath.Join(root, "pods", "python", "python.exe"),
				"-I", "-B", "-X", "utf8", helper, data, assets)
			hideHelperWindow(cache)
			cache.Stdout, cache.Stderr = log, log
			if err := cache.Start(); err != nil {
				fmt.Fprintln(log, "Octo cache preparation:", err)
			} else {
				finished := make(chan error, 1)
				go func() { finished <- cache.Wait() }()
				select {
				case err := <-finished:
					if err != nil {
						fmt.Fprintln(log, "Octo cache preparation:", err)
					}
				case <-closed:
					cache.Process.Kill()
					<-finished
					return nil
				case <-stop:
					cache.Process.Kill()
					<-finished
					return setup.stopped(errStoppedFromLauncher, nil)
				}
			}
		}
	}
	menu := setup
	if desktop && menu == nil {
		menu, e = startLauncher(root, server, assets, "", "", true, true, nil)
		if e != nil {
			return e
		}
		defer menu.close()
	}
	fmt.Println("Starting the game...")
	dir := filepath.Dir(game)
	args := []string{native, apk, filepath.Join(data, "storage"),
		"--frame-gate", "1280", "720", filepath.Join(dir, "angle"),
		"--mute-audio", "--collector-delivery", "--first-frame",
		"--firebase-app-dll", filepath.Join(dir, "FirebaseCppApp-9_6_0.dll"),
		"--run-until-close", "--quiet-trace",
		"--target-fps", fps, "--server-profile",
		"--grpc-native", filepath.Join(dir, "grpc_csharp_ext.x64.dll")}
	if sound {
		args = append(args, "--sound")
	}
	if render != "" {
		args = append(args, "--render-size", render)
	}
	if pods != "" {
		args = append(args, "--bunker-view", filepath.Join(pods, "WebView2Loader.dll"), view, filepath.Join(data, "webview"))
	}
	gameLog, e := os.Create(filepath.Join(data, "game.log"))
	if e != nil {
		return e
	}
	defer gameLog.Close()
	makePlayer := func() (*exec.Cmd, error) {
		cmd := exec.Command(game, args...)
		if desktop {
			display, err := savedDisplay(data, fps, render)
			if err != nil {
				return nil, err
			}
			maximized := false
			if fullscreen {
				display.windowMode = "borderless"
			}
			windowSize := display.size
			if setup == nil && render == "" {
				if size, max := previousWindowSize(data); size != "" {
					windowSize, maximized = size, max
				}
			}
			cmd, err = desktopCommand(game, root, data, display, windowSize)
			if err != nil {
				return nil, err
			}
			cmd.Env = append(cmd.Env, "REINCOMP_LAUNCHER_URL="+menu.homeURL)
			if !sound {
				cmd.Args = append(cmd.Args, "-nosound")
				cmd.Env = append(cmd.Env, "REINCOMP_MUTE_AUDIO=1")
			}
			if setup != nil {
				if display.windowMode == "borderless" {
					setup.send("borderless")
				} else {
					setup.send("windowed")
				}
				cmd.Args = append(cmd.Args, "-parentHWND", setup.handle, "delayed")
				cmd.Env = append(cmd.Env, "REINCOMP_WINDOW_PARENT="+setup.handle)
			} else if maximized {
				cmd.Env = append(cmd.Env, "REINCOMP_WINDOW_MAXIMIZED=1")
			}
		}
		cmd.Env = append(cmd.Environ(), "REINCOMP_BUNKER_PORT_OFFSET="+portOffset)
		cmd.Dir, cmd.Stdout, cmd.Stderr = dir, gameLog, gameLog
		return cmd, nil
	}
	if session != nil {
		if setup != nil {
			session.started = func(pid int) { setup.send("player " + strconv.Itoa(pid)) }
			session.windowClosed = setup.done
		}
		session.applyDisplay = func(pid int, resize bool) error {
			display, err := savedDisplay(data, fps, render)
			if err != nil {
				return err
			}
			return applyPlayerDisplay(pid, display, resize)
		}
		e = session.play(makePlayer, menu)
		if errors.Is(e, errReturnToLauncher) {
			return e
		}
	} else {
		cmd, err := makePlayer()
		if err != nil {
			return err
		}
		e = cmd.Run()
	}
	if e != nil {
		return fmt.Errorf("The game stopped: %v. See %s.", e, gameLog.Name())
	}
	return nil
}

func installBundledNotices(root, assets string) error {
	source := filepath.Join(root, "game", "notices.zip")
	dest := filepath.Join(assets, "notices", "notices.zip")
	if !regularFile(source) || regularFile(dest) {
		return nil
	}
	if err := os.MkdirAll(filepath.Dir(dest), 0755); err != nil {
		return err
	}
	in, err := os.Open(source)
	if err != nil {
		return err
	}
	defer in.Close()
	temp, err := os.CreateTemp(filepath.Dir(dest), ".notices-")
	if err != nil {
		return err
	}
	defer os.Remove(temp.Name())
	_, err = io.Copy(temp, in)
	if err = errors.Join(err, temp.Close()); err != nil {
		return err
	}
	return os.Rename(temp.Name(), dest)
}

func startBundleConverter(root, data string) (func(), error) {
	cmd := exec.Command(filepath.Join(root, "pods", "python", "python.exe"), "-I", "-B", "-X", "utf8",
		filepath.Join(root, "game", "desktop_bundles.py"), data)
	hideHelperWindow(cmd)
	log, err := os.Create(filepath.Join(data, "bundle-converter.log"))
	if err != nil {
		return nil, err
	}
	cmd.Stderr = log
	input, err := cmd.StdinPipe()
	if err != nil {
		log.Close()
		return nil, err
	}
	if err := cmd.Start(); err != nil {
		input.Close()
		log.Close()
		return nil, err
	}
	done := make(chan struct{})
	go func() {
		if err := cmd.Wait(); err != nil {
			fmt.Fprintln(log, err)
		}
		log.Close()
		close(done)
	}()
	return func() {
		input.Close()
		select {
		case <-done:
		case <-time.After(5 * time.Second):
			cmd.Process.Kill()
			<-done
		}
	}, nil
}

// servePods starts the Bunker's pages (Pod Programs) beside the server and
// writes where they are to STATE, which the game reads when Ctrl+B is pressed:
// {"url","token"}, or {"error"} when they did not start. The game does not
// wait for it. The returned function stops the pages.
func servePods(root, pods, data, assets, token, url, state string, session *playerSession) func() {
	report := func(answer any) {
		b, _ := json.Marshal(answer)
		if os.WriteFile(state+".part", b, 0o600) == nil {
			os.Rename(state+".part", state)
		}
	}
	fail := func(text string) {
		report(map[string]string{"error": text + " See pods.log in the data folder."})
	}
	// The master data as the player supplied it, which Pod Programs keeps to start changes from.
	master := filepath.Join(data, "original-master.bin.e")
	if _, e := os.Stat(master); e != nil {
		master = filepath.Join(root, masterName)
	}
	if _, e := os.Stat(master); e != nil {
		master = filepath.Join(assets, "assets", "release", masterName)
	}
	if url == "" {
		fail("The Bunker did not start: the server has no control endpoint.")
		return func() {}
	}
	services := filepath.Join(root, "game", "desktop_services.py")
	if _, e := os.Stat(services); e != nil {
		services = filepath.Join(pods, "app", "desktop_runtime.py")
	}
	run := exec.Command(filepath.Join(pods, "python", "python.exe"), "-I", "-X", "utf8", services,
		"--data", filepath.Join(data, "server"), "--assets", assets, "--tools", filepath.Join(data, "pods"),
		"--control", url, "--original-master", master)
	hideHelperWindow(run)
	run.Dir = filepath.Join(pods, "app")
	run.Env = append(os.Environ(), "BUNKER_CONTROL_TOKEN="+token)
	if session != nil {
		run.Env = append(run.Env, "REINCOMP_PLAYER_CONTROL="+session.url)
	}
	hold, e1 := run.StdinPipe() // closing it stops the pages, also when this program dies
	out, e2 := run.StdoutPipe()
	log, e3 := os.Create(filepath.Join(data, "pods.log"))
	if e := errors.Join(e1, e2, e3); e != nil {
		fail("The Bunker did not start: " + e.Error() + ".")
		return func() {}
	}
	run.Stderr = log
	if e := run.Start(); e != nil {
		log.Close()
		fail("The Bunker did not start: " + e.Error() + ".")
		return func() {}
	}
	go func() {
		var answer struct{ URL, Token string }
		if session != nil {
			defer close(session.editorGone)
		}
		line, _ := bufio.NewReader(out).ReadString('\n')
		if json.Unmarshal([]byte(line), &answer) != nil || answer.URL == "" {
			fail("The Bunker did not start.")
		} else {
			report(map[string]string{"url": answer.URL, "token": answer.Token, "theme": readLauncherPreferences(data).Theme})
		}
		io.Copy(io.Discard, out)
	}()
	return func() {
		hold.Close()
		done := make(chan struct{})
		go func() { run.Wait(); close(done) }()
		select {
		case <-done:
		case <-time.After(5 * time.Second):
			run.Process.Kill()
			<-done
		}
		log.Close()
		os.Remove(state)
	}
}

// saves keeps or restores the player's save with the server's own commands
// (bunker-server saves). The backup is the phone app's format, so a save
// moves between a phone and this computer either way.
func saves(root, exportTo, importFrom string) error {
	server := filepath.Join(root, "server", "bunker-server.exe")
	data := filepath.Join(root, "data", "server")
	arguments := []string{"saves", "export", "-data", data, "-out", exportTo}
	if importFrom != "" {
		arguments = []string{"saves", "import", "-data", data, "-from", importFrom}
	}
	out, e := exec.Command(server, arguments...).CombinedOutput()
	if e != nil {
		return fmt.Errorf("%s", strings.TrimSpace(strings.TrimPrefix(strings.TrimSpace(string(out)), "bunker-server: ")))
	}
	if importFrom != "" {
		fmt.Println("The save was restored. The one it replaced is kept in data\\server.before-import.")
	} else {
		fmt.Println("The save was written to " + exportTo)
	}
	return nil
}

func regularFile(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.Mode().IsRegular()
}

func resourcesReady(assets string) bool {
	if !regularFile(filepath.Join(assets, "assets", "release", masterName)) {
		return false
	}
	base := filepath.Join(assets, "assets", "revisions", "0")
	for _, leaf := range []string{"", "android"} {
		revision := filepath.Join(base, leaf)
		bundles, e1 := os.Stat(filepath.Join(revision, "assetbundle"))
		resources, e2 := os.Stat(filepath.Join(revision, "resources"))
		if e1 == nil && e2 == nil && bundles.IsDir() && resources.IsDir() && regularFile(filepath.Join(revision, "list.bin")) &&
			(regularFile(filepath.Join(revision, "info.json")) || regularFile(filepath.Join(base, "info.json"))) {
			return true
		}
	}
	return false
}

func locateResources(root, resourceDir string) string {
	local := filepath.Join(root, "data", "assets")
	for _, dir := range []string{local, filepath.Join(resourceDir, "data", "assets"), filepath.Join(resourceDir, "assets"), resourceDir} {
		if resourcesReady(dir) {
			return dir
		}
	}
	return local
}

func resourceInputs(resourceDir, root string) (string, string) {
	first := func(patterns ...string) string {
		for _, pattern := range patterns {
			if found, _ := filepath.Glob(filepath.Join(resourceDir, pattern)); len(found) > 0 {
				return found[0]
			}
		}
		return ""
	}
	dump, master := first("resource_dump*.7z", "resource_dump*.zip"), first(masterName)
	if dump == "" {
		// An unambiguously named archive can also sit beside the downloaded EXE.
		var found []string
		files, _ := os.ReadDir(resourceDir)
		for _, file := range files {
			ext := strings.ToLower(filepath.Ext(file.Name()))
			if !file.IsDir() && (ext == ".7z" || ext == ".zip") {
				found = append(found, file.Name())
			}
		}
		if len(found) == 1 {
			dump = filepath.Join(resourceDir, found[0])
		}
	}
	if packaged := filepath.Join(root, "data", "import-master.bin.e"); regularFile(packaged) {
		master = packaged
	}
	return dump, master
}

func importArchive(ctx context.Context, server, assets, master, dump string, progress func(string)) error {
	if !regularFile(dump) || !regularFile(master) {
		return fmt.Errorf("Put these two files next to this program, then start it again:\n  %s (the master data)\n  resource_dump_android.7z (the resource dump)\nOr pass a folder that already holds the imported game files with -assets.", masterName)
	}
	progress("Importing " + filepath.Base(dump))
	args := []string{"import", "-assets", assets, "-dump", dump}
	if filepath.Clean(master) != filepath.Join(assets, "assets", "release", masterName) {
		args = append(args, "-master", master)
	}
	run := exec.CommandContext(ctx, server, args...)
	hideHelperWindow(run)
	out, e := run.StdoutPipe()
	if e != nil {
		return e
	}
	var problem strings.Builder
	run.Stderr = &problem
	if e := run.Start(); e != nil {
		return e
	}
	lines := bufio.NewScanner(out)
	for lines.Scan() {
		var state struct {
			Step        string
			Done, Total int64
		}
		if json.Unmarshal(lines.Bytes(), &state) == nil && state.Total > 0 {
			progress(fmt.Sprintf("Importing %s · %d%%", filepath.Base(dump), state.Done*100/state.Total))
		}
	}
	if e := run.Wait(); e != nil {
		if ctx.Err() != nil {
			return ctx.Err()
		}
		return fmt.Errorf("The game files were not imported: %s", strings.TrimSpace(problem.String()))
	}
	if !resourcesReady(assets) {
		return errors.New("The imported resources are missing the Global revision catalog or master data.")
	}
	return nil
}

// prepare makes data/game.apk and data/native from the player's APK, once.
func prepare(root, data string) (apk, native string, e error) {
	apk, native = filepath.Join(data, "game.apk"), filepath.Join(data, "native")
	done := filepath.Join(data, "prepared")
	if sound {
		apk, done = filepath.Join(data, "game-sound.apk"), filepath.Join(data, "prepared-sound")
	}
	if _, e := os.Stat(done); e == nil {
		return apk, native, nil
	}
	found, _ := filepath.Glob(filepath.Join(root, "*.apk"))
	var source string
	for _, f := range found {
		fmt.Printf("Checking %s...\n", filepath.Base(f))
		if sum, e := fileSHA256(f); e == nil && sum == gameAPK {
			source = f
			break
		}
	}
	if source == "" {
		return "", "", fmt.Errorf("Put your game APK next to this program.\nIt must be the version this port was made from (SHA-256 %s).", gameAPK)
	}
	fmt.Println("Preparing the game files (once)...")
	if e := os.MkdirAll(native, 0o755); e != nil {
		return "", "", e
	}
	in, e := zip.OpenReader(source)
	if e != nil {
		return "", "", e
	}
	defer in.Close()
	temp := apk + ".part"
	file, e := os.Create(temp)
	if e != nil {
		return "", "", e
	}
	out := zip.NewWriter(file)
	fail := func(e error) (string, string, error) { file.Close(); os.Remove(temp); return "", "", e }
	patched, extracted := false, 0
	for _, entry := range in.File {
		if entry.Name == settingsAsset {
			content, e := readEntry(entry)
			if e != nil {
				return fail(e)
			}
			for _, s := range settings {
				if sound && s.name == "AudioManager.m_DisableAudio" {
					continue
				}
				if s.offset >= len(content) || content[s.offset] != s.old {
					return fail(fmt.Errorf("%s is not where it is in the supported game version", s.name))
				}
				content[s.offset] = s.new
			}
			// Stored, not compressed: the prepared APK is then the same bytes whatever
			// wrote it, and the game checks its hash.
			header := entry.FileHeader
			header.Method = zip.Store
			header.Flags &^= 0x8
			header.CRC32 = crc32.ChecksumIEEE(content)
			header.CompressedSize64, header.UncompressedSize64 = uint64(len(content)), uint64(len(content))
			w, e := out.CreateRaw(&header)
			if e == nil {
				_, e = w.Write(content)
			}
			if e != nil {
				return fail(e)
			}
			patched = true
			continue
		}
		for _, name := range libraries {
			if entry.Name == "lib/arm64-v8a/"+name {
				content, e := readEntry(entry)
				if e == nil {
					e = os.WriteFile(filepath.Join(native, name), content, 0o644)
				}
				if e != nil {
					return fail(e)
				}
				extracted++
			}
		}
		// Every other entry is copied as it is stored, without recompressing.
		raw, e := entry.OpenRaw()
		if e != nil {
			return fail(e)
		}
		w, e := out.CreateRaw(&entry.FileHeader)
		if e == nil {
			_, e = io.Copy(w, raw)
		}
		if e != nil {
			return fail(e)
		}
	}
	if !patched || extracted != len(libraries) {
		return fail(errors.New("The APK lacks the game's settings or libraries."))
	}
	if e := out.Close(); e != nil {
		return fail(e)
	}
	if e := file.Close(); e != nil {
		return fail(e)
	}
	if e := os.Rename(temp, apk); e != nil {
		return "", "", e
	}
	return apk, native, os.WriteFile(done, []byte(gameAPK+"\n"), 0o644)
}

func readEntry(entry *zip.File) ([]byte, error) {
	r, e := entry.Open()
	if e != nil {
		return nil, e
	}
	defer r.Close()
	return io.ReadAll(r)
}

func fileSHA256(path string) (string, error) {
	f, e := os.Open(path)
	if e != nil {
		return "", e
	}
	defer f.Close()
	h := sha256.New()
	if _, e := io.Copy(h, f); e != nil {
		return "", e
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}
