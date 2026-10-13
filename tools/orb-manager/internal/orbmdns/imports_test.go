package orbmdns

import (
	"context"
	"errors"
	"go/ast"
	"go/parser"
	"go/token"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
)

// TestThisPackageCannotDial is the structural half of "list candidates without
// dialing them". A promise you can only check by reading the file is a promise a
// refactor breaks silently, so this reads the package's own import blocks and fails
// if anything that can open a connection to an orb has been added to it.
//
// net/http is the dial the hub already owns elsewhere; orbnet is the client that
// owns it. A browser that reaches for either has become the thing §5.1 point 1 says
// discovery must replace — a hub that knocks on an address a stranger mentioned.
func TestThisPackageCannotDial(t *testing.T) {
	banned := []string{"net/http", "net/rpc", "orbnet"}
	files, err := filepath.Glob("*.go")
	if err != nil || len(files) == 0 {
		t.Fatalf("no sources to inspect: %v", err)
	}
	checked := 0
	for _, f := range files {
		if strings.HasSuffix(f, "_test.go") {
			continue // a test may dial anything it likes; the shipped code may not
		}
		src, err := os.ReadFile(f)
		if err != nil {
			t.Fatalf("read %s: %v", f, err)
		}
		fset := token.NewFileSet()
		file, err := parser.ParseFile(fset, f, src, parser.ImportsOnly)
		if err != nil {
			t.Fatalf("parse %s: %v", f, err)
		}
		checked++
		for _, imp := range file.Imports {
			path, _ := strconv.Unquote(imp.Path.Value)
			for _, b := range banned {
				if path == b || strings.Contains(path, "/"+b+"/") || strings.HasSuffix(path, "/"+b) {
					t.Errorf("%s imports %s: the browser must not be able to reach an orb at all", f, path)
				}
			}
		}
	}
	if checked == 0 {
		t.Fatal("no non-test sources: this guard is inspecting nothing")
	}
}

// TestBrowserHasNoUnicastTargetField is the other half of the same promise, aimed at
// the bench. This machine cannot originate multicast onto the LAN, so the browse on
// air is verified passively from an orb's own announce burst; the temptation, having
// shipped browse_mdns.py with a --unicast-out flag to work around exactly that, is to
// grow the same knob here. It must stay a bench script's problem: a browser that can
// be aimed at one host is a neighbour-discovery tool with extra steps, and the
// firmware's reason for the workaround (no IP_PKTINFO, one socket, two joins) is a
// fact about lwIP that the hub does not share.
func TestBrowserHasNoUnicastTargetField(t *testing.T) {
	src, err := os.ReadFile("browse.go")
	if err != nil {
		t.Fatalf("read browse.go: %v", err)
	}
	fset := token.NewFileSet()
	file, err := parser.ParseFile(fset, "browse.go", src, parser.ParseComments)
	if err != nil {
		t.Fatalf("parse browse.go: %v", err)
	}
	for _, d := range file.Decls {
		gd, ok := d.(*ast.GenDecl)
		if !ok || gd.Tok != token.TYPE {
			continue
		}
		for _, spec := range gd.Specs {
			ts, ok := spec.(*ast.TypeSpec)
			if !ok || ts.Name.Name != "Browser" {
				continue
			}
			st, ok := ts.Type.(*ast.StructType)
			if !ok {
				continue
			}
			for _, fld := range st.Fields.List {
				for _, n := range fld.Names {
					low := strings.ToLower(n.Name)
					if strings.Contains(low, "target") || strings.Contains(low, "unicast") ||
						strings.Contains(low, "peer") || strings.Contains(low, "host") && low != "hosts" {
						t.Errorf("Browser has a field %q that could aim a query at one host", n.Name)
					}
				}
			}
		}
	}
}

// TestAskOnlyWritesToTheGroup is the executable version of the same rule: it runs
// Ask against a source that records every destination and fails if anything other
// than the mDNS group was ever dialed.
func TestAskOnlyWritesToTheGroup(t *testing.T) {
	rec := &recordingSource{}
	b := NewBrowser(Config{})
	if err := b.Start(context.Background(), rec); err != nil {
		t.Fatalf("start: %v", err)
	}
	defer b.Close()
	// Seed one half-announced instance, so Ask emits the follow-ups as well as the
	// browse: a PTR heard without its SRV/TXT/A is the ordinary case for a hub that
	// started after the orb's one announce burst.
	b.note(goodInst, goodIP, func(p *partial) {})
	if err := b.Ask(); err != nil {
		t.Fatalf("ask: %v", err)
	}
	sent := rec.snapshots()
	if len(sent) < 4 {
		t.Fatalf("Ask emitted %d packets; a browse plus the SRV/TXT/A follow-ups is 4", len(sent))
	}
	for _, s := range sent {
		if s.to != GroupAddr {
			t.Errorf("Ask wrote to %s: the browser must only ever address the group", s.to)
		}
	}
}

// TestWindowsSocketBindsWildcardNotGroup is the third structural half of the same
// promise, aimed at the platform the Unix guards cannot see. A live orb answers a
// browse with a UNICAST packet to whoever asked (orb_mdns.c replies to the query's
// source, not the group), so a browser whose socket is bound to the group address
// discards every answer to a query it sent itself and refreshes only on a reboot —
// the symptom that reads as "mDNS is broken" to a Windows operator but never shows on
// the Linux box the other guards were written on. net.ListenMulticastUDP binds exactly
// that group address. This fails the moment mcastsock_windows.go reaches for it, so the
// trap cannot silently return on the one platform most operators use.
func TestWindowsSocketBindsWildcardNotGroup(t *testing.T) {
	src, err := os.ReadFile("mcastsock_windows.go")
	if err != nil {
		t.Fatalf("read mcastsock_windows.go: %v", err)
	}
	fset := token.NewFileSet()
	file, err := parser.ParseFile(fset, "mcastsock_windows.go", src, 0)
	if err != nil {
		t.Fatalf("parse mcastsock_windows.go: %v", err)
	}
	// Walk the real code, not the comments: this file's own doc says it must not call
	// ListenMulticastUDP, so a text match would fire on the explanation, not the fault.
	ast.Inspect(file, func(n ast.Node) bool {
		call, ok := n.(*ast.CallExpr)
		if !ok {
			return true
		}
		sel, ok := call.Fun.(*ast.SelectorExpr)
		if !ok {
			return true
		}
		if sel.Sel.Name == "ListenMulticastUDP" {
			t.Error("mcastsock_windows.go calls net.ListenMulticastUDP: it binds the socket to the group, " +
				"which discards the unicast answer an orb sends back and makes live discovery reboot-only")
		}
		return true
	})
	// The wildcard bind is what the AST cannot infer, so check for the literal once more.
	if !strings.Contains(string(src), "0.0.0.0") {
		t.Error("mcastsock_windows.go must bind the wildcard 0.0.0.0, the only address that receives " +
			"an orb's unicast answer to the hub's own LAN IP")
	}
}

// recordingSource is the Source a test hands the browser: it records every
// destination Ask aimed at, and delivers nothing, so the only packets on the wire
// are the ones the browser chose to send.
type recordingSource struct {
	mu   sync.Mutex
	sent []sentPacket
}

type sentPacket struct {
	to string
	n  int
}

func (r *recordingSource) ReadFrom(p []byte) (int, net.Addr, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if len(r.sent) > 0 {
		return 0, nil, errIdle // let the pump exit once the test has its writes
	}
	return 0, nil, errIdle
}

func (r *recordingSource) WriteTo(p []byte, addr net.Addr) (int, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	to := ""
	if ua, ok := addr.(*net.UDPAddr); ok {
		to = ua.IP.String()
	}
	r.sent = append(r.sent, sentPacket{to: to, n: len(p)})
	return len(p), nil
}

func (r *recordingSource) Close() error { return nil }

func (r *recordingSource) snapshots() []sentPacket {
	r.mu.Lock()
	defer r.mu.Unlock()
	return append([]sentPacket(nil), r.sent...)
}

var errIdle = errors.New("orbmdns/test: no datagram")
