package main

import (
	"strings"
	"testing"
)

// A smoke-check that stands in for the JS parser this machine does not have. It is NOT
// a JavaScript grammar: it strips comments and quoted spans, then requires the
// grouping characters to balance. That catches the realistic edit mistakes — a
// dropped brace, an unclosed template literal, a stray paren from a botched refactor —
// which otherwise surface only as a blank card in the browser. The README says plainly
// that the client has no parser here; this is the mitigation, not a substitute.

// stripJSLiterals removes // and /* */ comments, '...' and "..." strings, `...`
// template literals (with the ${...} inside them, since the whole span goes), and
// /.../ regex literals. Grouping characters that survive are code.
//
// The regex case is not pedantry: esc() contains the literal /[&<>"]/g, and without
// this the quote inside that pattern opened a "string" that swallowed the rest of the
// line and reported a balanced file as unbalanced. A checker that lies about the code
// is worse than no checker, so it handles the case or it does not run.
func stripJSLiterals(src string) string {
	var out strings.Builder
	// lastCode remembers the previous significant character, which is what decides
	// whether a '/' can begin a regex at all: after a value it is a division.
	lastCode := byte('\n')
	for i := 0; i < len(src); {
		c := src[i]
		switch {
		case c == '/' && i+1 < len(src) && src[i+1] == '/':
			for i < len(src) && src[i] != '\n' {
				i++
			}
		case c == '/' && i+1 < len(src) && src[i+1] == '*':
			i += 2
			for i+1 < len(src) && !(src[i] == '*' && src[i+1] == '/') {
				i++
			}
			i += 2
		case c == '/' && regexAllowedAfter(lastCode):
			end, ok := skipRegex(src, i)
			if !ok {
				// Not a real regex (an unterminated pattern): treat it as code and
				// carry on rather than eating the rest of the file.
				out.WriteByte(c)
				lastCode = c
				i++
			} else {
				i = end
			}
		case c == '\'' || c == '"':
			q := c
			i++
			for i < len(src) && src[i] != q {
				if src[i] == '\\' {
					i++
				}
				i++
			}
			i++
			lastCode = q
		case c == '`':
			i++
			for i < len(src) && src[i] != '`' {
				if src[i] == '\\' {
					i++
				}
				i++
			}
			i++
			lastCode = '`'
		case c == ' ' || c == '\t' || c == '\r':
			i++
		default:
			out.WriteByte(c)
			lastCode = c
			i++
		}
	}
	return out.String()
}

// regexAllowedAfter is the classic positional test: a slash starts a pattern where a
// value cannot end, and a division where one can.
func regexAllowedAfter(b byte) bool {
	switch b {
	case '(', ',', '=', ':', '[', '!', '&', '|', '?', '{', '}', ';', '\n', '+', '%', '<', '>', '*':
		return true
	}
	return false
}

// skipRegex consumes a /.../...flags/ literal from src[i], returning the index just
// past it. A '[' class may hold an unescaped '/', which is why the class is tracked.
func skipRegex(src string, i int) (int, bool) {
	j := i + 1
	inClass := false
	for j < len(src) {
		switch src[j] {
		case '\\':
			j += 2
			continue
		case '\n':
			return i, false
		case '[':
			inClass = true
		case ']':
			inClass = false
		case '/':
			if inClass {
				break
			}
			j++
			for j < len(src) && ((src[j] >= 'a' && src[j] <= 'z') || (src[j] >= 'A' && src[j] <= 'Z')) {
				j++ // flags
			}
			return j, true
		}
		j++
	}
	return i, false
}

func TestAppJSGroupsBalance(t *testing.T) {
	code := stripJSLiterals(readAppJS(t))
	for _, pair := range [][2]byte{{'{', '}'}, {'(', ')'}, {'[', ']'}} {
		open, close := strings.Count(code, string(pair[0])), strings.Count(code, string(pair[1]))
		if open != close {
			t.Errorf("app.js is unbalanced in %q: %d open, %d close (after comments and literals are removed)",
				string(pair[0]), open, close)
		}
	}
	// The file must still be recognisable as this client, or the balance check above
	// is passing on an empty string.
	if !strings.Contains(code, "function") {
		t.Error("stripping literals left nothing recognisable; the checker is not testing what it thinks")
	}
}

// The checker has to be able to fail, or it proves nothing. Each of these is a real
// JS fragment with exactly one defect of the kind a bad edit makes; all four must be
// caught, and the clean one must pass. Without this, the balance test above is a
// green light that any string-handling bug could walk through.
func TestJSCheckerCatchesBrokenCode(t *testing.T) {
	cases := []struct {
		name     string
		src      string
		balanced bool
	}{
		{"clean", "function a(x) { const s = `${x[0]}`; return {y: [1, 2]}; }", true},
		{"dropped brace", "function a(x) { if (x) { return 1; }", false},
		{"dropped paren", "function a(x) { return foo(x; }", false},
		{"unclosed template", "function a() { return `oops{[}", false},
		{"regex with braces and a quote", `function esc(s){ return s.replace(/[&<>"]/g, (c) => ({ "&": "&amp;" }[c])); }`, true},
		{"comment holding braces", "function a() { /* }{ */ return {}; }", true},
		{"division is not a regex", "function a(x, y) { return x / y; }", true},
	}
	for _, tc := range cases {
		code := stripJSLiterals(tc.src)
		ok := strings.Count(code, "{") == strings.Count(code, "}") &&
			strings.Count(code, "(") == strings.Count(code, ")") &&
			strings.Count(code, "[") == strings.Count(code, "]")
		if ok != tc.balanced {
			t.Errorf("%s: stripJSLiterals judged %q balanced=%v, want %v (stripped %q)", tc.name, tc.src, ok, tc.balanced, code)
		}
	}
}

// A checker that reports the file fine after deleting half of it is worthless, so say
// what it is for in the failure text and keep the size floor honest.
func TestAppJSCoversTheWholeClient(t *testing.T) {
	src := readAppJS(t)
	if len(src) < 20000 {
		t.Errorf("app.js is only %d B; the client has been truncated or the wrong file is embedded", len(src))
	}
	// The functions the cards call by name must exist, not merely be mentioned.
	for _, fn := range []string{"function refreshPorts", "function streamFlash", "function renderIdentity",
		"function loadThemes", "function themeRow", "function readConsole", "function startPolling",
		"function prefillOrbIP", "function fillConsolePorts", "function esc", "function hubFetch"} {
		if !strings.Contains(src, fn) {
			t.Errorf("app.js has no %s", fn)
		}
	}
}

// The same check on the page: an unclosed tag silently removes a card the client still
// expects to find, which is the failure the selector test above depends on.
func TestIndexHTMLTagsBalance(t *testing.T) {
	page := readWebFile(t, "index.html")
	for _, tag := range []string{"section", "article", "template", "div", "select"} {
		open := len(strings.Split(page, "<"+tag)) - 1
		close := len(strings.Split(page, "</"+tag+">")) - 1
		if open != close {
			t.Errorf("index.html has %d <%s> opening tags and %d closing", open, tag, close)
		}
	}
	if n := strings.Count(page, `id="`); n < 20 {
		t.Errorf("index.html has only %d ids; the page looks truncated", n)
	}
}
