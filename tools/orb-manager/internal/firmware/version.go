package firmware

import (
	"regexp"
	"strconv"
	"strings"
)

// Version is a parsed gorb-os firmware stamp: v1.2.3-rc.1+e1a2b3c-dirty. The triple
// is the declared release, Pre marks a candidate, Commit names the tree it was built
// from, and Dirty says the build carried uncommitted edits. Ordering reads the triple
// and the Pre only: Commit and Dirty distinguish builds, and a distinguishing fact
// must not decide which one is newer.
type Version struct {
	Major, Minor, Patch int
	Pre                 string
	Commit              string
	Dirty               bool
	Raw                 string
}

// versionRe is the whole scheme, anchored, so a legacy git describe cannot half-parse:
// 8eb4bf0-dirty has no leading v and no triple, and fails here rather than becoming a
// bogus 0.0.0 that would outrank nothing.
var versionRe = regexp.MustCompile(`^v(\d{1,2})\.(\d{1,2})\.(\d{1,2})` +
	`(?:-([0-9A-Za-z][0-9A-Za-z.-]*))?(?:\+([0-9a-f]{7}))?(-dirty)?$`)

// ParseVersion reads the stamp exactly as tools/orb_version.sh writes it. ok is false
// for every pre-scheme string, and callers must then fall back to the build timestamp
// rather than guess: firmware built before this scheme exists is real firmware, and
// reporting it as versionless would refuse a working orb.
func ParseVersion(raw string) (v Version, ok bool) {
	m := versionRe.FindStringSubmatch(strings.TrimSpace(raw))
	if m == nil {
		return Version{}, false
	}
	v = Version{
		Pre:    m[4],
		Commit: m[5],
		Dirty:  m[6] != "",
		Raw:    strings.TrimSpace(raw),
	}
	// Every group matched, so each Atoi is infallible. Ignore the errors rather than
	// branch on them, and let a zero stand.
	v.Major, _ = strconv.Atoi(m[1])
	v.Minor, _ = strconv.Atoi(m[2])
	v.Patch, _ = strconv.Atoi(m[3])
	if !validPre(v.Pre) {
		return Version{}, false
	}
	return v, true
}

// validPre enforces the two semver rules a regex cannot reach: no empty identifier,
// and no leading zero on a numeric one. comparePre splits on the same dots, so an
// empty piece would compare as nothing and a padded "01" would sort against "1".
func validPre(pre string) bool {
	if pre == "" {
		return true
	}
	for _, id := range strings.Split(pre, ".") {
		if id == "" {
			return false
		}
		numeric := true
		for i := 0; i < len(id); i++ {
			if id[i] < '0' || id[i] > '9' {
				numeric = false
				break
			}
		}
		if numeric && len(id) > 1 && id[0] == '0' {
			return false
		}
	}
	return true
}

// Declared is the version with no build metadata: what a human calls the release.
func (v Version) Declared() string {
	s := "v" + strconv.Itoa(v.Major) + "." + strconv.Itoa(v.Minor) + "." + strconv.Itoa(v.Patch)
	if v.Pre != "" {
		return s + "-" + v.Pre
	}
	return s
}

// Compare returns -1, 0, or 1 by semver precedence. Commit metadata is ignored, so
// v1.2.3+aaaaaaa and v1.2.3+bbbbbbb are one release, and a -dirty build of a release
// still IS that release.
func (v Version) Compare(o Version) int {
	if v.Major != o.Major {
		return sign(v.Major - o.Major)
	}
	if v.Minor != o.Minor {
		return sign(v.Minor - o.Minor)
	}
	if v.Patch != o.Patch {
		return sign(v.Patch - o.Patch)
	}
	return comparePre(v.Pre, o.Pre)
}

func sign(d int) int {
	switch {
	case d < 0:
		return -1
	case d > 0:
		return 1
	}
	return 0
}

// comparePre orders a pre-release below the release it precedes, then identifier by
// identifier as semver defines it: numeric identifiers compare numerically,
// alphanumeric lexically, numeric below alphanumeric, and a shorter set below a longer
// one that starts with it. v1.3.0-rc.1 < v1.3.0-rc.2 < v1.3.0.
func comparePre(a, b string) int {
	switch {
	case a == b:
		return 0
	case a == "":
		return 1 // a final release outranks any candidate of it
	case b == "":
		return -1
	}
	as, bs := strings.Split(a, "."), strings.Split(b, ".")
	for i := 0; i < len(as) && i < len(bs); i++ {
		if c := compareIdent(as[i], bs[i]); c != 0 {
			return c
		}
	}
	return sign(len(as) - len(bs))
}

func compareIdent(a, b string) int {
	an, aerr := strconv.ParseInt(a, 10, 64)
	bn, berr := strconv.ParseInt(b, 10, 64)
	switch {
	case aerr == nil && berr == nil:
		return sign(int(an - bn))
	case aerr == nil: // numeric ranks below alphanumeric
		return -1
	case berr == nil:
		return 1
	}
	return strings.Compare(a, b)
}
