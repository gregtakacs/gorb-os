package main

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"fmt"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"
)

// fakeBrowser is a DevTools browser endpoint on loopback: it answers the
// websocket handshake on /devtools/browser/<id> and hands every client frame to
// reply, which answers with the frames to send (nil = say nothing) and whether
// to drop the connection after them.
type fakeBrowser struct {
	port int
	got  chan string
}

func startFakeBrowser(t *testing.T, refuse bool, reply func(msg string) (frames [][]byte, drop bool)) *fakeBrowser {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { ln.Close() })
	fb := &fakeBrowser{port: ln.Addr().(*net.TCPAddr).Port, got: make(chan string, 4)}
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			go func(c net.Conn) {
				defer c.Close()
				br := bufio.NewReader(c)
				req, err := http.ReadRequest(br)
				if err != nil {
					return
				}
				if refuse || !strings.HasPrefix(req.URL.Path, "/devtools/browser/") || req.Header.Get("Origin") != "" {
					fmt.Fprint(c, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
					return
				}
				sum := sha1.Sum([]byte(req.Header.Get("Sec-WebSocket-Key") + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
				fmt.Fprintf(c, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",
					base64.StdEncoding.EncodeToString(sum[:]))
				op, payload, err := readFrame(br)
				if err != nil || op != 0x1 {
					return
				}
				fb.got <- string(payload)
				frames, drop := reply(string(payload))
				for _, f := range frames {
					c.Write(f)
				}
				if !drop {
					time.Sleep(5 * time.Second) // a browser that never goes away
				}
			}(c)
		}
	}()
	return fb
}

// serverText is one unmasked server-to-client text frame.
func serverText(s string) []byte { return append([]byte{0x81, byte(len(s))}, s...) }

func profileWithEndpoint(t *testing.T, port int, path string) string {
	t.Helper()
	dir := t.TempDir()
	body := strconv.Itoa(port) + "\n" + path + "\n"
	if err := os.WriteFile(filepath.Join(dir, devtoolsPortFile), []byte(body), 0o600); err != nil {
		t.Fatal(err)
	}
	return dir
}

const browserPath = "/devtools/browser/4f0e2c1a-7b7e-4d55-9f7b-2d0c3a1e9b11"

// The owner's report (2026-11-18): on macOS the hub's browser outlived the hub.
// The close goes to the hub's own endpoint, as Browser.close, with no Origin.
func TestCloseAppBrowserSendsBrowserClose(t *testing.T) {
	fb := startFakeBrowser(t, false, func(string) ([][]byte, bool) {
		return [][]byte{serverText(`{"id":1,"result":{}}`)}, false
	})
	start := time.Now()
	if err := closeAppBrowser(profileWithEndpoint(t, fb.port, browserPath), 2*time.Second); err != nil {
		t.Fatalf("closeAppBrowser: %v", err)
	}
	if d := time.Since(start); d > time.Second {
		t.Fatalf("an answered close took %s: it must return on the answer, not the deadline", d)
	}
	if got := <-fb.got; got != `{"id":1,"method":"Browser.close"}` {
		t.Fatalf("sent %q", got)
	}
}

// A browser that quits before it answers drops the socket: that is the close
// landing, not a failure.
func TestCloseAppBrowserTakesADroppedConnectionAsClosed(t *testing.T) {
	fb := startFakeBrowser(t, false, func(string) ([][]byte, bool) { return nil, true })
	if err := closeAppBrowser(profileWithEndpoint(t, fb.port, browserPath), 2*time.Second); err != nil {
		t.Fatalf("closeAppBrowser: %v", err)
	}
}

func TestCloseAppBrowserReportsARefusal(t *testing.T) {
	fb := startFakeBrowser(t, true, nil)
	if err := closeAppBrowser(profileWithEndpoint(t, fb.port, browserPath), 2*time.Second); err == nil {
		t.Fatal("a refused handshake must be an error, so the caller falls back")
	}
	fb2 := startFakeBrowser(t, false, func(string) ([][]byte, bool) {
		return [][]byte{serverText(`{"id":1,"error":{"code":-32000,"message":"no"}}`)}, false
	})
	if err := closeAppBrowser(profileWithEndpoint(t, fb2.port, browserPath), 2*time.Second); err == nil {
		t.Fatal("an error answer must be an error")
	}
}

// A browser that takes the frame and says nothing must not hold the hub past the budget.
func TestCloseAppBrowserIsBoundedByItsBudget(t *testing.T) {
	fb := startFakeBrowser(t, false, func(string) ([][]byte, bool) { return nil, false })
	start := time.Now()
	err := closeAppBrowser(profileWithEndpoint(t, fb.port, browserPath), 300*time.Millisecond)
	if err == nil {
		t.Fatal("a silent browser is not a closed one")
	}
	if d := time.Since(start); d > 1500*time.Millisecond {
		t.Fatalf("waited %s on a 300 ms budget", d)
	}
}

func TestCloseAppBrowserOnADeadPortReturnsAtOnce(t *testing.T) {
	ln, _ := net.Listen("tcp", "127.0.0.1:0")
	port := ln.Addr().(*net.TCPAddr).Port
	ln.Close()
	start := time.Now()
	if err := closeAppBrowser(profileWithEndpoint(t, port, browserPath), 2*time.Second); err == nil {
		t.Fatal("nothing is listening: that is an error")
	}
	if d := time.Since(start); d > time.Second {
		t.Fatalf("a refused connect took %s", d)
	}
}

// Only the endpoint the hub's own profile published, and only its browser target.
func TestBrowserEndpointReadsOnlyTheBrowserTarget(t *testing.T) {
	if _, _, ok := browserEndpoint(""); ok {
		t.Fatal("no profile (the owner's default): nothing to close")
	}
	if _, _, ok := browserEndpoint(t.TempDir()); ok {
		t.Fatal("no DevToolsActivePort: nothing to close")
	}
	for _, body := range []string{
		"9222\n",                                 // no path
		"9222\n/devtools/page/abc\n",             // a page, not the browser
		"0\n" + browserPath + "\n",               // port out of range
		"70000\n" + browserPath + "\n",           // port out of range
		"x\n" + browserPath + "\n",               // not a port
		"9222\n" + browserPath + " extra\n",      // a path that would smuggle header text
		"9222\n" + browserPath + "?q=1\n",        // query
		"9222\nhttp://evil/devtools/browser/x\n", // not a path
	} {
		dir := t.TempDir()
		os.WriteFile(filepath.Join(dir, devtoolsPortFile), []byte(body), 0o600)
		if _, _, ok := browserEndpoint(dir); ok {
			t.Errorf("accepted %q", body)
		}
	}
	port, path, ok := browserEndpoint(profileWithEndpoint(t, 9222, browserPath))
	if !ok || port != 9222 || path != browserPath {
		t.Fatalf("got %d %q %v", port, path, ok)
	}
}

// No endpoint answering, and the browser the hub started still running: the
// closer ends that process, and only once however often it is called.
func TestCloseBrowserFallsBackToTheProcessItStarted(t *testing.T) {
	mark := filepath.Join(t.TempDir(), "ended")
	stub := writeStubBrowser(t, "trap 'echo term > "+mark+"; exit 0' TERM; while :; do sleep 0.05; done")
	old := browserCloseBudget
	browserCloseBudget = 200 * time.Millisecond
	t.Cleanup(func() { browserCloseBudget = old })
	w, err := startAppWindow("http://127.0.0.1:9", stub)
	if err != nil {
		t.Fatalf("startAppWindow: %v", err)
	}
	time.Sleep(200 * time.Millisecond) // let the stub install its trap
	w.closeBrowser()
	w.closeBrowser() // idempotent
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if b, err := os.ReadFile(mark); err == nil && strings.TrimSpace(string(b)) == "term" {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatal("the browser process the hub started was never asked to end")
}
