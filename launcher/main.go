// reincomp starts the native game: it prepares the player's own game APK on
// first run, starts Bunker's Lunar Tear server, then the game, and stops the
// server when the game closes.
//
// Folder layout, next to this program:
//
//	game/     bunker-game-bootstrap.exe and its DLLs (angle/, Firebase, gRPC)
//	server/   bunker-server.exe, from Bunker's releases
//	*.apk     the player's game APK (any name)
//	data/     made here: the prepared APK, its libraries, saves and game storage
//
// Nothing of the game is part of this program.
package main

import (
	"archive/zip"
	"bufio"
	"crypto/sha256"
	"encoding/hex"
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

// The ports the game's server profile expects: Bunker's 8003, 8080 and 3000 plus this.
const portOffset = "30000"

func main() {
	assets := flag.String("assets", "", "folder with the imported game files the server serves (default: data/assets)")
	fps := flag.String("fps", "game", "frame rate target: a number, or \"game\" for the game's own")
	render := flag.String("render", "", "render size, such as 3840x2160 (default: the window's)")
	silent := flag.Bool("nosound", false, "turn the game's sound off")
	prepareOnly := flag.Bool("prepare", false, "prepare the game files, then stop")
	flag.Parse()
	sound = !*silent
	if e := run(*assets, *fps, *render, *prepareOnly); e != nil {
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
	if _, e := os.Stat(assets); e != nil {
		return fmt.Errorf("The game files the server serves were not found at %s.\nPass their folder with -assets.", assets)
	}

	fmt.Println("Starting the server...")
	serve := exec.Command(server, "-data", filepath.Join(data, "server"), "-assets", assets, "-port-offset", portOffset, "-stdin")
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
	ready := make(chan string, 1)
	go func() { line, _ := bufio.NewReader(out).ReadString('\n'); ready <- line; io.Copy(io.Discard, out) }()
	select {
	case line := <-ready:
		if !strings.Contains(line, "\"game\"") {
			return fmt.Errorf("The server did not start. See %s.", log.Name())
		}
	case <-time.After(3 * time.Minute):
		return fmt.Errorf("The server took too long to start. See %s.", log.Name())
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
