package main

// G1 step 3: what the two headless commands owe the site that runs them.
//
// The claim ruling 15 makes is that unpacking editors with a command is the same as
// serving them, because it is the same code. These tests hold that claim: every byte
// extract-editors writes is fetched again through the hub's own routes and compared with
// what the handler answers, every exported runtime file likewise, and the archives a
// hostile upload would carry are fed straight to the unpacker to prove that a member named
// `../x` costs a row and not a file outside the out directory.
//
// The commands are driven through runSubcommand — the same entry point main() calls — so
// the argument handling, the exit codes and the reports are exercised, not a helper the
// binary never uses.

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"fmt"
	"io/fs"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// runCommand drives one headless command the way main() does, and returns its exit code
// with everything it wrote.
func runCommand(t *testing.T, args ...string) (int, string, string) {
	t.Helper()
	var stdout, stderr bytes.Buffer
	code, ok := runSubcommand(args, &stdout, &stderr)
	if !ok {
		t.Fatalf("runSubcommand(%v) said it was not a subcommand", args)
	}
	return code, stdout.String(), stderr.String()
}

// editorURL is a member name as a request path: each step escaped and the separators left
// alone, which is how a site puts an editor's own subfolders on the wire.
func editorURL(name string) string {
	segs := strings.Split(name, "/")
	for i, s := range segs {
		segs[i] = url.PathEscape(s)
	}
	return strings.Join(segs, "/")
}

func readReport(t *testing.T, path string, v any) {
	t.Helper()
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}
	if err := json.Unmarshal(raw, v); err != nil {
		t.Fatalf("decode %s: %v\n%s", path, err, raw)
	}
}

// TestExtractEditorsMatchesWhatTheHubServes is ruling 15's whole case in one test: the
// bytes the command unpacked from the shipped shelf are the bytes serveEditor answers for
// the same container, including the document a bare /editor/<ip>/<slug>/ opens.
func TestExtractEditorsMatchesWhatTheHubServes(t *testing.T) {
	out := filepath.Join(t.TempDir(), "editors")
	code, stdout, stderr := runCommand(t, "extract-editors", "apps", out)
	if code != 0 {
		t.Fatalf("extract-editors apps: exit %d\nstdout %s\nstderr %s", code, stdout, stderr)
	}
	var rep extractReport
	readReport(t, filepath.Join(out, "editors.json"), &rep)
	if rep.Format != 1 || rep.Hub != version {
		t.Errorf("report header: format %d hub %q, want 1 %q", rep.Format, rep.Hub, version)
	}
	if strings.Join(rep.Runtime, ",") != strings.Join(runtimeIDs(), ",") {
		t.Errorf("report runtimes %v, want the ids this hub carries %v", rep.Runtime, runtimeIDs())
	}
	// One row per shipped container, in the sorted order the directory was read.
	var got []string
	rows := map[string]extractRow{}
	for _, r := range rep.Apps {
		got = append(got, r.File+":"+r.Status)
		rows[r.File] = r
	}
	// color_picker is the one shelf app whose header names no editor region at all, so the
	// truthful status today is "no-editor". A release that gives it an editor turns this
	// line into the thing that says so, rather than a mystery in the site's listing.
	want := []string{"clock.orbapp:ok", "color_picker.orbapp:no-editor",
		"flights.orbapp:ok", "radar.orbapp:ok", "weather.orbapp:ok"}
	if strings.Join(got, ",") != strings.Join(want, ",") {
		t.Fatalf("rows = %v, want %v", got, want)
	}
	for _, f := range want {
		if strings.HasSuffix(f, ":ok") {
			checkExtractedApp(t, out, rows[strings.TrimSuffix(f, ":ok")])
		}
	}
	// stdout is one line per app: the status, then the app or the reason.
	for _, line := range []string{"ok         clock v1.6.1+0569cae (9 files)",
		"no-editor  color_picker.orbapp: this app ships no editor"} {
		if !strings.Contains(stdout, line) {
			t.Errorf("stdout lacks %q:\n%s", line, stdout)
		}
	}
}

// checkExtractedApp fetches every file the report lists for one app through the hub's own
// routes, with the app's real region behind the dial, and demands the same bytes that
// landed on disk — plus the entry a directory request opens, the file count the archive
// holds, and digests that match the files.
func checkExtractedApp(t *testing.T, out string, row extractRow) {
	t.Helper()
	const ip = "203.0.113.21"
	if row.Editor == nil {
		t.Fatalf("%s: status ok with no editor description", row.File)
	}
	body, err := os.ReadFile(filepath.Join("apps", row.File))
	if err != nil {
		t.Fatal(err)
	}
	region, err := orbnet.AppEditorRegion(body)
	if err != nil {
		t.Fatalf("%s carries no readable editor region: %v", row.File, err)
	}
	if row.Editor.Dir != row.Slug {
		t.Errorf("%s: dir %q, want the slug %q", row.File, row.Editor.Dir, row.Slug)
	}
	s, _ := editorHub(t, ip, region, nil)
	entryDisk, err := os.ReadFile(filepath.Join(out, row.Editor.Dir, filepath.FromSlash(row.Editor.Entry)))
	if err != nil {
		t.Fatalf("%s: the entry the report names is not on disk: %v", row.File, err)
	}
	zr0, err := zip.NewReader(bytes.NewReader(region), int64(len(region)))
	if err != nil {
		t.Fatal(err)
	}
	// A hosted editor (host@1) has no page in its region: extract-editors writes the hub's
	// own, which the hub answers at the directory (checked below), not under a file name.
	_, inRegion := editorMember(zr0, row.Editor.Entry)
	generated := !inRegion
	for _, f := range row.Editor.Files {
		if generated && f.Name == row.Editor.Entry {
			continue
		}
		disk, err := os.ReadFile(filepath.Join(out, row.Editor.Dir, filepath.FromSlash(f.Name)))
		if err != nil {
			t.Fatalf("%s: %s: %v", row.File, f.Name, err)
		}
		if len(disk) != f.Bytes || hexSha(disk) != f.SHA256 {
			t.Errorf("%s: %s: report says %d B / %s, disk holds %d B / %s",
				row.File, f.Name, f.Bytes, short(f.SHA256), len(disk), short(hexSha(disk)))
		}
		rec := editorGet(t, s, "/editor/"+ip+"/"+row.Slug+"/"+editorURL(f.Name), true)
		if rec.Code != http.StatusOK {
			t.Errorf("%s: GET %s: status %d, want 200", row.File, f.Name, rec.Code)
			continue
		}
		// One name means two things to the hub: editor.json is the manifest, and asking
		// for it by name opens the editor rather than the file (editor.go's own rule, and
		// the reason the report carries the manifest's bytes separately). Every other
		// member is served as itself, which is the byte-for-byte claim this test is for.
		served := disk
		if f.Name == editorManifestName {
			served = entryDisk
		}
		if !bytes.Equal(rec.Body.Bytes(), served) {
			t.Errorf("%s: GET %s served %d B, want the %d B of %s",
				row.File, f.Name, rec.Body.Len(), len(served), f.Name)
		}
	}
	// A bare directory request answers the entry the report names.
	rec := editorGet(t, s, "/editor/"+ip+"/"+row.Slug+"/", true)
	if rec.Code != http.StatusOK || !bytes.Equal(rec.Body.Bytes(), entryDisk) {
		t.Errorf("%s: the directory request answered with %d B, want the %d B entry %s",
			row.File, rec.Body.Len(), len(entryDisk), row.Editor.Entry)
	}
	// Every member the archive holds arrived: nothing was quietly dropped.
	zr, err := zip.NewReader(bytes.NewReader(region), int64(len(region)))
	if err != nil {
		t.Fatal(err)
	}
	var members int
	for _, f := range zr.File {
		if !f.FileInfo().IsDir() {
			members++
		}
	}
	if generated {
		members++ // the page extract-editors wrote
	}
	if len(row.Editor.Files) != members {
		t.Errorf("%s: %d files reported, the region holds %d members", row.File, len(row.Editor.Files), members)
	}
	// The manifest came through unchanged, so the site serves the runtime list and entry
	// the hub itself read out of the container.
	if raw, found := editorMember(zr, editorManifestName); found {
		var m struct {
			Entry   string   `json:"entry"`
			Runtime []string `json:"runtime"`
		}
		if err := json.Unmarshal(raw, &m); err != nil {
			t.Fatalf("%s: the shipped editor.json does not parse: %v", row.File, err)
		}
		if m.Entry != "" && m.Entry != row.Editor.Entry {
			t.Errorf("%s: entry %q, the manifest names %q", row.File, row.Editor.Entry, m.Entry)
		}
		if strings.Join(row.Editor.Runtime, ",") != strings.Join(orEmpty(m.Runtime), ",") {
			t.Errorf("%s: report runtime %v, manifest lists %v", row.File, row.Editor.Runtime, m.Runtime)
		}
	}
}

// orEmpty is a report's empty list as an empty list rather than a null.
func orEmpty(s []string) []string {
	if s == nil {
		return []string{}
	}
	return s
}

// TestExportRuntimeMatchesWhatTheHubServes walks every file the export wrote back through
// serveRuntime: the site's tree and the hub's answers must be one set of bytes, or an
// editor built against a hub's release paints differently once it is served from a site.
// The tree is also demanded to be exactly the report — no extra file, no missing one.
func TestExportRuntimeMatchesWhatTheHubServes(t *testing.T) {
	out := filepath.Join(t.TempDir(), "runtime")
	code, stdout, stderr := runCommand(t, "export-runtime", out)
	if code != 0 {
		t.Fatalf("export-runtime: exit %d\nstdout %s\nstderr %s", code, stdout, stderr)
	}
	var rep runtimeReport
	readReport(t, filepath.Join(out, "runtime.json"), &rep)
	if rep.Format != 1 || rep.Hub != version {
		t.Errorf("report header: format %d hub %q, want 1 %q", rep.Format, rep.Hub, version)
	}
	var ids []string
	for _, r := range rep.IDs {
		ids = append(ids, r.ID)
	}
	if strings.Join(ids, ",") != strings.Join(runtimeIDs(), ",") {
		t.Fatalf("exported ids %v, want what this binary carries %v", ids, runtimeIDs())
	}
	s := newHub()
	want := []string{"runtime.json"}
	for _, r := range rep.IDs {
		if r.Vendor != runtimeCatalog[r.ID].vendor {
			t.Errorf("%s: report vendor %v, the catalog says %v", r.ID, r.Vendor, runtimeCatalog[r.ID].vendor)
		}
		var total int
		for i, f := range r.Files {
			if i > 0 && r.Files[i-1].Name >= f.Name {
				t.Errorf("%s: files are not sorted by name (%s then %s)", r.ID, r.Files[i-1].Name, f.Name)
			}
			disk, err := os.ReadFile(filepath.Join(out, filepath.FromSlash(r.ID), filepath.FromSlash(f.Name)))
			if err != nil {
				t.Fatalf("%s/%s: %v", r.ID, f.Name, err)
			}
			if len(disk) != f.Bytes || hexSha(disk) != f.SHA256 {
				t.Errorf("%s/%s: report says %d B / %s, disk holds %d B / %s",
					r.ID, f.Name, f.Bytes, short(f.SHA256), len(disk), short(hexSha(disk)))
			}
			rec := editorGet(t, s, "/runtime/"+r.ID+"/"+editorURL(f.Name), true)
			if rec.Code != http.StatusOK {
				t.Errorf("GET /runtime/%s/%s: status %d, want 200", r.ID, f.Name, rec.Code)
				continue
			}
			if !bytes.Equal(rec.Body.Bytes(), disk) {
				t.Errorf("GET /runtime/%s/%s served %d B and the export wrote %d B", r.ID, f.Name, rec.Body.Len(), len(disk))
			}
			total += len(disk)
			want = append(want, r.ID+"/"+f.Name)
		}
		if line := fmt.Sprintf("%s %d files %d B\n", r.ID, len(r.Files), total); !strings.Contains(stdout, line) {
			t.Errorf("stdout lacks %q:\n%s", line, stdout)
		}
	}
	// The tree holds exactly the reported files and the report: an export that left a
	// vendor manifest or a selftest harness behind would hand a site bytes the hub 404s.
	var found []string
	if err := filepath.WalkDir(out, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.Type().IsRegular() {
			rel, rerr := filepath.Rel(out, p)
			if rerr != nil {
				return rerr
			}
			found = append(found, filepath.ToSlash(rel))
		}
		return nil
	}); err != nil {
		t.Fatal(err)
	}
	sort.Strings(want)
	sort.Strings(found)
	if strings.Join(found, "\n") != strings.Join(want, "\n") {
		t.Errorf("%d files on disk, %d in the report:\n%s", len(found), len(want),
			strings.Join(found, " "))
	}
}

// zipMember is one entry of a crafted archive, in the order it was written: the checks walk
// members in archive order, so a hostile region cannot be a Go map.
type zipMember struct {
	name string
	body string
	dir  bool // a folder entry: no bytes, but its name is still checked
	link bool // a symlink entry
	size int  // when set, the body is this many bytes of x
}

// craft writes members in the order given.
func craft(t *testing.T, ms ...zipMember) []byte {
	t.Helper()
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	for _, m := range ms {
		h := &zip.FileHeader{Name: m.name}
		switch {
		case m.dir:
			h.SetMode(fs.ModeDir | 0o755)
		case m.link:
			h.SetMode(fs.ModeSymlink | 0o777)
		}
		w, err := zw.CreateHeader(h)
		if err != nil {
			t.Fatalf("write member %q: %v", m.name, err)
		}
		body := m.body
		if m.size > 0 {
			body = strings.Repeat("x", m.size)
		}
		if _, err := w.Write([]byte(body)); err != nil {
			t.Fatalf("write member %q: %v", m.name, err)
		}
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

// notPlain is the sentence a name that is not a plain relative path costs.
func notPlain(name string) string {
	return fmt.Sprintf("the editor carries a member named %q, which is not a plain relative path", name)
}

// TestExtractEditorsRefusesUnsafeArchives feeds the unpacker the regions an upload that
// means harm would carry, straight to the per-archive function with a fresh root, and
// demands two things every time: the reason the contract gives, and an output directory left
// exactly as it was found — no file, no folder, and no `.partial` of one either.
//
// The last case is the opposite check, and the one the shipped shelf cannot make: an editor
// whose files sit in a subfolder. Nothing else here proves the unpacker makes folders.
func TestExtractEditorsRefusesUnsafeArchives(t *testing.T) {
	const slug = "clock"
	good := []zipMember{
		{name: "editor.json", body: `{"entry":"index.html","runtime":["support@1"]}`},
		{name: "index.html", body: `<body>hi</body>`},
	}
	cases := []struct {
		what    string
		members []zipMember
		want    string // the reason, or "" when the archive is fine and must be written
	}{
		{"parent step", []zipMember{{name: "../x", body: "x"}}, notPlain("../x")},
		{"absolute", []zipMember{{name: "/abs", body: "x"}}, notPlain("/abs")},
		{"grandparent", []zipMember{{name: "a/../../b", body: "x"}}, notPlain("a/../../b")},
		{"backslash", []zipMember{{name: `a\b`, body: "x"}}, notPlain(`a\b`)},
		{"drive letter", []zipMember{{name: "C:/x", body: "x"}}, notPlain("C:/x")},
		{"empty step", []zipMember{{name: "a//b", body: "x"}}, notPlain("a//b")},
		{"dot step", []zipMember{{name: "./a", body: "x"}}, notPlain("./a")},
		{"inner dot step", []zipMember{{name: "a/./b", body: "x"}}, notPlain("a/./b")},
		{"nul", []zipMember{{name: "a\x00b", body: "x"}}, notPlain("a\x00b")},
		{"folder named for a parent", []zipMember{{name: "../", dir: true}}, notPlain("../")},
		{"symlink", append([]zipMember{{name: "link.js", body: "index.html", link: true}}, good...),
			`the editor member "link.js" is not a plain file`},
		{"twice", []zipMember{{name: "a.js", body: "1"}, {name: "a.js", body: "2"}},
			`the editor carries "a.js" twice`},
		{"case clash", []zipMember{{name: "A.js", body: "1"}, {name: "a.js", body: "2"}},
			`the editor members "A.js" and "a.js" differ only in case`},
		{"file and folder", []zipMember{{name: "a", body: "1"}, {name: "a/b", body: "2"}},
			`the editor member "a" is both a file and a folder`},
		{"past the limit", []zipMember{{name: "big.js", size: editorMaxEntry + 1}},
			fmt.Sprintf("the editor member %q claims %d B, past the %d B one file may ship",
				"big.js", editorMaxEntry+1, editorMaxEntry)},
		{"manifest without an entry", []zipMember{{name: "editor.json", body: `{"runtime":["support@1"]}`}},
			"editor.json names no entry the hub can open"},
		{"manifest naming a missing file", []zipMember{
			{name: "editor.json", body: `{"entry":"gone.html"}`},
			{name: "index.html", body: `<body>hi</body>`}},
			"the editor has no gone.html"},
		{"runtime this hub lacks", []zipMember{
			{name: "editor.json", body: `{"entry":"index.html","runtime":["nope@9"]}`},
			{name: "index.html", body: `<body>hi</body>`}},
			runtimeMissingSentence(slug, []string{"nope@9"})},
		{"no manifest and no index", []zipMember{{name: "ui.html", body: `<body>hi</body>`}},
			"the editor has no index.html"},
		{"nested editor is written", []zipMember{
			{name: "editor.json", body: `{"entry":"ui/panel.html","runtime":["support@1"]}`},
			{name: "ui/", dir: true},
			{name: "ui/panel.html", body: `<body>nested</body>`},
			{name: "ui/assets/b.css", body: `b{}`}}, ""},
	}
	for _, c := range cases {
		t.Run(c.what, func(t *testing.T) {
			parent := t.TempDir()
			out := filepath.Join(parent, "out")
			if err := os.Mkdir(out, 0o755); err != nil {
				t.Fatal(err)
			}
			root, err := os.OpenRoot(out)
			if err != nil {
				t.Fatal(err)
			}
			defer root.Close()
			res, err := extractEditorRegion(root, slug, craft(t, c.members...))
			if err != nil {
				t.Fatalf("the unpacker failed instead of refusing: %v", err)
			}
			if c.want == "" {
				if res.status != "" || res.editor == nil {
					t.Fatalf("a good archive was refused: %q %q", res.status, res.reason)
				}
				for _, name := range []string{"editor.json", "ui/panel.html", "ui/assets/b.css"} {
					if _, err := root.ReadFile(slug + "/" + name); err != nil {
						t.Errorf("%s was not written under %s: %v", name, slug, err)
					}
				}
				if res.editor.Dir != slug || res.editor.Entry != "ui/panel.html" || len(res.editor.Files) != 3 {
					t.Errorf("editor = %+v", *res.editor)
				}
				return
			}
			if res.status != "skipped" {
				t.Fatalf("status = %q (reason %q), want skipped", res.status, res.reason)
			}
			if res.reason != c.want {
				t.Errorf("reason =\n%s\nwant\n%s", res.reason, c.want)
			}
			// Nothing escaped and nothing was half-written: the parent holds only the out
			// directory, and the out directory holds nothing at all.
			if entries, err := os.ReadDir(parent); err != nil {
				t.Fatal(err)
			} else if len(entries) != 1 || entries[0].Name() != "out" {
				t.Errorf("the parent holds %v, want only out", entries)
			}
			if entries, err := os.ReadDir(out); err != nil {
				t.Fatal(err)
			} else if len(entries) != 0 {
				var left []string
				for _, e := range entries {
					left = append(left, e.Name())
				}
				t.Errorf("out holds %v, want nothing — not even a .partial", left)
			}
		})
	}
}

// TestExtractEditorsRows covers what the report and the exit code are for: a directory of
// mixed luck gets one row per container in sorted order with the status the hub reached by,
// an unusable <out> and an empty <in> are exit 1 with no files written, and a wrong
// argument count is exit 2 with the usage line — the code a release job reads.
func TestExtractEditorsRows(t *testing.T) {
	in := t.TempDir()
	shelf := func(name string) {
		body, err := os.ReadFile(filepath.Join("apps", name))
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(in, name), body, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	flights, err := os.ReadFile(filepath.Join("apps", "flights.orbapp"))
	if err != nil {
		t.Fatal(err)
	}
	// A second copy of an app the run already took: same slug, different file name, and it
	// sorts first so the duplicate row is the one the run refuses.
	if err := os.WriteFile(filepath.Join(in, "copy.orbapp"), flights, 0o644); err != nil {
		t.Fatal(err)
	}
	shelf("flights.orbapp")
	if err := os.WriteFile(filepath.Join(in, "garbage.orbapp"), []byte("not a container at all"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(in, "truncated.orbapp"), flights[:len(flights)/2], 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(in, "notes.txt"), []byte("ignored"), 0o644); err != nil {
		t.Fatal(err)
	}
	out := filepath.Join(t.TempDir(), "editors")
	code, stdout, stderr := runCommand(t, "extract-editors", in, out)
	if code != 0 {
		t.Fatalf("a run that completed exits 0 whatever the rows say: %d\n%s%s", code, stdout, stderr)
	}
	var rep extractReport
	readReport(t, filepath.Join(out, "editors.json"), &rep)
	statuses, reasons := map[string]string{}, map[string]string{}
	for _, r := range rep.Apps {
		statuses[r.File], reasons[r.File] = r.Status, r.Reason
	}
	want := map[string]string{"copy.orbapp": "ok", "flights.orbapp": "skipped",
		"garbage.orbapp": "unreadable", "truncated.orbapp": "unreadable"}
	for file, status := range want {
		if statuses[file] != status {
			t.Errorf("%s: status %q, want %q (reason %q)", file, statuses[file], status, reasons[file])
		}
	}
	if len(rep.Apps) != len(want) {
		t.Errorf("%d rows, want %d — a .txt file is not an app: %v", len(rep.Apps), len(want), statuses)
	}
	for i := 1; i < len(rep.Apps); i++ {
		if rep.Apps[i-1].File >= rep.Apps[i].File {
			t.Errorf("rows are not in sorted file order: %s then %s", rep.Apps[i-1].File, rep.Apps[i].File)
		}
	}
	for _, r := range rep.Apps {
		if r.SHA256 == "" || r.Bytes == 0 {
			t.Errorf("%s: row carries no digest of the input file", r.File)
		}
		if r.Status == "ok" {
			continue
		}
		if r.Reason == "" {
			t.Errorf("%s: status %q with no reason", r.File, r.Status)
		}
		if !strings.Contains(stdout, r.File+": "+r.Reason) {
			t.Errorf("stdout lacks the reason line for %s:\n%s", r.File, stdout)
		}
	}
	for _, r := range rep.Apps {
		if r.Status == "skipped" &&
			r.Reason != "flights.orbapp is also the flights app; only the first is extracted" {
			t.Errorf("duplicate row reason = %q", r.Reason)
		}
	}
	// Only the app that unpacked has a folder: a refusal writes nothing, not even empty.
	entries, err := os.ReadDir(out)
	if err != nil {
		t.Fatal(err)
	}
	var dirs []string
	for _, e := range entries {
		if e.IsDir() {
			dirs = append(dirs, e.Name())
		}
	}
	if strings.Join(dirs, ",") != "flights" {
		t.Errorf("folders = %v, want only the one editor that unpacked (flights)", dirs)
	}
}

// TestExtractEditorsExitCodes is the half of the contract a release job reads: the codes,
// the words beside them, and the promise that a run which refused to start wrote nothing.
func TestExtractEditorsExitCodes(t *testing.T) {
	empty := t.TempDir()
	out := filepath.Join(t.TempDir(), "editors")
	t.Run("one file as input", func(t *testing.T) {
		code, stdout, stderr := runCommand(t, "extract-editors", filepath.Join("apps", "radar.orbapp"), out)
		if code != 0 {
			t.Fatalf("exit %d\n%s%s", code, stdout, stderr)
		}
		var rep extractReport
		readReport(t, filepath.Join(out, "editors.json"), &rep)
		if len(rep.Apps) != 1 || rep.Apps[0].Slug != "radar" || rep.Apps[0].Status != "ok" {
			t.Errorf("rows = %+v, want the one radar row", rep.Apps)
		}
	})
	t.Run("an out directory that is not fresh", func(t *testing.T) {
		// The run above wrote it, so the next one must refuse it rather than mix a tree.
		code, _, stderr := runCommand(t, "extract-editors", "apps", out)
		if code != 1 {
			t.Fatalf("exit %d, want 1", code)
		}
		if want := "orb-manager: " + out + " is not empty: extract-editors writes into a fresh directory\n"; stderr != want {
			t.Errorf("stderr = %q, want %q", stderr, want)
		}
		if entries, err := os.ReadDir(out); err != nil || len(entries) != 2 {
			t.Errorf("a refused run changed the tree: %v entries, err %v", len(entries), err)
		}
	})
	t.Run("a directory of no containers", func(t *testing.T) {
		code, _, stderr := runCommand(t, "extract-editors", empty, filepath.Join(t.TempDir(), "e"))
		if code != 1 {
			t.Fatalf("exit %d, want 1", code)
		}
		if want := "orb-manager: no .orbapp files in " + empty + "\n"; stderr != want {
			t.Errorf("stderr = %q, want %q", stderr, want)
		}
	})
	t.Run("a missing input", func(t *testing.T) {
		code, _, stderr := runCommand(t, "extract-editors", filepath.Join(empty, "nope"), out)
		if code != 1 {
			t.Fatalf("exit %d, want 1", code)
		}
		if !strings.HasPrefix(stderr, "orb-manager: stat ") || !strings.HasSuffix(stderr, "nope: no such file or directory\n") {
			t.Errorf("stderr = %q, want the hub-prefixed stat error", stderr)
		}
	})
	t.Run("usage", func(t *testing.T) {
		for _, c := range []struct {
			args []string
			want string
		}{
			{[]string{"extract-editors"}, extractEditorsUsage},
			{[]string{"extract-editors", "apps"}, extractEditorsUsage},
			{[]string{"extract-editors", "apps", "a", "b"}, extractEditorsUsage},
			{[]string{"export-runtime"}, exportRuntimeUsage},
			{[]string{"export-runtime", "a", "b"}, exportRuntimeUsage},
		} {
			code, stdout, stderr := runCommand(t, c.args...)
			if code != 2 {
				t.Errorf("%v: exit %d, want 2", c.args, code)
			}
			if stdout != "" {
				t.Errorf("%v: wrote %q to stdout, want nothing", c.args, stdout)
			}
			if want := c.want + "\n"; stderr != want {
				t.Errorf("%v: stderr = %q, want %q", c.args, stderr, want)
			}
		}
	})
	t.Run("anything else is not ours", func(t *testing.T) {
		for _, args := range [][]string{nil, {}, {"-addr", "x"}, {"--version"}, {"serve"}, {""}} {
			var stdout, stderr bytes.Buffer
			if _, handled := runSubcommand(args, &stdout, &stderr); handled {
				t.Errorf("runSubcommand(%q) claimed a run it should have left to the flags", args)
			}
			if stdout.Len() != 0 || stderr.Len() != 0 {
				t.Errorf("runSubcommand(%q) wrote while refusing: %q %q", args, stdout.String(), stderr.String())
			}
		}
	})
}
