package main

import "testing"

func TestCacheProgress(t *testing.T) {
	for line, want := range map[string]string{
		"progress 0":   "Checking game files for cache sharing · 0%",
		"progress 42":  "Checking game files for cache sharing · 42%",
		"progress 100": "Checking game files for cache sharing · 100%",
	} {
		if got, ok := cacheProgress(line); !ok || got != want {
			t.Errorf("cacheProgress(%q) = %q, %v; want %q", line, got, ok, want)
		}
	}
	for _, line := range []string{"", "Linked 12 verified Octo files", "progress", "progress x", "progress 101", "progress -1", "progress 5 extra"} {
		if got, ok := cacheProgress(line); ok {
			t.Errorf("cacheProgress(%q) = %q; want it left for the log", line, got)
		}
	}
}
