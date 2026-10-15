package orbmdns

import (
	"go/ast"
	"go/parser"
	"go/token"
	"os"
	"path/filepath"
	"strconv"
	"strings"
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

// TestBrowserNeverSends is the structural half of "discovery runs on check-ins
// alone". The hub used to query on a timer, and the honest version of "the hub is a
// listener now" is not a doc comment saying so — it is a test that fails the package
// the moment a write call reappears anywhere in its non-test sources. Two checks,
// both by parse rather than text match: no method call named Write* anywhere, and a
// Source interface whose method set is exactly ReadFrom and Close, so there is no
// write method to reach even if a call were smuggled in through an embedded type.
func TestBrowserNeverSends(t *testing.T) {
	files, err := filepath.Glob("*.go")
	if err != nil || len(files) == 0 {
		t.Fatalf("no sources to inspect: %v", err)
	}
	checked := 0
	sourceMethods := []string{}
	for _, f := range files {
		if strings.HasSuffix(f, "_test.go") {
			continue // a test may hand in any double it likes; the shipped code may not
		}
		src, err := os.ReadFile(f)
		if err != nil {
			t.Fatalf("read %s: %v", f, err)
		}
		fset := token.NewFileSet()
		file, err := parser.ParseFile(fset, f, src, 0)
		if err != nil {
			t.Fatalf("parse %s: %v", f, err)
		}
		checked++
		ast.Inspect(file, func(n ast.Node) bool {
			switch x := n.(type) {
			case *ast.CallExpr:
				if sel, ok := x.Fun.(*ast.SelectorExpr); ok {
					name := sel.Sel.Name
					if strings.HasPrefix(name, "Write") || strings.HasPrefix(name, "Send") {
						t.Errorf("%s calls %s: the browser listens; it does not put packets on the wire", f, name)
					}
				}
			case *ast.TypeSpec:
				if x.Name.Name != "Source" {
					return true
				}
				iface, ok := x.Type.(*ast.InterfaceType)
				if !ok {
					t.Errorf("%s declares Source as something other than an interface", f)
					return true
				}
				for _, m := range iface.Methods.List {
					for _, n := range m.Names {
						sourceMethods = append(sourceMethods, n.Name)
					}
				}
			}
			return true
		})
	}
	if checked == 0 {
		t.Fatal("no non-test sources: this guard is inspecting nothing")
	}
	want := []string{"ReadFrom", "Close"}
	if len(sourceMethods) != len(want) {
		t.Fatalf("Source has methods %v: the browser's socket must expose exactly %v — no way to send exists by construction", sourceMethods, want)
	}
	for i, m := range want {
		if sourceMethods[i] != m {
			t.Errorf("Source method %d is %q, want %q", i, sourceMethods[i], m)
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
