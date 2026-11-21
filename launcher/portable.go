package main

import (
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

const packageMagic = "BUNKER.WINDOWS.1"
const packageChunk = 4 << 20

type playerFile struct {
	Path        string   `json:"path"`
	Size        int64    `json:"size"`
	Chunks      []string `json:"chunks"`
	Compression string   `json:"compression,omitempty"`
	StoredSize  int64    `json:"storedSize,omitempty"`
}

func (p playerFile) diskSize() int64 {
	if p.Compression == "gzip" {
		return p.StoredSize
	}
	return p.Size
}

var lockPlayerPackage = func(string) (func(), error) {
	return nil, errors.New("the portable player requires Windows")
}

func immutableMember(name string) bool {
	top, rest, ok := strings.Cut(name, "/")
	if !ok || !fs.ValidPath(name) || strings.ContainsAny(name, `\:`) {
		return false
	}
	for _, part := range strings.Split(name, "/") {
		if strings.HasSuffix(part, ".") || strings.HasSuffix(part, " ") {
			return false
		}
	}
	switch top {
	case "game", "pods", "server", "licenses":
		return true
	case "data":
		return strings.HasPrefix(rest, "native/") || strings.HasPrefix(rest, "apk-assets/") || strings.HasPrefix(rest, "storage/files/il2cpp/") ||
			rest == "game-sound.apk" || rest == "original-master.bin.e" || rest == "import-master.bin.e"
	}
	return false
}

type installation struct {
	root  string
	close func()
}

// Normal builds use the launcher directory. A browser-built executable
// installs its immutable members beside itself. Saves and settings never
// appear in the package manifest.
func playerInstallation(exe string) (installation, error) {
	outer := filepath.Dir(exe)
	f, err := os.Open(exe)
	if err != nil {
		return installation{}, err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return installation{}, err
	}
	var footer [28]byte
	if info.Size() < int64(len(footer)) {
		return installation{outer, func() {}}, nil
	}
	end := info.Size() - int64(len(footer))
	if _, err = f.ReadAt(footer[:], end); err != nil {
		return installation{}, err
	}
	if string(footer[:16]) != packageMagic {
		return installation{outer, func() {}}, nil
	}
	n := int64(binary.LittleEndian.Uint32(footer[16:]))
	body := binary.LittleEndian.Uint64(footer[20:])
	if n <= 0 || n > 32<<20 || n > end || body > uint64(end-n) {
		return installation{}, errors.New("invalid Windows player package footer")
	}
	raw := make([]byte, n)
	if _, err = f.ReadAt(raw, end-n); err != nil {
		return installation{}, err
	}
	var manifest struct {
		Format int
		Files  []playerFile
	}
	if err = json.Unmarshal(raw, &manifest); err != nil {
		return installation{}, err
	}
	if manifest.Format != 1 && manifest.Format != 2 {
		return installation{}, errors.New("unsupported Windows player package version")
	}
	var total uint64
	seen := make(map[string]bool)
	for _, part := range manifest.Files {
		key := strings.ToLower(part.Path)
		_, pathErr := filepath.Localize(part.Path)
		validEncoding := part.Compression == "" && part.StoredSize == 0 || manifest.Format == 2 && part.Compression == "gzip" && part.StoredSize > 0
		if pathErr != nil || !immutableMember(part.Path) || seen[key] || !validEncoding || part.Size < 0 || uint64(part.diskSize()) > body-total ||
			uint64(len(part.Chunks)) != (uint64(part.Size)+packageChunk-1)/packageChunk {
			return installation{}, fmt.Errorf("invalid player package member %q", part.Path)
		}
		seen[key] = true
		total += uint64(part.diskSize())
	}
	if total != body || !seen["game/windowsplayer.exe"] || !seen["server/bunker-server.exe"] || !seen["data/import-master.bin.e"] {
		return installation{}, errors.New("incomplete Windows player package")
	}
	fingerprint := fmt.Sprintf("%x", sha256.Sum256(raw))
	install := filepath.Join(outer, ".bunker-player")
	if err = os.MkdirAll(install, 0755); err != nil {
		return installation{}, err
	}
	dir, err := os.Lstat(install)
	if err != nil || !dir.IsDir() || dir.Mode()&os.ModeSymlink != 0 {
		return installation{}, fmt.Errorf("the player cache must be a directory beside the executable")
	}
	root, err := os.OpenRoot(install)
	if err != nil {
		return installation{}, err
	}
	defer root.Close()
	// Serialize installers and keep this generation selected until the game
	// exits. Windows releases the lock even if the launcher is terminated.
	release, err := lockPlayerPackage(install)
	if err != nil {
		return installation{}, err
	}
	keep := false
	defer func() {
		if !keep {
			release()
		}
	}()
	const owner = "Bunker portable player 1"
	if marker, readErr := root.ReadFile(".owner"); readErr != nil || string(marker) != owner {
		items, listErr := os.ReadDir(install)
		if listErr != nil {
			return installation{}, listErr
		}
		for _, item := range items {
			if item.Name() != ".package-lock" {
				return installation{}, fmt.Errorf("the folder %s is already in use", install)
			}
		}
		if err = root.WriteFile(".owner", []byte(owner), 0600); err != nil {
			return installation{}, err
		}
	}
	if existing, err := root.ReadFile(".package"); err == nil && string(existing) == fingerprint {
		keep = true
		return installation{install, release}, nil
	}
	// Verify the full download before replacing any file in a working install.
	incoming, err := os.MkdirTemp(install, ".install-")
	if err != nil {
		return installation{}, err
	}
	defer os.RemoveAll(incoming)
	stage, err := os.OpenRoot(incoming)
	if err != nil {
		return installation{}, err
	}
	defer stage.Close()
	source := io.NewSectionReader(f, end-n-int64(body), int64(body))
	buffer := make([]byte, packageChunk)
	for _, part := range manifest.Files {
		name, _ := filepath.Localize(part.Path)
		if err = stage.MkdirAll(filepath.Dir(name), 0755); err != nil {
			return installation{}, err
		}
		out, err := stage.OpenFile(name, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
		if err != nil {
			return installation{}, err
		}
		stored := &io.LimitedReader{R: source, N: part.diskSize()}
		var decoded io.Reader = stored
		var compressed *gzip.Reader
		if part.Compression == "gzip" {
			compressed, err = gzip.NewReader(stored)
			if err != nil {
				out.Close()
				return installation{}, fmt.Errorf("opening compressed %s: %w", name, err)
			}
			decoded = compressed
		}
		left := part.Size
		for _, expected := range part.Chunks {
			chunk := buffer[:min(int64(len(buffer)), left)]
			if _, err = io.ReadFull(decoded, chunk); err != nil {
				break
			}
			want, decodeErr := hex.DecodeString(expected)
			digest := sha256.Sum256(chunk)
			if decodeErr != nil || !bytes.Equal(want, digest[:]) {
				err = fmt.Errorf("invalid player data for %s", name)
				break
			}
			if _, err = out.Write(chunk); err != nil {
				break
			}
			left -= int64(len(chunk))
		}
		if compressed != nil {
			if err == nil {
				var extra [1]byte
				if n, end := compressed.Read(extra[:]); n != 0 || end != io.EOF || stored.N != 0 {
					err = fmt.Errorf("invalid compressed length or checksum for %s", name)
				}
			}
			err = errors.Join(err, compressed.Close())
		}
		err = errors.Join(err, out.Close())
		if err != nil {
			return installation{}, err
		}
	}
	if seen["game/package_assets.py"] {
		// Browser builds carry the player's original assets once. Convert them
		// in the verified staging directory before publishing this generation.
		convert := exec.Command(filepath.Join(incoming, "pods", "python", "python.exe"), "-I", "-B", "-X", "utf8",
			filepath.Join(incoming, "game", "package_assets.py"), filepath.Join(incoming, "data", "game-sound.apk"), incoming,
			filepath.Join(incoming, "data", "storage", "files", "il2cpp", "Metadata", "global-metadata.dat"))
		hideHelperWindow(convert)
		if out, e := convert.CombinedOutput(); e != nil {
			return installation{}, fmt.Errorf("preparing the APK assets: %w\n%s", e, out)
		}
		for _, dir := range []string{"game/WindowsPlayer_Data", "data/apk-assets", "data/native", "data/storage/files/il2cpp"} {
			err = fs.WalkDir(os.DirFS(incoming), dir, func(name string, entry fs.DirEntry, e error) error {
				if e != nil {
					return e
				}
				if entry.IsDir() {
					return nil
				}
				if !entry.Type().IsRegular() || !immutableMember(name) {
					return fmt.Errorf("invalid generated player asset %s", name)
				}
				key := strings.ToLower(name)
				if !seen[key] {
					manifest.Files = append(manifest.Files, playerFile{Path: name})
					seen[key] = true
				}
				return nil
			})
			if err != nil {
				return installation{}, err
			}
		}
	}
	if err = root.Remove(".package"); err != nil && !errors.Is(err, fs.ErrNotExist) {
		return installation{}, err
	}
	for _, part := range manifest.Files {
		name, _ := filepath.Localize(part.Path)
		if err = root.MkdirAll(filepath.Dir(name), 0755); err != nil {
			return installation{}, err
		}
		if err = root.Rename(filepath.Join(filepath.Base(incoming), name), name); err != nil {
			return installation{}, err
		}
	}
	if err = root.WriteFile(".package-part", []byte(fingerprint), 0600); err != nil {
		return installation{}, err
	}
	if err = root.Rename(".package-part", ".package"); err != nil {
		return installation{}, err
	}
	keep = true
	return installation{install, release}, nil
}
