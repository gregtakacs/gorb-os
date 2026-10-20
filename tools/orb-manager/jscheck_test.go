package main

import (
	"fmt"
	"regexp"
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

// The two floors of the landing page must be complements, or a board falls between
// them and cannot be clicked at all. The page is not allowed to derive either verdict:
// it reads the hub's word on the row (`orb`) and on the port (`orbSeen`). Pinning the
// source text is what stops a future edit from quietly reintroducing the field-by-field
// rule that hid a wiped board on this bench — and from mistaking `inFleet`, which says
// only that the address book has a row, for proof a chip has ever run the firmware.
func TestClientReadsTheOrbVerdictAndNothingElse(t *testing.T) {
	src := readAppJS(t)
	for _, want := range []string{
		"function isBlankRow(o)", "return o.orb !== true;",
		"function isBlankPort(e)", "if (e.orbSeen === true) return false;",
	} {
		if !strings.Contains(src, want) {
			t.Errorf("app.js no longer contains %q: the blank gate and the fleet filter must read the hub's verdict", want)
		}
	}
	// The two rules the fix replaced. Their return is unconditional and does not
	// mention the verdict, so they must be absent from any function that decides
	// whether a board is a card or a candidate.
	for _, banned := range []string{
		"if (e.inFleet) return false",
		"if (o.name || o.ip || o.fw || o.state) return false",
		`if (o.reachable === "alive" || o.reachable === "dead") return false`,
	} {
		if strings.Contains(src, banned) {
			t.Errorf("app.js re-derives a blank from %q: read the hub's orb verdict instead", banned)
		}
	}
}

// A name assigned without a declaration becomes a property of the browser's global
// object instead of a module-level binding. Nothing in the page notices, and the
// value is silently lost on the next reload. This bit when the flash modal replaced
// the manage view's update note: the assignment survived, the `let` above it did not,
// and no test on this machine could run the file to find out.
//
// The check is textual, so it is deliberately narrow: a bare identifier assigned
// outside a string or comment, whose declaration appears nowhere in the file. Locals,
// parameters and destructured names all have declarations somewhere, so they pass.
func undeclaredAssignments(t *testing.T, full string) []string {
	var leaks []string

	// Every name introduced by a declaration, a parameter, or a function name. A
	// name assigned anywhere in the file must appear in this set.
	declared := map[string]bool{}
	note := func(names string) {
		for _, f := range strings.FieldsFunc(names, func(r rune) bool {
			return !(r == '_' || r == '$' || r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' || r >= '0' && r <= '9')
		}) {
			declared[f] = true
		}
	}
	for _, m := range regexp.MustCompile(`\b(?:let|var|const)\s+([^=;\n]+)`).FindAllStringSubmatch(full, -1) {
		note(m[1]) // one statement may declare several names
	}
	for _, m := range regexp.MustCompile(`\bfunction\s*[A-Za-z_$\w]*\(([^)]*)\)`).FindAllStringSubmatch(full, -1) {
		note(m[1])
	}
	for _, m := range regexp.MustCompile(`\bfunction\s+([A-Za-z_$][\w$]*)`).FindAllStringSubmatch(full, -1) {
		declared[m[1]] = true
	}
	// Destructuring and for-of targets, which the patterns above read as an
	// expression rather than a binding list.
	for _, m := range regexp.MustCompile(`\b(?:for)\s*\(\s*(?:let|var|const)\s*(\[[^\]]*\])`).FindAllStringSubmatch(full, -1) {
		note(m[1])
	}

	builtin := map[string]bool{
		"window": true, "document": true, "console": true, "location": true, "navigator": true,
		"history": true, "localStorage": true, "sessionStorage": true, "performance": true,
	}

	// Assigned names are read line by line from the raw file. A line holding a quote
	// is skipped: a template literal can contain text that reads like an assignment,
	// and a checker that cries wolf on one stops being trusted.
	// RE2 has no lookahead, so the character following `=` is captured and judged
	// in Go: `==` and `=>` are a comparison and an arrow, not an assignment.
	// The character before `=` is captured because `+=` is still an assignment to a
	// name that must be declared, and the one after it is captured because `==` is
	// not an assignment at all. RE2 has no lookahead for either test.
	assign := regexp.MustCompile(`^\s*(?:let|var|const)?[ \t]*([A-Za-z_$][\w$]*)[ \t]*([-+*/%&|^|]?)=([^=])`)
	prefix := regexp.MustCompile(`^\s*(?:let|var|const)[ \t]+`)
	for _, line := range strings.Split(full, "\n") {
		if strings.ContainsAny(line, "\"'`") || strings.Contains(line, "//") {
			continue
		}
		m := assign.FindStringSubmatch(line)
		if m == nil || m[3] == ">" {
			continue // `=>`, an arrow function, not an assignment
		}
		name := m[1]
		// A declaration line binds the name; only a bare assignment can leak.
		if prefix.MatchString(line) {
			continue
		}
		if builtin[name] || declared[name] {
			continue
		}
		leaks = append(leaks, name)
	}
	return leaks
}

func TestAppJSAssignsOnlyDeclaredNames(t *testing.T) {
	for _, leak := range undeclaredAssignments(t, readAppJS(t)) {
		t.Errorf("app.js assigns %q with no let/var/const anywhere: it becomes an implicit global", leak)
	}
}

// The checker earns its keep on the bug it was written for: an assignment left behind
// when the declaration above it was deleted in a rewrite. app.js is never executed on
// this machine, so nothing else would ever notice.
func TestUndeclaredAssignmentCheckerCatchesARealLeak(t *testing.T) {
	broken := "let hubFirmware = null;\n" +
		"function refreshBuild() {\n" +
		"  hubFirmware = b;\n" +
		"  buildVersion = b.version;\n" +
		"}\n"
	leaks := undeclaredAssignments(t, broken)
	if len(leaks) != 1 || leaks[0] != "buildVersion" {
		t.Errorf("the checker reported %v on code with exactly one leak, want [buildVersion]", leaks)
	}
	if got := undeclaredAssignments(t, readAppJS(t)); len(got) != 0 {
		t.Errorf("the checker reports leaks on the real file: %v", got)
	}
}

// proseAtColumnZero reports lines that begin at column zero with two or more bare
// words and carry no code punctuation at all: prose the parser has no choice but to
// read as a statement. The rule is deliberately narrow. A comment continuation loses
// its `//` only at column zero, because that is where a block comment ends and code
// resumes, and every string that legitimately holds prose in this file is indented.
//
// The rule exists because of a specific 2026-10-19 failure: a splice wrote this file
// with one comment line missing its prefix. Braces stayed balanced, quotes stayed
// paired, so every balance test passed, and the hub served a page that sat on
// "loading" forever with no button wired and nothing in the console the operator
// could see. A SyntaxError discards the entire file, so the smallest prose leak is a
// whole-page failure, and the symptom gives no hint of the line responsible.
func TestAppJSProseStaysInComments(t *testing.T) {
	lines := strings.Split(readWebFile(t, "app.js"), "\n")
	inBlock := false
	for i, line := range lines {
		if strings.Contains(line, "/*") {
			inBlock = true
		}
		if strings.Contains(line, "*/") {
			inBlock = false
			continue
		}
		if inBlock {
			continue
		}
		if !proseAtColumnZero(line) {
			continue
		}
		t.Errorf("app.js line %d is prose at column zero: %q\n"+
			"one unprefixed comment line is a SyntaxError, and a SyntaxError silently kills every button on the page",
			i+1, line)
		inBlock = true // report once per leaked block, not once per line
	}
}

// proseAtColumnZero decides whether a line can only be English. Code at column zero
// carries structure: an assignment, a call, a block, an index, a member, a terminator.
// Stripping prose punctuation from every token and still finding bare words means the
// line is a sentence. The statement keywords are listed because `return nothing now`
// is a real statement and not a sentence.
func proseAtColumnZero(line string) bool {
	if line == "" || line[0] == ' ' || line[0] == '\t' {
		return false
	}
	if strings.HasPrefix(strings.TrimSpace(line), "//") {
		return false
	}
	// Structure characters. A comma and a period are deliberately absent: they are
	// sentence punctuation far more often than they are code punctuation, and every
	// code line that uses one also uses a character below.
	for _, r := range line {
		switch r {
		case '(', ')', '=', '{', '}', '[', ']', ';', '"', '\'', '`', '\\', '+', '-', '<', '>', '?', ':', '&', '|', '!':
			return false
		}
	}
	words := strings.Fields(line)
	if len(words) < 4 {
		return false
	}
	for _, w := range words {
		bare := strings.Trim(w, ".,;:-'\"()[]{}")
		if bare == "" {
			continue
		}
		if !regexp.MustCompile(`^[A-Za-z][A-Za-z]*$`).MatchString(bare) {
			return false
		}
	}
	switch words[0] {
	case "return", "if", "else", "for", "while", "do", "try", "catch", "finally", "switch", "case",
		"default", "break", "continue", "throw", "new", "delete", "typeof", "instanceof", "void",
		"in", "of", "function", "class", "const", "let", "var", "async", "await", "yield", "export",
		"import", "extends", "static", "get", "set", "this", "super":
		return false
	}
	return true
}

// ---- function scope ------------------------------------------------------------
//
// undeclaredAssignments above reads whole-line bare assignments only, and treats a name
// as declared if the declaration appears anywhere in the file. Both shortcuts failed on
// 2026-10-19. A revert deleted `const btn = $("#mn-flashbtn")` from gateFwButtons and
// left `btn.disabled = ...` behind: not a whole-line assignment, and `btn` is declared in
// several other functions, so the file-wide check passed while every click on every fleet
// tile threw a ReferenceError inside openOrbByIP, before show("orb"). The symptom was a
// tile that looked selectable and did nothing, with the manage view never appearing.
//
// So: walk each top-level function as its own lexical scope, and require the root of any
// property assignment to resolve inside that scope, its enclosing scope, the file's
// top level, or the browser's set of globals.
// jsBlockKeyword reports whether a scope-introducing keyword starts at stripped[i].
var jsScopeKeywords = []string{"function", "for", "if", "while", "do", "try", "else", "switch"}

func jsBlockKeyword(src string, i int) bool {
	if i > 0 {
		if c := src[i-1]; c == '_' || c == '$' || c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' {
			return false // a member or the tail of a longer word, e.g. `notify.if`
		}
	}
	for _, k := range jsScopeKeywords {
		if !strings.HasPrefix(src[i:], k) {
			continue
		}
		j := i + len(k)
		if j < len(src) {
			if c := src[j]; c == '_' || c == '$' || c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' {
				return false // `iffy`, `format`, `trial`
			}
		}
		return true
	}
	return false
}

func jsFunctionSpans(stripped string) [][2]int {
	var spans [][2]int
	depth := 0
	matchBrace := func(i int) int {
		// i is the index of the '{'; return the index just past its partner.
		p := 0
		for ; i < len(stripped); i++ {
			if stripped[i] == '{' {
				p++
			} else if stripped[i] == '}' {
				p--
				if p == 0 {
					return i + 1
				}
			}
		}
		return len(stripped)
	}
	for i := 0; i < len(stripped); {
		c := stripped[i]
		switch {
		case c == '{' || c == '(' || c == '[':
			depth++
			i++
		case c == '}' || c == ')' || c == ']':
			if depth > 0 {
				depth--
			}
			i++

		case c >= 'a' && c <= 'z' && jsBlockKeyword(stripped, i):
			// A `function` body, a `for` block and an `if` block all bind their own
			// const/let, so none of them may contribute declarations to the file's top
			// level. Skipping only `function` left `for (const pair of ...) { const inp
			// = ..., btn = ... }` at the top level, and its `btn` excused an undeclared
			// write in a function 240 lines away.
			j := i
			for j < len(stripped) && stripped[j] != '{' {
				if stripped[j] != '(' {
					j++
					continue
				}
				p := 0
				for ; j < len(stripped); j++ {
					if stripped[j] == '(' {
						p++
					} else if stripped[j] == ')' {
						p--
						if p == 0 {
							j++
							break
						}
					}
				}
			}
			if j >= len(stripped) {
				i++
				break
			}
			end := matchBrace(j)
			spans = append(spans, [2]int{i, end})
			i = end
		case depth > 0 && !(i+1 < len(stripped) && c == '=' && stripped[i+1] == '>'):
			i++
		case c == '=' && i+1 < len(stripped) && stripped[i+1] == '>':
			// An arrow body is its own scope wherever it sits. `function` alone is not
			// enough: this client wires most of its behaviour through arrow handlers, and
			// an arrow nested in a top-level statement such as
			// document.querySelectorAll(...).forEach((r) => { const btn = ... })
			// otherwise contributes its locals to the file's top level, which is exactly
			// the file-wide blindness this checker exists to remove. Depth is tracked so
			// braces inside the body stay counted, not so the body is skipped.
			j := i + 2
			for j < len(stripped) && (stripped[j] == ' ' || stripped[j] == '\t' || stripped[j] == '\n') {
				j++
			}
			if j >= len(stripped) || stripped[j] != '{' {
				i++
				break
			}
			// Take the parameter list into the span: it sits before the `=>`, and a
			// region that omits its own parameters reports them as undeclared.
			p := i
			for p > 0 && (stripped[p-1] == ' ' || stripped[p-1] == '\t' || stripped[p-1] == '\n') {
				p--
			}
			if p > 0 && stripped[p-1] == ')' {
				q, d := p-1, 0
				for ; q > 0; q-- {
					if stripped[q] == ')' {
						d++
					} else if stripped[q] == '(' {
						d--
						if d == 0 {
							break
						}
					}
				}
				p = q
			} else {
				for p > 0 && (stripped[p-1] == '_' || stripped[p-1] == '$' ||
					stripped[p-1] >= 'a' && stripped[p-1] <= 'z' ||
					stripped[p-1] >= 'A' && stripped[p-1] <= 'Z' ||
					stripped[p-1] >= '0' && stripped[p-1] <= '9') {
					p--
				}
			}
			end := matchBrace(j)
			spans = append(spans, [2]int{p, end})
			i = end
		default:
			i++
		}
	}
	return spans
}

// jsDeclaredNames collects every binding a scope introduces. Over-approximating is the
// safe direction: a name wrongly counted as declared can hide a bug, which is what the
// file-wide checker already did, but a name wrongly counted as undeclared fails the build
// on correct code, and a test that fails on correct code gets deleted.
func jsDeclaredNames(text string) map[string]bool {
	declared := map[string]bool{}
	note := func(names string) {
		for _, f := range regexp.MustCompile(`[A-Za-z_$][\w$]*`).FindAllString(names, -1) {
			declared[f] = true
		}
	}
	// One pattern per declarator shape. None reads past its `=` into an initializer, so
	// a name merely used on the right hand side is not mistaken for a binding.
	for _, m := range regexp.MustCompile(`\b(?:let|var|const)[ \t]+([\{\[\(]?[^,=\n;]*)`).FindAllStringSubmatch(text, -1) {
		note(m[1]) // first declarator, including an opening destructuring bracket
	}
	for _, m := range regexp.MustCompile(`,[ \t]*([\{\[\(]?[^,=\n]*)[ \t]*=`).FindAllStringSubmatch(text, -1) {
		note(m[1]) // every later declarator, which owns its own `=`
	}
	for _, m := range regexp.MustCompile(`,[ \t]*([A-Za-z_$][\w$]*)[ \t]*;`).FindAllStringSubmatch(text, -1) {
		note(m[1]) // `let a, b;` with no initializer
	}
	for _, m := range regexp.MustCompile(`\{([^{}]*)\}[ \t]*=`).FindAllStringSubmatch(text, -1) {
		note(m[1]) // object destructuring: const { log, note } = ...
	}
	for _, m := range regexp.MustCompile(`\[([^\[\]]*)\][ \t]*=`).FindAllStringSubmatch(text, -1) {
		note(m[1]) // array destructuring: const [a, b] = ...
	}
	for _, m := range regexp.MustCompile(`\bfunction[ \t]*[A-Za-z_$\w]*[ \t]*\(([^)]*)\)`).FindAllStringSubmatch(text, -1) {
		note(m[1])
	}
	// Arrow parameters carry no `function` keyword to anchor on: (v) =>, (item) =>.
	for _, m := range regexp.MustCompile(`\(([^)]*)\)[ \t]*=>`).FindAllStringSubmatch(text, -1) {
		note(m[1])
	}
	for _, m := range regexp.MustCompile(`([A-Za-z_$][\w$]*)[ \t]*=>`).FindAllStringSubmatch(text, -1) {
		note(m[1])
	}
	for _, m := range regexp.MustCompile(`\bcatch[ \t]*\(([A-Za-z_$][\w$]*)`).FindAllStringSubmatch(text, -1) {
		note(m[1])
	}
	for _, m := range regexp.MustCompile(`\bfunction[ \t]+([A-Za-z_$][\w$]*)`).FindAllStringSubmatch(text, -1) {
		declared[m[1]] = true
	}
	return declared
}

// jsWrite is one assignment: the base identifier it targets and where in the text it sits.
type jsWrite struct {
	name string
	at   int
}

// jsAssignmentRoots returns the base identifier of every write in text: `x.f = v`,
// `x[0] = v`, `x = v`, `x++`. RE2 has no lookahead, so the character following `=` is
// captured and judged in Go: `==` is a comparison and `=>` is an arrow.
func jsAssignmentRoots(text string) []jsWrite {
	// The preceding character is captured so `.field =` cannot match on its own: the root
	// of a member write is the base of the whole chain, matched in one piece.
	re := regexp.MustCompile(`(^|[^\w$.])([A-Za-z_$][\w$]*)((?:[ \t]*(?:\.\w+|\[[^\]]*\]))*)[ \t]*(\+\+|--|[-+*/%&|^]?=)(.)`)
	var roots []jsWrite
	for _, m := range re.FindAllStringSubmatchIndex(text, -1) {
		// Submatch i is the pair m[2i], m[2i+1]: 1 previous, 2 name, 3 chain,
		// 4 operator, 5 the single character after the operator.
		if m[10] < 0 {
			continue // the operator ended the text, so nothing follows it
		}
		name, op, next := text[m[4]:m[5]], text[m[8]:m[9]], text[m[10]:m[11]]
		if op == "=" && (next == "=" || next == ">") {
			continue
		}
		roots = append(roots, jsWrite{name: name, at: m[2]})
	}
	return roots
}

// undeclaredScopeWrites checks each top-level scope separately, which is what the
// file-wide undeclaredAssignments above cannot do: `btn` is declared in several functions
// of app.js, and a write to it from a function that never declares it is the exact bug
// that killed every fleet tile click on 2026-10-19.
func undeclaredScopeWrites(masked string) []string {
	jsKeywords := map[string]bool{
		"if": true, "else": true, "for": true, "while": true, "do": true, "switch": true,
		"case": true, "default": true, "return": true, "typeof": true, "instanceof": true,
		"new": true, "delete": true, "void": true, "in": true, "of": true, "await": true,
		"async": true, "function": true, "class": true, "extends": true, "super": true,
		"this": true, "try": true, "catch": true, "finally": true, "throw": true,
		"break": true, "continue": true, "yield": true, "let": true, "const": true,
		"var": true, "get": true, "set": true, "static": true, "arguments": true,
	}
	globals := map[string]bool{
		"window": true, "document": true, "console": true, "location": true,
		"navigator": true, "history": true, "localStorage": true, "sessionStorage": true,
		"performance": true, "self": true, "globalThis": true, "screen": true,
		"EventSource": true, "FormData": true, "Blob": true, "URL": true, "crypto": true,
		"AbortController": true, "Math": true, "JSON": true, "Date": true, "Number": true,
		"String": true, "Array": true, "Object": true, "Promise": true, "Set": true,
		"Map": true, "Boolean": true, "Error": true, "RegExp": true, "Uint8Array": true,
		"TextEncoder": true, "TextDecoder": true, "Response": true, "Request": true,
		"Headers": true, "ResizeObserver": true, "MutationObserver": true, "URLSearchParams": true,
		"Intl": true, "Symbol": true, "Infinity": true, "NaN": true, "undefined": true,
	}
	spans := jsFunctionSpans(masked)
	topLevel := jsDeclaredNames(stripJSRegions(masked, spans))
	atLine := func(off int) int { return strings.Count(masked[:off], "\n") + 1 }
	seen := map[string]bool{}
	var leaks []string
	report := func(scope string, base int, roots []jsWrite, declared map[string]bool) {
		for _, w := range roots {
			if jsKeywords[w.name] || globals[w.name] || declared[w.name] || seen[w.name] {
				continue
			}
			seen[w.name] = true
			leaks = append(leaks, fmt.Sprintf("line %d: %s writes to %s, which is declared nowhere in that function, at the file's top level, or among the browser globals",
				atLine(base+w.at), scope, w.name))
		}
	}
	for _, sp := range spans {
		body := masked[sp[0]:sp[1]]
		head := regexp.MustCompile(`^\s*function[ \t]+([A-Za-z_$][\w$]*)`).FindStringSubmatch(body)
		scope := "a top-level handler"
		if len(head) == 2 {
			scope = "function " + head[1]
		}
		declared := jsDeclaredNames(body)
		for k := range topLevel {
			declared[k] = true
		}
		report(scope, sp[0], jsAssignmentRoots(body), declared)
	}
	report("the file's top level", 0, jsAssignmentRoots(stripJSRegions(masked, spans)), topLevel)
	return leaks
}

// maskJSForScope removes comments, string and template literals and regex literals,

// maskJSForScope removes comments, string and template literals and regex literals,
// writing spaces in their place. Unlike stripJSLiterals it preserves length and
// whitespace, because the scope checker needs token separation in its patterns and a
// byte offset it can turn back into a line number for the failure message.
func maskJSForScope(src string) string {
	out := []byte(src)
	blank := func(from, to int) {
		for i := from; i < to && i < len(out); i++ {
			if out[i] != '\n' {
				out[i] = ' '
			}
		}
	}
	lastCode := byte('\n')
	for i := 0; i < len(src); {
		c := src[i]
		switch {
		case c == '/' && i+1 < len(src) && src[i+1] == '/':
			j := i
			for j < len(src) && src[j] != '\n' {
				j++
			}
			blank(i, j)
			i = j
		case c == '/' && i+1 < len(src) && src[i+1] == '*':
			j := i + 2
			for j+1 < len(src) && !(src[j] == '*' && src[j+1] == '/') {
				j++
			}
			blank(i, j+2)
			i = j + 2
		case c == '/' && regexAllowedAfter(lastCode):
			end, ok := skipRegex(src, i)
			if !ok {
				lastCode = c
				i++
				break
			}
			blank(i, end)
			i = end
		case c == '\'' || c == '"':
			q := c
			j := i + 1
			for j < len(src) && src[j] != q {
				if src[j] == '\\' {
					j++
				}
				j++
			}
			blank(i, j+1)
			lastCode = q
			i = j + 1
		case c == '`':
			j := i + 1
			for j < len(src) && src[j] != '`' {
				if src[j] == '\\' {
					j++
				}
				j++
			}
			blank(i, j+1)
			lastCode = '`'
			i = j + 1
		default:
			lastCode = c
			i++
		}
	}
	return string(out)
}

// stripJSRegions blanks the given spans, leaving the file's top level.
func stripJSRegions(stripped string, spans [][2]int) string {
	out := []byte(stripped)
	for _, sp := range spans {
		for i := sp[0]; i < sp[1] && i < len(out); i++ {
			if out[i] != '\n' {
				out[i] = ' '
			}
		}
	}
	return string(out)
}

// The tile-click bug, written out: a rewrite that deleted the declaration and kept the
// use must be caught by name, in the function that got broken.
func TestScopeCheckerCatchesTheTileClickBug(t *testing.T) {
	broken := `
function gateFwButtons() {
  const reachable = !!(current.port || current.ip);
  btn.disabled = !reachable || !hubFirmware;
}
function otherButton() {
  const btn = $("#mn-flashbtn");
  btn.disabled = true;
}
let current = {};
let hubFirmware = null;
`
	leaks := undeclaredScopeWrites(maskJSForScope(broken))
	if len(leaks) != 1 || !strings.Contains(leaks[0], "gateFwButtons") || !strings.Contains(leaks[0], " to btn,") {
		t.Errorf("the scope checker reported %v on code with exactly one leak, want one naming gateFwButtons and btn", leaks)
	}
	fixed := `
function gateFwButtons() {
  const btn = $("#mn-flashbtn");
  btn.disabled = !!(current.port || current.ip);
}
let current = {};
`
	if leaks := undeclaredScopeWrites(maskJSForScope(fixed)); len(leaks) != 0 {
		t.Errorf("the scope checker reports %v on correct code", leaks)
	}
	// A closure over an outer function's local is legal, and must stay unreported.
	closure := `
function outer() {
  const state = $("#ff-state");
  try { state.textContent = "x"; } catch (e) { e.message = "y"; }
  items.forEach((item) => { item.hidden = true; });
  return function inner() { state.textContent = "z"; };
}
let items = [];
`
	if leaks := undeclaredScopeWrites(maskJSForScope(closure)); len(leaks) != 0 {
		t.Errorf("the scope checker reports %v on closures and catch bindings, which are legal", leaks)
	}
}

// And on the real client: no write anywhere resolves to nothing.
func TestAppJSWritesResolveInScope(t *testing.T) {
	for _, leak := range undeclaredScopeWrites(maskJSForScope(readAppJS(t))) {
		t.Errorf("app.js: %s", leak)
	}
}
