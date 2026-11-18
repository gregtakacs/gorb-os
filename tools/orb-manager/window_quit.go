package main

import (
	"bufio"
	"crypto/rand"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// --- quitting the hub's own browser ------------------------------------------
//
// The app window runs in a browser on the hub's private profile (profileDirFor),
// which Chrome treats as a separate instance. On Windows and Linux that instance
// exits with its last window. On macOS it does not: an app with no windows keeps
// running, so after the hub quits, a Chrome with the hub's profile sits in the
// Dock, and the next "open Chrome" can land in it instead of the owner's own
// profile — their bookmarks, logins and extensions missing (owner report,
// 2026-11-18). So the hub quits that instance on its way out.
//
// Only that instance. The browser is told to close through the DevTools
// endpoint it published in the hub's own profile dir (DevToolsActivePort's
// second line is the browser target's websocket path), so a browser on any other
// profile — the owner's — is unreachable from here by construction. A hub that
// ran on the browser's default profile (profileDirFor answered "") closes
// nothing.

// browserCloseBudget bounds the whole close: a hub on its way out must not hang
// on a browser that does not answer.
var browserCloseBudget = 3 * time.Second

// browserEndpoint reads the port and the browser websocket path Chrome wrote to
// the profile dir. ok is false for anything that is not exactly that shape.
func browserEndpoint(profileDir string) (port int, path string, ok bool) {
	if profileDir == "" {
		return 0, "", false
	}
	raw, err := os.ReadFile(filepath.Join(profileDir, devtoolsPortFile))
	if err != nil {
		return 0, "", false
	}
	first, rest, _ := strings.Cut(strings.TrimSpace(string(raw)), "\n")
	port, err = strconv.Atoi(strings.TrimSpace(first))
	if err != nil || port <= 0 || port > 65535 {
		return 0, "", false
	}
	path = strings.TrimSpace(rest)
	if !strings.HasPrefix(path, "/devtools/browser/") || strings.ContainsAny(path, " \r\n?#") {
		return 0, "", false
	}
	return port, path, true
}

// closeAppBrowser asks the browser on profileDir to quit (DevTools
// Browser.close), and answers nil once the browser accepted the command or
// dropped the connection doing so. It touches nothing when no endpoint is
// published there.
func closeAppBrowser(profileDir string, budget time.Duration) error {
	port, path, ok := browserEndpoint(profileDir)
	if !ok {
		return errors.New("no DevTools endpoint in the hub's profile")
	}
	deadline := time.Now().Add(budget)
	conn, err := net.DialTimeout("tcp", "127.0.0.1:"+strconv.Itoa(port), budget)
	if err != nil {
		return err
	}
	defer conn.Close()
	_ = conn.SetDeadline(deadline)

	// The opening handshake (RFC 6455 §4.1). No Origin header: Chrome refuses a
	// DevTools websocket whose Origin it was not told to allow, and a client
	// that sends none is the local tool it expects.
	var nonce [16]byte
	if _, err := rand.Read(nonce[:]); err != nil {
		return err
	}
	key := base64.StdEncoding.EncodeToString(nonce[:])
	host := "127.0.0.1:" + strconv.Itoa(port)
	if _, err := fmt.Fprintf(conn, "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"+
		"Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n", path, host, key); err != nil {
		return err
	}
	br := bufio.NewReader(conn)
	resp, err := http.ReadResponse(br, nil)
	if err != nil {
		return err
	}
	_ = resp.Body.Close()
	if resp.StatusCode != http.StatusSwitchingProtocols {
		return fmt.Errorf("DevTools refused the websocket: %s", resp.Status)
	}
	sum := sha1.Sum([]byte(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
	if resp.Header.Get("Sec-WebSocket-Accept") != base64.StdEncoding.EncodeToString(sum[:]) {
		return errors.New("DevTools answered the websocket with the wrong accept key")
	}

	if err := writeTextFrame(conn, []byte(`{"id":1,"method":"Browser.close"}`)); err != nil {
		return err
	}
	// The browser answers {"id":1,"result":{}} and then goes away; either one
	// is the close landing. Anything else before the deadline is read past.
	for {
		op, payload, err := readFrame(br)
		if err != nil {
			if errors.Is(err, io.EOF) || errors.Is(err, io.ErrUnexpectedEOF) || isConnReset(err) {
				return nil // the browser went away mid-answer: it is closing
			}
			return err
		}
		switch op {
		case 0x8: // close frame
			return nil
		case 0x1:
			if strings.Contains(string(payload), `"id":1`) {
				if strings.Contains(string(payload), `"error"`) {
					return fmt.Errorf("Browser.close refused: %s", payload)
				}
				return nil
			}
		}
	}
}

// writeTextFrame sends one masked text frame, as every client frame must be.
func writeTextFrame(w io.Writer, payload []byte) error {
	hdr := []byte{0x81} // FIN + text
	n := len(payload)
	switch {
	case n < 126:
		hdr = append(hdr, 0x80|byte(n))
	case n <= 0xffff:
		hdr = append(hdr, 0x80|126, byte(n>>8), byte(n))
	default:
		return errors.New("frame too large")
	}
	var mask [4]byte
	if _, err := rand.Read(mask[:]); err != nil {
		return err
	}
	hdr = append(hdr, mask[:]...)
	body := make([]byte, n)
	for i := range payload {
		body[i] = payload[i] ^ mask[i%4]
	}
	_, err := w.Write(append(hdr, body...))
	return err
}

// frameMax bounds one server frame we will buffer: the answer to Browser.close
// is a few bytes, and an endpoint that claims more is not one worth reading.
const frameMax = 1 << 20

// readFrame reads one (unmasked, server-to-client) frame.
func readFrame(r *bufio.Reader) (op byte, payload []byte, err error) {
	var h [2]byte
	if _, err = io.ReadFull(r, h[:]); err != nil {
		return 0, nil, err
	}
	op = h[0] & 0x0f
	n := uint64(h[1] & 0x7f)
	switch n {
	case 126:
		var b [2]byte
		if _, err = io.ReadFull(r, b[:]); err != nil {
			return 0, nil, err
		}
		n = uint64(binary.BigEndian.Uint16(b[:]))
	case 127:
		var b [8]byte
		if _, err = io.ReadFull(r, b[:]); err != nil {
			return 0, nil, err
		}
		n = binary.BigEndian.Uint64(b[:])
	}
	if n > frameMax {
		return 0, nil, fmt.Errorf("DevTools frame of %d bytes", n)
	}
	var mask [4]byte
	masked := h[1]&0x80 != 0
	if masked {
		if _, err = io.ReadFull(r, mask[:]); err != nil {
			return 0, nil, err
		}
	}
	payload = make([]byte, n)
	if _, err = io.ReadFull(r, payload); err != nil {
		return 0, nil, err
	}
	if masked {
		for i := range payload {
			payload[i] ^= mask[i%4]
		}
	}
	return op, payload, nil
}

func isConnReset(err error) bool {
	var ne *net.OpError
	return errors.As(err, &ne) && !ne.Timeout()
}
