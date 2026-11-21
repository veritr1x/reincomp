package main

import (
	"context"
	"crypto/subtle"
	"errors"
	"log"
	"net"
	"net/http"
	"os/exec"
	"time"
)

// Only the packaged Pod Programs service receives this loopback address and token.
// The game window can close during a calendar edit while the editor and its
// Bunker server remain alive. Closing the window normally still ends the launcher.
type playerSession struct {
	url          string
	server       *http.Server
	requests     chan playerRequest
	closed       chan struct{}
	editorGone   chan struct{}
	applyDisplay func(pid int) error
	started      func(pid int)
	windowClosed <-chan struct{}
}

type playerRequest struct {
	stop    bool
	display bool
	result  chan error
}

var closePlayerWindow = func(pid int) bool { return false }
var resizePlayerWindow = func(pid int, size string) error { return errors.New("the native player is unavailable") }

func openPlayerSession(token string) (*playerSession, error) {
	listener, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		return nil, err
	}
	address := listener.Addr().String()
	s := &playerSession{url: "http://" + address, requests: make(chan playerRequest), closed: make(chan struct{}), editorGone: make(chan struct{})}
	s.server = &http.Server{ReadHeaderTimeout: 5 * time.Second, Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Host != address || subtle.ConstantTimeCompare([]byte(r.Header.Get("X-Bunker-Token")), []byte(token)) != 1 {
			http.Error(w, "Forbidden", http.StatusForbidden)
			return
		}
		if r.Method != http.MethodPost || (r.URL.Path != "/stop" && r.URL.Path != "/start" && r.URL.Path != "/display") {
			http.NotFound(w, r)
			return
		}
		req := playerRequest{stop: r.URL.Path == "/stop", display: r.URL.Path == "/display", result: make(chan error, 1)}
		// A disconnected browser does not own the game process. The local editor
		// finishes the accepted stop/edit/start sequence on its own connection.
		ctx, cancel := context.WithTimeout(r.Context(), 45*time.Second)
		defer cancel()
		select {
		case s.requests <- req:
		case <-s.closed:
			http.Error(w, "Launcher is closing", http.StatusServiceUnavailable)
			return
		case <-ctx.Done():
			http.Error(w, "Player did not respond", http.StatusGatewayTimeout)
			return
		}
		select {
		case err := <-req.result:
			if err != nil {
				http.Error(w, err.Error(), http.StatusConflict)
				return
			}
			w.WriteHeader(http.StatusNoContent)
		case <-s.closed:
			http.Error(w, "Launcher is closing", http.StatusServiceUnavailable)
		case <-ctx.Done():
			http.Error(w, "Player did not respond", http.StatusGatewayTimeout)
		}
	})}
	go s.server.Serve(listener)
	return s, nil
}

func (s *playerSession) close() {
	close(s.closed)
	s.server.Close()
}

func (s *playerSession) play(makePlayer func() (*exec.Cmd, error)) error {
	var restart *playerRequest
	for {
		cmd, err := makePlayer()
		if err == nil {
			err = cmd.Start()
		}
		if restart != nil {
			restart.result <- err
			restart = nil
		}
		if err != nil {
			return err
		}
		if s.started != nil {
			s.started(cmd.Process.Pid)
		}
		exited := make(chan error, 1)
		go func() { exited <- cmd.Wait() }()
	playing:
		for {
			select {
			case err := <-exited:
				return err
			case <-s.windowClosed:
				if !closePlayerWindow(cmd.Process.Pid) {
					cmd.Process.Kill()
				}
				<-exited
				return nil
			case req := <-s.requests:
				if req.display {
					req.result <- s.applyDisplay(cmd.Process.Pid)
					continue
				}
				if !req.stop {
					req.result <- nil
					continue
				}
				if !closePlayerWindow(cmd.Process.Pid) {
					req.result <- errors.New("the game window is not ready to close")
					continue
				}
				select {
				case err := <-exited:
					req.result <- err
				case <-time.After(30 * time.Second):
					req.result <- errors.New("the game did not close in time")
					continue
				}
				break playing
			}
		}
		// Recover if the editor exits between stopping and restarting the player.
	stopped:
		for {
			select {
			case <-s.windowClosed:
				return nil
			case req := <-s.requests:
				if req.stop || req.display {
					req.result <- nil
					continue
				}
				restart = &req
				break stopped
			case <-s.editorGone:
				log.Print("Restarting the game after Pod Programs exited during the edit.")
				break stopped
			}
		}
	}
}
