package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestPreviousWindowSize(t *testing.T) {
	original := displayMode
	displayMode = func() (int, int, int) { return 1920, 1080, 60 }
	t.Cleanup(func() { displayMode = original })
	data := t.TempDir()
	if size, max := previousWindowSize(data); size != "" || max {
		t.Fatalf("fresh install returned %q, maximized %v", size, max)
	}
	for _, tc := range []struct {
		file, want string
		maximized  bool
	}{
		{"1438 778 0", "1438x778", false},
		{"1190 702 1", "1190x702", true},
		{"3840 2160 0", "1920x1080", false},
		{"1400 0 0", "", false},
		{"1400 800 2", "", false},
		{"1400", "", false},
	} {
		if err := os.WriteFile(filepath.Join(data, "window-size"), []byte(tc.file), 0600); err != nil {
			t.Fatal(err)
		}
		if size, max := previousWindowSize(data); size != tc.want || max != tc.maximized {
			t.Errorf("read %q as %q, maximized %v", tc.file, size, max)
		}
	}
}

func TestBunkerDisplay(t *testing.T) {
	mode := func() (int, int, int) { return 3840, 2160, 120 }
	for _, tc := range []struct{ conf, fps, size, wantFPS, wantSize, skip string }{
		{"", "", "", "144", "1600x900", ""},
		{"", "165", "", "165", "1600x900", ""},
		{"fps=max\r\nresolution=native\r\ntextures=half", "", "", "120", "3840x2160", "1"},
		{"fps=60\nresolution=540\ntextures=quarter", "", "", "60", "960x540", "2"},
		{"fps=60\nresolution=540", "game", "2560x1440", "game", "2560x1440", ""},
		{"resolution=720", "", "", "144", "1280x720", ""},
		{"resolution=900", "", "", "144", "1600x900", ""},
		{"fps=165\nresolution=2160", "", "", "165", "3840x2160", ""},
		{"fps=bad\nresolution=bad\ntextures=bad", "", "", "144", "1600x900", ""},
	} {
		d := desktopDisplay(tc.conf+"\nshadows=off\nbloom=off\nidle=on", tc.fps, tc.size, mode)
		if d.fps != tc.wantFPS || d.size != tc.wantSize || d.textureSkip != tc.skip || d.effectsOff != 5 || !d.idle {
			t.Errorf("settings %q, flags %q %q: got %+v", tc.conf, tc.fps, tc.size, d)
		}
	}
}
