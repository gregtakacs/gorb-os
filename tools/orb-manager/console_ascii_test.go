package main

import (
	"go/ast"
	"go/parser"
	"go/token"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"unicode"
)

// The hub prints to a console on a machine whose default code page is not UTF-8,
// where an em dash arrives as "Â¿Â?". Browsers are fine, so this rule covers only
// what reaches stdout/stderr: log.* and fmt.Print* / Fprint-to-std*.
//
// Checked through the parser rather than with grep, so a string in a comment or a
// line continuation cannot produce a false hit, and so a test fixture may keep the
// exact bytes it captured from a device.
func TestConsoleOutputIsASCII(t *testing.T) {
	consoleFuncs := map[string]bool{
		"log.Print": true, "log.Printf": true, "log.Println": true,
		"log.Fatal": true, "log.Fatalf": true, "log.Fatalln": true,
		"fmt.Print": true, "fmt.Printf": true, "fmt.Println": true,
	}
	// Sprintf results reach the console through logf()/send-style helpers that were
	// themselves measured as console text; check those too.
	fmtTarget := map[string]bool{"os.Stdout": true, "os.Stderr": true}

	fset := token.NewFileSet()
	err := filepath.Walk(".", func(path string, info os.FileInfo, err error) error {
		if err != nil || info.IsDir() {
			return err
		}
		if !strings.HasSuffix(path, ".go") || strings.HasSuffix(path, "_test.go") {
			return nil
		}
		f, perr := parser.ParseFile(fset, path, nil, parser.ParseComments)
		if perr != nil {
			return perr
		}
		ast.Inspect(f, func(n ast.Node) bool {
			call, ok := n.(*ast.CallExpr)
			if !ok {
				return true
			}
			sel, ok := call.Fun.(*ast.SelectorExpr)
			if !ok {
				return true
			}
			pkg, ok := sel.X.(*ast.Ident)
			if !ok {
				return true
			}
			name := pkg.Name + "." + sel.Sel.Name
			check := consoleFuncs[name]
			if !check && strings.HasPrefix(name, "fmt.Fprint") {
				// fmt.Fprintln(os.Stderr, ...) — only the std streams reach a console.
				if len(call.Args) > 0 {
					if e, ok := call.Args[0].(*ast.SelectorExpr); ok {
						if x, ok := e.X.(*ast.Ident); ok {
							check = fmtTarget[x.Name+"."+e.Sel.Name]
						}
					}
				}
			}
			if !check {
				return true
			}
			for _, arg := range call.Args {
				lit, ok := arg.(*ast.BasicLit)
				if !ok || lit.Kind != token.STRING {
					continue
				}
				v := strings.Trim(lit.Value, "`\"")
				for _, r := range v {
					if r > unicode.MaxASCII {
						t.Errorf("%v: console string %q carries %q (%c); the Windows console renders it as mojibake",
							fset.Position(lit.Pos()), truncate(v), r, r)
						break
					}
				}
			}
			return true
		})
		return nil
	})
	if err != nil {
		t.Fatalf("walk: %v", err)
	}
}

func TestConsoleOutputIsASCII_ESPromLogf(t *testing.T) {
	// internal/esprom logs through a caller-supplied logf, so the AST walk above
	// cannot see those call sites. Check the literal arguments by text there.
	err := filepath.Walk("internal/esprom", func(path string, info os.FileInfo, err error) error {
		if err != nil || info.IsDir() || !strings.HasSuffix(path, ".go") || strings.HasSuffix(path, "_test.go") {
			return err
		}
		src, rerr := os.ReadFile(path)
		if rerr != nil {
			return rerr
		}
		for i, line := range strings.Split(string(src), "\n") {
			st := strings.TrimSpace(line)
			if !strings.HasPrefix(st, "logf(") && !strings.HasPrefix(st, "logf :=") {
				continue
			}
			for _, part := range strings.Split(st, `"`) {
				// Odd-indexed chunks are outside the quotes we care about; simpler to
				// test every chunk and ignore ones that are pure separators.
				if hasNonASCII(part) {
					t.Errorf("%s:%d: logf text is not console-safe: %s", path, i+1, truncate(part))
				}
			}
		}
		return nil
	})
	if err != nil {
		t.Fatalf("walk: %v", err)
	}
}

func hasNonASCII(s string) bool {
	for _, r := range s {
		if r > unicode.MaxASCII {
			return true
		}
	}
	return false
}

func truncate(s string) string {
	if len(s) > 60 {
		return s[:60] + "..."
	}
	return s
}
