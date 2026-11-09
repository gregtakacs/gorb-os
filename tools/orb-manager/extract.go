package main

// orb-os-net design §6.1 and §6.2, G1 slice step 3: the two headless commands a site's
// release pull runs instead of writing a second implementation of the .orbapp format.
//
// Ruling 15 says the shelf editors are unpacked by an Orb Manager command, not by a copy
// of the container layout in TypeScript, and ruling 8 says an app from another repo then
// works on the site exactly as it works here. Both hold only if the site is handed the
// hub's own answers, so this file reuses the hub's readers rather than re-deriving them:
// orbnet.ParseApp and orbnet.AppEditorRegion for identity and region, editorBytes for the
// per-file limit, rawEntryName for the entry document, editorRuntimeMissing and
// runtimeMissingSentence for the runtime gate. A row written `ok` here is a row serveEditor
// serves, byte for byte, and extract_test.go checks that against s.routes.
//
// Two properties these commands owe a job that runs them unattended:
//
//   - Nothing leaves <out>. Every write goes through an *os.Root, so a member named
//     `../../etc/x` is a refused NAME rather than a file somewhere else, and a report is
//     the only place such an archive is ever mentioned.
//   - An app is never half-written under its own name. Its files go into
//     `.<slug>.partial` and that folder is renamed to `<slug>` once the last byte is
//     down, so a pull cannot publish an editor with a file missing.
//
// The exit codes are the contract the release job reads: 0 when the run completed (rows
// in the report may still be skipped), 1 for an input or output problem, 2 for usage.
// Nothing goes to stdout but one line per app or per runtime id, so the job can log it.

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strings"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// The two usage lines are the whole help these commands offer. They exist to be called by
// a job; a human who types one wrong gets the one line that says the shape.
const (
	extractEditorsUsage = "usage: orb-manager extract-editors <dir of .orbapp files | file.orbapp> <out dir>"
	exportRuntimeUsage  = "usage: orb-manager export-runtime <out dir>"
)

// runSubcommand serves the headless commands from the very top of main(), before any flag
// is parsed, log redirected, instance lock claimed or port bound: a release pull must not
// fail because a hub is already running on the machine, and must not depend on a flag set
// it does not use. Everything else — no arguments, a first argument starting with a dash,
// an unknown word — is not a subcommand, and every existing flag keeps working as it did.
func runSubcommand(args []string, stdout, stderr io.Writer) (code int, handled bool) {
	if len(args) == 0 || strings.HasPrefix(args[0], "-") {
		return 0, false
	}
	switch args[0] {
	case "extract-editors":
		if len(args) != 3 {
			fmt.Fprintln(stderr, extractEditorsUsage)
			return 2, true
		}
		return extractEditors(args[1], args[2], stdout, stderr), true
	case "export-runtime":
		if len(args) != 2 {
			fmt.Fprintln(stderr, exportRuntimeUsage)
			return 2, true
		}
		return exportRuntime(args[1], stdout, stderr), true
	}
	return 0, false
}

// extractRow is one input file's answer in editors.json: what the file is, what the hub
// makes of it, and — when it can serve it — where the editor landed.
type extractRow struct {
	File    string         `json:"file"`             // base name of the input file
	SHA256  string         `json:"sha256"`           // of the whole input file, lowercase hex
	Bytes   int            `json:"bytes"`            // of the whole input file
	Status  string         `json:"status"`           // "ok" | "skipped" | "no-editor" | "unreadable"
	Reason  string         `json:"reason,omitempty"` // why, for every status but "ok"
	Slug    string         `json:"slug,omitempty"`
	Title   string         `json:"title,omitempty"`
	Version string         `json:"version,omitempty"`
	Caps    []string       `json:"caps,omitempty"`   // the container's own disclosure list
	OSMin   string         `json:"os_min,omitempty"` // the floor the editor's orb-app.js stamps
	Editor  *extractEditor `json:"editor,omitempty"` // only when status is "ok"
}

// extractEditor says what was unpacked for one app: the folder it is under, the document a
// bare /editor/<ip>/<slug>/ opens, the runtime ids the manifest lists, and every file, in
// the order the archive held them.
type extractEditor struct {
	Dir     string        `json:"dir"`     // the slug: files are at <out>/<dir>/<name>
	Entry   string        `json:"entry"`   // the document the hub opens for /editor/<ip>/<slug>/
	Runtime []string      `json:"runtime"` // editor.json's runtime ids, as listed ([] when none)
	Files   []extractFile `json:"files"`   // every member written, in archive order
}

// extractFile is one unpacked member, with what a site needs to name and verify it.
type extractFile struct {
	Name   string `json:"name"`
	Bytes  int    `json:"bytes"`
	SHA256 string `json:"sha256"`
}

// extractReport is editors.json: the format, the hub that wrote it, the runtimes that hub
// carries (so a reader can check an app's ids against the release it arrived beside), and
// one row per input file, in input order.
type extractReport struct {
	Format  int          `json:"format"`
	Hub     string       `json:"hub"`
	Runtime []string     `json:"runtime"`
	Apps    []extractRow `json:"apps"`
}

// extractEditors unpacks the editor of every container it is given. <in> is one file or a
// directory of them; <out> must be fresh, because a half-written tree from an interrupted
// run must never be read as a complete one.
func extractEditors(in, out string, stdout, stderr io.Writer) int {
	files, err := inputApps(in)
	if err != nil {
		return commandFailed(stderr, err)
	}
	root, err := freshOut(out, "extract-editors")
	if err != nil {
		return commandFailed(stderr, err)
	}
	defer root.Close()
	report := extractReport{Format: 1, Hub: version, Runtime: runtimeIDs(), Apps: []extractRow{}}
	seen := map[string]string{}
	for _, f := range files {
		row, werr := extractOneApp(root, f, seen)
		if werr != nil {
			// A write that failed is the one thing that stops the run: a report naming
			// files that are not on disk would be a lie, and the job's next step would
			// publish it. Whatever partial folders exist are left for the operator.
			return commandFailed(stderr, werr)
		}
		report.Apps = append(report.Apps, row)
		fmt.Fprintf(stdout, "%-10s %s\n", row.Status, rowDetail(row))
	}
	if err := writeReport(root, "editors.json", report); err != nil {
		return commandFailed(stderr, err)
	}
	return 0
}

// rowDetail is the line a human watching a pull wants: which app and how much of it for
// an editor that unpacked, which file and why not for one that did not.
func rowDetail(row extractRow) string {
	if row.Status == "ok" {
		return fmt.Sprintf("%s %s (%d files)", row.Slug, row.Version, len(row.Editor.Files))
	}
	return row.File + ": " + row.Reason
}

// inputApps lists the containers to read: the regular files of a directory whose name ends
// in .orbapp, not recursive, sorted by name — the order a release's shelf arrives in is
// the shelf's, not the author's — or the one file named.
func inputApps(in string) ([]string, error) {
	fi, err := os.Stat(in)
	if err != nil {
		return nil, err
	}
	if !fi.IsDir() {
		return []string{in}, nil
	}
	entries, err := os.ReadDir(in)
	if err != nil {
		return nil, err
	}
	var out []string
	for _, e := range entries {
		if e.Type().IsRegular() && strings.HasSuffix(e.Name(), ".orbapp") {
			out = append(out, filepath.Join(in, e.Name()))
		}
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("no .orbapp files in %s", in)
	}
	return out, nil
}

// freshOut checks <out> may be written into and opens it as a root: it must not exist, or
// exist and be an empty directory. Both commands refuse anything else in the same words
// but their own name, because the reason is the same — a tree this run did not write must
// not be mixed into, and a job must not publish from it.
func freshOut(out, command string) (*os.Root, error) {
	notFresh := fmt.Errorf("%s is not empty: %s writes into a fresh directory", out, command)
	switch fi, err := os.Stat(out); {
	case errors.Is(err, os.ErrNotExist):
		if err := os.Mkdir(out, 0o755); err != nil {
			return nil, err
		}
	case err != nil:
		return nil, err
	case !fi.IsDir():
		return nil, notFresh
	default:
		entries, err := os.ReadDir(out)
		if err != nil {
			return nil, err
		}
		if len(entries) > 0 {
			return nil, notFresh
		}
	}
	return os.OpenRoot(out)
}

// commandFailed is how an input or output problem leaves: the one line on stderr with the
// hub's own prefix, and exit code 1, which is what tells a release job to stop.
func commandFailed(stderr io.Writer, err error) int {
	fmt.Fprintf(stderr, "orb-manager: %v\n", err)
	return 1
}

// writeReport encodes a report into the out root. Two-space indent and no HTML escaping,
// so the file reads in a diff and a name with a quote in it stays itself; the trailing
// newline is json.Encoder's own. O_EXCL because nothing in a fresh directory may be
// overwritten, not even the report.
func writeReport(root *os.Root, name string, v any) error {
	f, err := root.OpenFile(name, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	enc := json.NewEncoder(f)
	enc.SetEscapeHTML(false)
	enc.SetIndent("", "  ")
	err = enc.Encode(v)
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	return err
}

// hexSha is the lowercase hex digest a report carries.
func hexSha(b []byte) string {
	sum := sha256.Sum256(b)
	return hex.EncodeToString(sum[:])
}

// extractOneApp reads one container and unpacks its editor if the hub would serve it. The
// order is the hub's own: what the file is, then whether it is a second copy of an app
// this run already took, then whether it carries an editor at all, then the archive.
// The error it returns is a failed write; every other refusal is a row.
func extractOneApp(root *os.Root, file string, seen map[string]string) (extractRow, error) {
	base := filepath.Base(file)
	row := extractRow{File: base, Status: "unreadable"}
	body, err := os.ReadFile(file)
	if err != nil {
		row.Reason = err.Error()
		return row, nil
	}
	row.SHA256, row.Bytes = hexSha(body), len(body)
	c, err := orbnet.ParseApp(body)
	if err != nil {
		row.Reason = orbnetRefusal(err)
		return row, nil
	}
	row.Slug, row.Title, row.Version = c.Slug, c.Title, c.Version
	row.Caps, row.OSMin = c.CapNames(), appOSMin(body)
	// One app, one editor: two files carrying the same slug are the same app twice, and
	// the folder under <out> is named for the slug, so the second one cannot be written.
	if _, dup := seen[c.Slug]; dup {
		row.Status, row.Reason = "skipped",
			fmt.Sprintf("%s is also the %s app; only the first is extracted", base, c.Slug)
		return row, nil
	}
	seen[c.Slug] = base
	if c.EditorSize == 0 {
		row.Status, row.Reason = "no-editor", "this app ships no editor"
		return row, nil
	}
	region, err := orbnet.AppEditorRegion(body)
	if err != nil {
		row.Status, row.Reason = "unreadable", orbnetRefusal(err)
		return row, nil
	}
	res, err := extractEditorRegion(root, c.Slug, region)
	if err != nil {
		return row, err
	}
	if res.reason != "" {
		row.Status, row.Reason = res.status, res.reason
		return row, nil
	}
	row.Status, row.Editor = "ok", res.editor
	return row, nil
}

// orbnetRefusal is the container reader's sentence without the package prefix the dial's
// console already carries: the reason an operator reads, not the Go package that found it.
func orbnetRefusal(err error) string {
	return strings.TrimPrefix(err.Error(), "orbnet: ")
}

// regionOutcome is one archive's answer: the editor that was written, or the status and the
// reason it was not.
type regionOutcome struct {
	editor *extractEditor
	status string
	reason string
}

// hostedEntryName is the page extract-editors writes for a hosted editor (host@1): the
// hub's own editor page, rendered as the hub would serve it at the editor's address.
const hostedEntryName = "index.html"

func skipped(reason string) regionOutcome { return regionOutcome{status: "skipped", reason: reason} }

// extractEditorRegion is the per-archive half of the contract: read the region as a ZIP,
// refuse what the hub would not serve, and only then write it. It takes the region bytes
// and the out root rather than a container, so a test can hand it the archives a hostile
// upload would carry without baking an .orbapp around each one, and so the checks are the
// same function the shipped shelf passes.
//
// Nothing is written until every member has been named, sized and resolved: an archive
// refused at its tenth entry must leave no trace of its first nine.
func extractEditorRegion(root *os.Root, slug string, region []byte) (regionOutcome, error) {
	zr, err := zip.NewReader(bytes.NewReader(region), int64(len(region)))
	if err != nil {
		return regionOutcome{status: "unreadable",
			reason: fmt.Sprintf("the editor region for %s is not a ZIP archive: %v", slug, err)}, nil
	}
	// Names first, in archive order. Each check below is a write that would otherwise land
	// somewhere the slug's folder does not own, or a member the hub could never answer.
	var names []string
	seenName := map[string]bool{}
	seenFold := map[string]string{}
	for _, f := range zr.File {
		name := f.Name
		if !plainRelPath(name) {
			return skipped(fmt.Sprintf("the editor carries a member named %q, which is not a plain relative path", name)), nil
		}
		if f.FileInfo().IsDir() {
			continue // a folder entry carries no bytes, and its name was just checked
		}
		if f.Mode().Type() != 0 {
			return skipped(fmt.Sprintf("the editor member %q is not a plain file", name)), nil
		}
		if seenName[name] {
			return skipped(fmt.Sprintf("the editor carries %q twice", name)), nil
		}
		if other, clash := seenFold[strings.ToLower(name)]; clash {
			return skipped(fmt.Sprintf("the editor members %q and %q differ only in case", other, name)), nil
		}
		seenName[name], seenFold[strings.ToLower(name)] = true, name
		names = append(names, name)
	}
	// A name that is also a folder of another member cannot be written as either: one of
	// the two writes must fail, and the other would be answered by a 404.
	dirs := map[string]bool{}
	for _, name := range names {
		for d := path.Dir(name); d != "." && d != "/"; d = path.Dir(d) {
			dirs[d] = true
		}
	}
	for _, name := range names {
		if dirs[name] {
			return skipped(fmt.Sprintf("the editor member %q is both a file and a folder", name)), nil
		}
	}
	// Bytes next, through the hub's own reader, so the per-file limit is the same one a
	// live dial pays: a declared size is a promise until the bytes are in hand.
	bodies := make(map[string][]byte, len(names))
	files := make([]extractFile, 0, len(names))
	for _, name := range names {
		data, _, err := editorBytes(zr, name)
		if err != nil {
			return skipped(err.Error()), nil
		}
		bodies[name] = data
		files = append(files, extractFile{Name: name, Bytes: len(data), SHA256: hexSha(data)})
	}
	// The entry, exactly as serveEditor resolves a bare /editor/<ip>/<slug>/: the
	// manifest's answer when there is one, index.html when there is no manifest at all.
	entry, manifest := "", seenName[editorManifestName]
	if manifest {
		if entry = rawEntryName(bodies[editorManifestName]); entry == "" {
			// A hosted editor (editor.json "host": 1) names no page: the hub serves Orb
			// Manager's own (runtime host@1) with the app's modules in it. A site has no hub
			// to write that page, so it is written here, from the same template and the same
			// checks serveHostedEditor applies, as the folder's index.html.
			p, hosted, why := hostedEditor(bodies[editorManifestName], zr)
			if !hosted {
				return skipped("editor.json names no entry the hub can open"), nil
			}
			if why != "" {
				return skipped(why), nil
			}
			if seenName[hostedEntryName] {
				return skipped("a hosted editor carries its own " + hostedEntryName + ", which the hub's page would stand in for"), nil
			}
			if missing := editorRuntimeMissing(bodies[editorManifestName]); len(missing) > 0 {
				return skipped(runtimeMissingSentence(slug, missing)), nil
			}
			p.Title = slug + " theme editor"
			var buf bytes.Buffer
			if err := hostedTmpl.Execute(&buf, p); err != nil {
				return skipped("the hosted editor page could not be written: " + err.Error()), nil
			}
			entry = hostedEntryName
			bodies[entry] = buf.Bytes()
			files = append(files, extractFile{Name: entry, Bytes: buf.Len(), SHA256: hexSha(buf.Bytes())})
			seenName[entry] = true
		}
	} else {
		entry = "index.html"
	}
	if !seenName[entry] {
		return skipped("the editor has no " + entry), nil
	}
	// The gate the door itself puts on an HTML entry. With an editor.json the entry is
	// always a document — rawEntryName only answers one — so this is the same gate
	// serveEditor applies, and its same sentence: an editor naming an id this binary lacks
	// is refused by name rather than unpacked to paint nothing.
	if manifest {
		if missing := editorRuntimeMissing(bodies[editorManifestName]); len(missing) > 0 {
			return skipped(runtimeMissingSentence(slug, missing)), nil
		}
	}
	return writeRegion(root, slug, entry, manifestIDs(bodies[editorManifestName], manifest), files, bodies)
}

// plainRelPath reports whether a member name is a plain relative path: no root, no parent
// step, no empty or dotted step, and none of the separators, drive letters or NULs that
// make one name mean two places on two systems. A trailing "/" marks a folder entry and is
// not a step of its own.
func plainRelPath(name string) bool {
	if name == "" || strings.ContainsRune(name, 0) || strings.Contains(name, `\`) ||
		strings.Contains(name, ":") || strings.HasPrefix(name, "/") {
		return false
	}
	if name = strings.TrimSuffix(name, "/"); name == "" {
		return false
	}
	for _, seg := range strings.Split(name, "/") {
		if seg == "" || seg == "." || seg == ".." {
			return false
		}
	}
	return true
}

// manifestIDs is editor.json's runtime list as the app wrote it, for the report: a manifest
// that does not parse or names nothing answers [], because the field is absent rather than
// empty. The gate above already refused what this binary cannot serve.
func manifestIDs(manifest []byte, present bool) []string {
	out := []string{}
	if !present {
		return out
	}
	var m struct {
		Runtime []string `json:"runtime"`
	}
	if json.Unmarshal(manifest, &m) == nil && m.Runtime != nil {
		out = append(out, m.Runtime...)
	}
	return out
}

// writeRegion puts one app's members down under `.<slug>.partial` and renames the folder to
// <slug> only once the last file is closed, so the directory named for the app either
// holds the whole editor or does not exist. A failed write returns the error, and the
// partial folder with it: the command exits 1 and an operator sees what was attempted.
func writeRegion(root *os.Root, slug, entry string, ids []string, files []extractFile, bodies map[string][]byte) (regionOutcome, error) {
	partial := "." + slug + ".partial"
	if err := root.RemoveAll(partial); err != nil {
		return regionOutcome{}, err
	}
	if err := root.MkdirAll(partial, 0o755); err != nil {
		return regionOutcome{}, err
	}
	for _, f := range files {
		if err := writeRootFile(root, partial+"/"+f.Name, bodies[f.Name]); err != nil {
			return regionOutcome{}, err
		}
	}
	if err := root.Rename(partial, slug); err != nil {
		return regionOutcome{}, err
	}
	return regionOutcome{editor: &extractEditor{Dir: slug, Entry: entry, Runtime: ids, Files: files}}, nil
}

// writeRootFile creates one file, and the folders above it, inside a root: folders 0o755,
// files 0o644, and O_EXCL so nothing already there is ever replaced — a fresh <out> has no
// right to hold a file this run did not write.
func writeRootFile(root *os.Root, name string, data []byte) error {
	if d := path.Dir(name); d != "." {
		if err := root.MkdirAll(d, 0o755); err != nil {
			return err
		}
	}
	f, err := root.OpenFile(name, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	_, werr := f.Write(data)
	if cerr := f.Close(); werr == nil {
		werr = cerr
	}
	return werr
}

// runtimeFile is one exported runtime file in runtime.json.
type runtimeFile struct {
	Name   string `json:"name"`
	Bytes  int    `json:"bytes"`
	SHA256 string `json:"sha256"`
}

// runtimeIDRow is one runtime id's export: the id, whether its bytes are third-party (so a
// site may cache it as immutably as the hub does), and the files, sorted by name.
type runtimeIDRow struct {
	ID     string        `json:"id"`
	Vendor bool          `json:"vendor"`
	Files  []runtimeFile `json:"files"`
}

// runtimeReport is runtime.json, with the same encoding rules as editors.json.
type runtimeReport struct {
	Format int            `json:"format"`
	Hub    string         `json:"hub"`
	IDs    []runtimeIDRow `json:"ids"`
}

// exportRuntime writes the editor runtime laid out as serveRuntime answers it, at
// <out>/<id>/<file>, for exactly the ids this binary carries. A site serves the tree beside
// the editors extract-editors unpacked, and an editor that opens against a hub's release
// then loads the same bytes it would have loaded from that hub — which is the whole reason
// this command exists instead of a copy of web/runtime in the site's repo.
func exportRuntime(out string, stdout, stderr io.Writer) int {
	root, err := freshOut(out, "export-runtime")
	if err != nil {
		return commandFailed(stderr, err)
	}
	defer root.Close()
	report := runtimeReport{Format: 1, Hub: version, IDs: []runtimeIDRow{}}
	for _, id := range runtimeIDs() { // sorted, so the tree and the report are stable
		e := runtimeCatalog[id]
		names, err := runtimeFiles(e)
		if err != nil {
			return commandFailed(stderr, err)
		}
		files := make([]runtimeFile, 0, len(names))
		var total int
		for _, rel := range names {
			data, err := webFS.ReadFile(runtimeEmbedDir + "/" + e.dir + "/" + rel)
			if err != nil {
				// runtimeIDs said the hub carries this id, so the only way the bytes are
				// not there is a build that changed under the catalog: say so and stop.
				return commandFailed(stderr, fmt.Errorf("%s: %w", id, err))
			}
			if err := writeRootFile(root, id+"/"+rel, data); err != nil {
				return commandFailed(stderr, err)
			}
			files = append(files, runtimeFile{Name: rel, Bytes: len(data), SHA256: hexSha(data)})
			total += len(data)
		}
		sort.Slice(files, func(i, j int) bool { return files[i].Name < files[j].Name })
		report.IDs = append(report.IDs, runtimeIDRow{ID: id, Vendor: e.vendor, Files: files})
		fmt.Fprintf(stdout, "%s %d files %d B\n", id, len(files), total)
	}
	if err := writeReport(root, "runtime.json", report); err != nil {
		return commandFailed(stderr, err)
	}
	return 0
}

// runtimeFiles names the files the hub would serve for one catalog entry: everything under
// its directory for an id that owns a whole directory, and the listed files — plus the
// prefix's subtree, which is how the font faces travel with the sheet that names them —
// for an id that owns a few files out of a shared directory.
func runtimeFiles(e runtimeEntry) ([]string, error) {
	base := runtimeEmbedDir + "/" + e.dir
	if len(e.files) == 0 {
		return walkRegularFiles(base)
	}
	names := append([]string{}, e.files...)
	if e.prefix != "" {
		sub, err := walkRegularFiles(base + "/" + strings.TrimSuffix(e.prefix, "/"))
		if err != nil {
			return nil, err
		}
		// The names come back from inside the subtree, and a file's URL is the path the
		// hub reads it at: faces.css is at fonts@1/fonts/<face>, not fonts@1/<face>.
		for _, n := range sub {
			names = append(names, path.Join(e.prefix, n))
		}
	}
	sort.Strings(names)
	return names, nil
}

// walkRegularFiles lists every regular file under an embedded directory, at any depth,
// relative to it. A directory that is not in the binary at all is an empty answer: the
// hub serves 404s for it, and an export that omits it matches what it serves.
func walkRegularFiles(dir string) ([]string, error) {
	if _, err := fs.Stat(webFS, dir); err != nil {
		if errors.Is(err, fs.ErrNotExist) {
			return nil, nil
		}
		return nil, err
	}
	var out []string
	err := fs.WalkDir(webFS, dir, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.Type().IsRegular() {
			out = append(out, strings.TrimPrefix(p, dir+"/"))
		}
		return nil
	})
	sort.Strings(out)
	return out, err
}
