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
	"crypto/rand"
	"crypto/sha256"
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
	"strings"
	"time"
)

var version = "dev"

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

// The ports the game's server profile expects: Bunker's 8003, 8080 and 3000 plus this.
const portOffset = "30000"

func main() {
	assets := flag.String("assets", "", "folder with the imported game files the server serves (default: data/assets)")
	fps := flag.String("fps", "game", "frame rate target: a number, or \"game\" for the game's own")
	render := flag.String("render", "", "render size, such as 3840x2160 (default: the window's)")
	silent := flag.Bool("nosound", false, "turn the game's sound off")
	exportSave := flag.String("export-save", "", "write a backup of the save to this .zip, then stop")
	importSave := flag.String("import-save", "", "replace the save with this backup (.zip, or a game.db), then stop")
	prepareOnly := flag.Bool("prepare", false, "prepare the game files, then stop")
	flag.Parse()
	sound = !*silent
	e := error(nil)
	if *exportSave != "" || *importSave != "" {
		e = saves(*exportSave, *importSave)
	} else {
		e = run(*assets, *fps, *render, *prepareOnly)
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

func run(assets, fps, render string, prepareOnly bool) error {
	self, e := os.Executable()
	if e != nil {
		return e
	}
	root := filepath.Dir(self)
	data := filepath.Join(root, "data")
	fmt.Printf("re[in]comp %s\n", version)
	game := filepath.Join(root, "game", "bunker-game-bootstrap.exe")
	server := filepath.Join(root, "server", "bunker-server.exe")
	for _, f := range []string{game, server} {
		if _, e := os.Stat(f); e != nil {
			return fmt.Errorf("%s is missing. Unzip the whole release into one folder.", f)
		}
	}
	apk, native, e := prepare(root, data)
	if e != nil {
		return e
	}
	if prepareOnly {
		fmt.Println("Game files are ready.")
		return nil
	}
	if assets == "" {
		assets = filepath.Join(data, "assets")
	}
	if e := importFiles(root, server, assets); e != nil {
		return e
	}

	// The Bunker's pages (Pod Programs) are served when the pods folder is there.
	pods := filepath.Join(root, "pods")
	if _, e := os.Stat(filepath.Join(pods, "app", "desktop_runtime.py")); e != nil {
		pods = ""
	}
	view := filepath.Join(data, "bunker-view.json")
	os.Remove(view)

	fmt.Println("Starting the server...")
	serve := exec.Command(server, "-data", filepath.Join(data, "server"), "-assets", assets, "-port-offset", portOffset, "-stdin")
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
	defer func() { hold.Close(); serve.Wait() }()
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

	if pods != "" {
		stop := servePods(root, pods, data, assets, token, control, view)
		defer stop()
	}

	fmt.Println("Starting the game...")
	dir := filepath.Dir(game)
	args := []string{native, apk, filepath.Join(data, "storage"),
		"--frame-gate", "1280", "720", filepath.Join(dir, "angle"),
		"--mute-audio", "--collector-delivery", "--first-frame",
		"--firebase-app-dll", filepath.Join(dir, "FirebaseCppApp-9_6_0.dll"),
		"--run-frames", "2000000000", "--quiet-trace",
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
	play := exec.Command(game, args...)
	play.Dir = dir
	gameLog, e := os.Create(filepath.Join(data, "game.log"))
	if e != nil {
		return e
	}
	defer gameLog.Close()
	play.Stdout, play.Stderr = gameLog, gameLog
	if e := play.Run(); e != nil {
		return fmt.Errorf("The game stopped: %v. See %s.", e, gameLog.Name())
	}
	return nil
}

// servePods starts the Bunker's pages (Pod Programs) beside the server and
// writes where they are to STATE, which the game reads when Ctrl+B is pressed:
// {"url","token"}, or {"error"} when they did not start. The game does not
// wait for it. The returned function stops the pages.
func servePods(root, pods, data, assets, token string, control <-chan string, state string) func() {
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
	master := filepath.Join(root, masterName)
	if _, e := os.Stat(master); e != nil {
		master = filepath.Join(assets, "assets", "release", masterName)
	}
	url := ""
	select {
	case url = <-control:
	case <-time.After(30 * time.Second):
	}
	if url == "" {
		fail("The Bunker did not start: the server has no control endpoint.")
		return func() {}
	}
	run := exec.Command(filepath.Join(pods, "python", "python.exe"), "-I", "-X", "utf8", filepath.Join(pods, "app", "desktop_runtime.py"),
		"--data", filepath.Join(data, "server"), "--assets", assets, "--tools", filepath.Join(data, "pods"),
		"--control", url, "--original-master", master)
	run.Dir = filepath.Join(pods, "app")
	run.Env = append(os.Environ(), "BUNKER_CONTROL_TOKEN="+token)
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
		line, _ := bufio.NewReader(out).ReadString('\n')
		if json.Unmarshal([]byte(line), &answer) != nil || answer.URL == "" {
			fail("The Bunker did not start.")
		} else {
			report(map[string]string{"url": answer.URL, "token": answer.Token})
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
func saves(exportTo, importFrom string) error {
	self, e := os.Executable()
	if e != nil {
		return e
	}
	root := filepath.Dir(self)
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

// importFiles puts the player's resource dump and master data where the server
// reads them, the first time: both are looked for next to this program. The
// server's own import does the work (bunker-server import).
func importFiles(root, server, assets string) error {
	if _, e := os.Stat(filepath.Join(assets, "assets", "revisions", "0")); e == nil {
		if _, e := os.Stat(filepath.Join(assets, "assets", "release", masterName)); e == nil {
			return nil
		}
	}
	first := func(patterns ...string) string {
		for _, pattern := range patterns {
			if found, _ := filepath.Glob(filepath.Join(root, pattern)); len(found) > 0 {
				return found[0]
			}
		}
		return ""
	}
	dump, master := first("resource_dump*.7z", "resource_dump*.zip"), first(masterName)
	if dump == "" || master == "" {
		return fmt.Errorf("Put these two files next to this program, then start it again:\n  %s (the master data)\n  resource_dump_android.7z (the resource dump)\nOr pass a folder that already holds the imported game files with -assets.", masterName)
	}
	fmt.Println("Importing the game files (once; about 20 GB, several minutes)...")
	run := exec.Command(server, "import", "-assets", assets, "-master", master, "-dump", dump)
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
			fmt.Printf("\r  %3d%%", state.Done*100/state.Total)
		}
	}
	fmt.Println()
	if e := run.Wait(); e != nil {
		return fmt.Errorf("The game files were not imported: %s", strings.TrimSpace(problem.String()))
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
