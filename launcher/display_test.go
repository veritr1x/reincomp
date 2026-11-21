package main

import "testing"

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
