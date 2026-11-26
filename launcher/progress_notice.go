package main

import (
	"sync"
	"time"
)

// openProgressWindow shows a small status window and returns functions that
// change its text and close it. Platforms without one keep the default.
var openProgressWindow = func(string) (func(string), func()) { return func(string) {}, func() {} }

// A progressNotice opens a status window only when a step outlasts its delay,
// so a step that finishes at once never flashes a window.
type progressNotice struct {
	mu     sync.Mutex
	open   func(string) (func(string), func())
	timer  *time.Timer
	text   string
	set    func(string)
	close  func()
	closed bool
}

func newProgressNotice(delay time.Duration, open func(string) (func(string), func())) *progressNotice {
	n := &progressNotice{open: open}
	n.timer = time.AfterFunc(delay, func() {
		n.mu.Lock()
		defer n.mu.Unlock()
		if !n.closed && n.set == nil {
			n.set, n.close = n.open(n.text)
		}
	})
	return n
}

func (n *progressNotice) update(text string) {
	n.mu.Lock()
	defer n.mu.Unlock()
	if n.closed || text == n.text {
		return
	}
	n.text = text
	if n.set != nil {
		n.set(text)
	}
}

func (n *progressNotice) finish() {
	n.mu.Lock()
	defer n.mu.Unlock()
	if n.closed {
		return
	}
	n.closed = true
	n.timer.Stop()
	if n.close != nil {
		n.close()
	}
}
