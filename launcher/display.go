package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
)

// A normal launch in a fresh installation starts in a 1600x900 window.
// Explicit command-line choices override the corresponding saved choice.
type displaySettings struct {
	fps, size, textureSkip string
	effectsOff             int
	idle                   bool
	borderless             bool
	fsr31                  bool
	fsrQuality, sharpness  int
}

func desktopDisplay(conf, fps, size string, mode func() (int, int, int)) displaySettings {
	settings := make(map[string]string)
	for _, line := range strings.Split(conf, "\n") {
		if k, v, ok := strings.Cut(line, "="); ok {
			settings[strings.TrimSpace(k)] = strings.TrimSpace(v)
		}
	}
	var width, height, hz int
	if (fps == "" && settings["fps"] == "max") || (size == "" && settings["resolution"] != "" && settings["resolution"] != "default") {
		width, height, hz = mode()
	}
	if fps == "" {
		switch value := settings["fps"]; value {
		case "30", "60", "90", "120", "144", "165":
			fps = value
		case "max":
			if hz > 1 {
				fps = strconv.Itoa(min(hz, 240))
			}
		}
		if fps == "" {
			fps = "144"
		}
	}
	if size == "" {
		size = "1600x900"
		if width > 0 && height > 0 {
			switch value := settings["resolution"]; value {
			case "native":
				size = fmt.Sprintf("%dx%d", width, height)
			case "540", "640", "720", "900", "1080", "1440", "2160":
				edge, _ := strconv.Atoi(value)
				short := min(width, height)
				edge = min(short, edge)
				size = fmt.Sprintf("%dx%d", (width*edge+short/2)/short, (height*edge+short/2)/short)
			}
		}
	}
	var effectsOff int
	for bit, name := range []string{"shadows", "reflections", "bloom"} {
		if settings[name] == "off" {
			effectsOff |= 1 << bit
		}
	}
	integer := func(key string, fallback, maximum int) int {
		value, err := strconv.Atoi(settings[key])
		if err != nil || value < 0 || value > maximum {
			return fallback
		}
		return value
	}
	return displaySettings{fps: fps, size: size, textureSkip: map[string]string{"half": "1", "quarter": "2"}[settings["textures"]],
		effectsOff: effectsOff, idle: settings["idle"] == "on", borderless: settings["window_mode"] == "borderless", fsr31: settings["upscale"] == "fsr31",
		fsrQuality: integer("fsr_quality", 1, 4), sharpness: integer("fsr_sharpness", 20, 100)}
}

// The launcher and its save commands can also be built on a non-Windows host.
var displayMode = func() (int, int, int) { return 0, 0, 0 }

func previousWindowSize(data string) (string, bool) {
	contents, err := os.ReadFile(filepath.Join(data, "window-size"))
	if err != nil {
		return "", false
	}
	var width, height, maximized int
	if n, err := fmt.Sscan(string(contents), &width, &height, &maximized); err != nil || n != 3 ||
		width < 1 || height < 1 || width > 32767 || height > 32767 || (maximized != 0 && maximized != 1) {
		return "", false
	}
	screenW, screenH, _ := displayMode()
	if screenW > 0 && screenH > 0 {
		width, height = min(width, screenW), min(height, screenH)
	}
	return fmt.Sprintf("%dx%d", width, height), maximized == 1
}

func savedDisplay(data, fps, render string) (displaySettings, error) {
	conf, err := os.ReadFile(filepath.Join(data, "pods", "display.conf"))
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return displaySettings{}, fmt.Errorf("read Bunker Display: %w", err)
	}
	return desktopDisplay(string(conf), fps, render, displayMode), nil
}

func desktopCommand(game, root, data string, display displaySettings, fullscreen bool) (*exec.Cmd, error) {
	width, height, err := renderDimensions(display.size)
	if err != nil {
		return nil, err
	}
	mode := "0"
	if fullscreen {
		mode = "1"
	}
	cmd := exec.Command(game, "-force-glcore45", "-screen-width", width, "-screen-height", height, "-screen-fullscreen", mode,
		"-logFile", filepath.Join(data, "player.log"))
	cmd.Env = append(os.Environ(), "REINCOMP_NATIVE_CHECKPOINT=1", "REINCOMP_DESKTOP_BUNDLES=1",
		"REINCOMP_INTERACTIVE_WINDOW=1", "REINCOMP_WINDOW_WIDTH="+width, "REINCOMP_WINDOW_HEIGHT="+height,
		"REINCOMP_INSTALL_ROOT="+root, "REINCOMP_PLAYER_DATA="+data, "REINCOMP_TARGET_FPS="+display.fps)
	if os.Getenv("REINCOMP_BENCHMARK_HIGHEST") != "1" {
		if display.textureSkip != "" {
			cmd.Args = append(cmd.Args, "-texture-mipmap-skip-level", display.textureSkip)
		}
		if display.effectsOff != 0 {
			cmd.Env = append(cmd.Env, "REINCOMP_BUNKER_EFFECTS_OFF="+strconv.Itoa(display.effectsOff))
		}
		if display.idle {
			cmd.Env = append(cmd.Env, "REINCOMP_BUNKER_IDLE=1")
		}
	}
	if display.fps != "game" {
		limit, err := strconv.Atoi(display.fps)
		if err != nil || limit < 1 || limit > 1000 {
			return nil, fmt.Errorf("invalid frame rate %q (use game or 1 to 1000)", display.fps)
		}
		if limit > 120 && limit <= 165 {
			cmd.Env = append(cmd.Env, "REINCOMP_NATIVE_PRESENT_RATE="+display.fps)
		}
	}
	if fullscreen {
		cmd.Args = append(cmd.Args, "-window-mode", "borderless")
		cmd.Env = append(cmd.Env, "REINCOMP_NATIVE_FULLSCREEN=1")
	}
	if display.fsr31 {
		cmd.Env = append(cmd.Env, "REINCOMP_FSR31=1", "REINCOMP_FSR_QUALITY="+strconv.Itoa(display.fsrQuality),
			"REINCOMP_FSR_SHARPNESS="+strconv.Itoa(display.sharpness))
	}
	return cmd, nil
}
