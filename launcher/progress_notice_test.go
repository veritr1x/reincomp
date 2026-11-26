package main

import (
	"sync"
	"testing"
	"time"
)

type fakeNotice struct {
	mu     sync.Mutex
	texts  []string
	opened int
	closed int
}

func (f *fakeNotice) open(text string) (func(string), func()) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.opened++
	f.texts = append(f.texts, text)
	return func(s string) { f.mu.Lock(); f.texts = append(f.texts, s); f.mu.Unlock() },
		func() { f.mu.Lock(); f.closed++; f.mu.Unlock() }
}

func (f *fakeNotice) state() (int, int, []string) {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.opened, f.closed, append([]string(nil), f.texts...)
}

func TestProgressNoticeStaysHiddenForShortSteps(t *testing.T) {
	fake := &fakeNotice{}
	notice := newProgressNotice(time.Hour, fake.open)
	notice.update("Checking game files for cache sharing · 3%")
	notice.finish()
	if opened, closed, _ := fake.state(); opened != 0 || closed != 0 {
		t.Fatalf("a step that finished at once opened %d and closed %d windows", opened, closed)
	}
}

func TestProgressNoticeShowsLatestTextAfterDelay(t *testing.T) {
	fake := &fakeNotice{}
	notice := newProgressNotice(10*time.Millisecond, fake.open)
	notice.update("Checking game files for cache sharing · 3%")
	deadline := time.Now().Add(2 * time.Second)
	for {
		if opened, _, _ := fake.state(); opened == 1 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("the window never opened")
		}
		time.Sleep(5 * time.Millisecond)
	}
	notice.update("Checking game files for cache sharing · 4%")
	notice.update("Checking game files for cache sharing · 4%")
	notice.finish()
	notice.update("late")
	opened, closed, texts := fake.state()
	want := []string{"Checking game files for cache sharing · 3%", "Checking game files for cache sharing · 4%"}
	if opened != 1 || closed != 1 || len(texts) != len(want) || texts[0] != want[0] || texts[1] != want[1] {
		t.Fatalf("opened %d, closed %d, texts %q", opened, closed, texts)
	}
}
