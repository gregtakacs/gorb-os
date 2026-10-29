package firmware

import (
	"strings"
	"testing"
)

// The accept list is the whole grammar in the shapes tools/orb_version.sh can
// actually emit, so a regression here is a build that reports itself versionless.
func TestParseVersionAcceptsRealStamps(t *testing.T) {
	for _, tc := range []struct {
		raw              string
		major, minor, pa int
		pre, commit      string
		dirty            bool
	}{
		{"v1.0.0+e1a2b3c", 1, 0, 0, "", "e1a2b3c", false},
		{"v1.0.0+e1a2b3c-dirty", 1, 0, 0, "", "e1a2b3c", true},
		{"v1.3.0-rc.1+aabbccd", 1, 3, 0, "rc.1", "aabbccd", false},
		{"v0.0.0+0000000", 0, 0, 0, "", "0000000", false},
		{"  v1.2.3+aaaaaaa  ", 1, 2, 3, "", "aaaaaaa", false}, // the trim is part of the contract
	} {
		v, ok := ParseVersion(tc.raw)
		if !ok {
			t.Errorf("ParseVersion(%q) refused a real stamp", tc.raw)
			continue
		}
		if v.Major != tc.major || v.Minor != tc.minor || v.Patch != tc.pa {
			t.Errorf("ParseVersion(%q) = %d.%d.%d, want %d.%d.%d",
				tc.raw, v.Major, v.Minor, v.Patch, tc.major, tc.minor, tc.pa)
		}
		if v.Pre != tc.pre || v.Commit != tc.commit || v.Dirty != tc.dirty {
			t.Errorf("ParseVersion(%q) = pre %q commit %q dirty %v, want pre %q commit %q dirty %v",
				tc.raw, v.Pre, v.Commit, v.Dirty, tc.pre, tc.commit, tc.dirty)
		}
		if want := strings.TrimSpace(tc.raw); v.Raw != want {
			t.Errorf("ParseVersion(%q).Raw = %q, want the trimmed input", tc.raw, v.Raw)
		}
	}
}

// Everything pre-scheme must be refused. A bare SHA that half-parsed into a bogus
// 0.0.0 would order below every real release and quietly mark a working orb ancient.
func TestParseVersionRefusesEverythingElse(t *testing.T) {
	for _, raw := range []string{
		"8eb4bf0", "8eb4bf0-dirty", "074afed", // the legacy git describe forms
		"1", "", "v1.2", "v1.2.0.4", "v1.2.0+abc", "v1.2.0-", "v1.2.0-rc..1",
		"V1.2.0+aaaaaaa",
		"v1.2.3-rc.1+e1a2b3c-dirty-and-then-some-more-bytes-here", // past the field width
		"v123.456.789-rc.999+e1a2b3c-dirty",                       // 33 B: three digits per component cannot fit
		"v1.0.0+e1a2b3C", "v1.0.0+e1a2b3cc", "v1.0.0+e1a2b3",      // metadata not 7 lowercase hex
		"v1.0.0dirty", "v1.0.0+-e1a2b3c", "nv1.0.0+e1a2b3c",
	} {
		if v, ok := ParseVersion(raw); ok {
			t.Errorf("ParseVersion(%q) = %#v, want refused", raw, v)
		}
	}
}

// The order is the reason the scheme exists: an upgrade decision that a clock skew
// could flip is the defect this replaces.
func TestVersionOrdering(t *testing.T) {
	for _, tc := range []struct {
		a, b string
		want int
	}{
		{"v1.9.0+aaaaaaa", "v1.10.0+aaaaaaa", -1}, // numeric, not lexical
		{"v1.2.3+aaaaaaa", "v1.2.3+bbbbbbb", 0},   // metadata never decides newer
		{"v1.2.3-rc.9+aaaaaaa", "v1.2.3-rc.9+aaaaaaa-dirty", 0},
		{"v1.3.0-rc.1+aaaaaaa", "v1.3.0-rc.2+aaaaaaa", -1},
		{"v1.3.0-rc.1+aaaaaaa", "v1.3.0+aaaaaaa", -1}, // a candidate below its release
		{"v1.3.0-alpha+aaaaaaa", "v1.3.0-alpha.1+aaaaaaa", -1},
		{"v1.3.0-alpha.1+aaaaaaa", "v1.3.0-alpha.beta+aaaaaaa", -1},
		{"v1.3.0-alpha.beta+aaaaaaa", "v1.3.0-beta+aaaaaaa", -1},
		{"v1.3.0-beta+aaaaaaa", "v1.3.0-rc.1+aaaaaaa", -1},
		{"v2.0.0+aaaaaaa", "v1.99.99+bbbbbbb", 1},
	} {
		av, aok := ParseVersion(tc.a)
		bv, bok := ParseVersion(tc.b)
		if !aok || !bok {
			t.Errorf("ordering test built on an unparsable pair: %q ok=%v, %q ok=%v", tc.a, aok, tc.b, bok)
			continue
		}
		if got := av.Compare(bv); got != tc.want {
			t.Errorf("Compare(%q, %q) = %d, want %d", tc.a, tc.b, got, tc.want)
		}
		if got := bv.Compare(av); got != -tc.want {
			t.Errorf("Compare(%q, %q) = %d, want %d: the order must be antisymmetric", tc.b, tc.a, got, -tc.want)
		}
	}
}

// Two digits per component is a width decision, not a style one: the descriptor
// field holds 31 bytes and a three-digit grammar needs 32 for its worst case.
func TestWorstCaseFitsTheDescriptor(t *testing.T) {
	const worst = "v99.99.99-rc.99+e1a2b3c-dirty"
	if len(worst) > 31 {
		t.Errorf("the worst-case stamp is %d B; esp_app_desc_t.version holds 31", len(worst))
	}
	if _, ok := ParseVersion(worst); !ok {
		t.Errorf("ParseVersion(%q) refused the very string the grammar is sized for", worst)
	}
	for _, tooBig := range []string{"v123.0.0+aaaaaaa", "v1.0.100+aaaaaaa"} {
		if _, ok := ParseVersion(tooBig); ok {
			t.Errorf("ParseVersion(%q) accepted a three-digit component, which cannot fit the field", tooBig)
		}
	}
}

// What a human calls the release is the declared part, metadata dropped.
func TestDeclaredDropsMetadata(t *testing.T) {
	v, ok := ParseVersion("v1.2.3-rc.1+e1a2b3c-dirty")
	if !ok {
		t.Fatal("ParseVersion refused a stamp this scheme emits")
	}
	if got := v.Declared(); got != "v1.2.3-rc.1" {
		t.Errorf("Declared() = %q, want v1.2.3-rc.1", got)
	}
}
