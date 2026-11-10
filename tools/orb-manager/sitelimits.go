package main

// S3 (ruling 35): the bounds "Import from orb-os.net" keeps to itself.
//
// Two of them. The first is whose links this hub will open at all: only the configured
// site — GORB_SITE, the one origin release.go's resolver answers, so a bench and the
// site's §11 move are the same one string — and orb-os.net itself, plus loopback, which
// is how the hub's own tests and a developer's machine site are served. A link to any
// other host is refused here, before a socket opens: dialing an unknown host with the
// design's id would tell that host both that this owner imports from orb-os.net and
// which design they were after.
//
// The second is what a bundle's art may be. The site applies these rules when it stores
// a picture, so a bundle that arrived through the site's own doors already holds to
// them, and one that does not is not a bundle the site served: a picture that is not a
// PNG, JPEG, GIF or WebP, or one whose header names a side longer than 1024 pixels. The
// size is read from the header and nothing else — no pixel is decoded, so a 60-byte file
// claiming 50000 × 50000 is refused as surely as a real one of that size would be. The
// parsing below is the site's src/lib/image-header.ts and design-assets.ts, ported; the
// sentences are the hub's own, worded to finish the owner's "the theme … " sentence.

import (
	"encoding/base64"
	"fmt"
	"net/url"
	"regexp"
	"sort"
	"strings"
)

// orbOSNet is the site's future home (§11): always allowed, https only.
const orbOSNet = "orb-os.net"

// siteAllowed reports whether a canonical site origin (scheme://host[:port], as
// parseSiteLink answers it) is one Orb Manager may import from or ask about.
func siteAllowed(site string) bool {
	scheme, host, hostport, ok := siteOriginParts(site)
	if !ok {
		return false
	}
	// A developer's own site and this hub's tests are served on loopback, http included,
	// exactly as parseSiteLink already lets plain http through to these.
	switch host {
	case "localhost", "127.0.0.1", "::1":
		return true
	}
	if c, _, cp, cok := siteOriginParts(siteReleaseBase()); cok && scheme == c && hostport == cp {
		return true
	}
	return scheme == "https" && hostport == orbOSNet
}

// siteNotAllowed is the refusal for a link to any other host. It names the two places a
// theme's link can have come from, so the owner reads it as "paste the other link"
// rather than as a failure of the site they meant.
func siteNotAllowed(site string) error {
	conf := canonicalSiteOrigin(siteReleaseBase())
	host := siteHost(site)
	if conf == "https://"+orbOSNet {
		return fmt.Errorf("Orb Manager imports themes only from %s, not from %s", orbOSNet, host)
	}
	return fmt.Errorf("Orb Manager imports themes only from %s and %s, not from %s", siteHost(conf), orbOSNet, host)
}

// siteOriginParts splits a site origin the way the comparisons need it: the scheme and
// the host lower-cased, and the host with its port — the name in the sentences and the
// thing the configured origin is matched against, port and all. A value that is not an
// http(s) origin has no parts and matches nothing.
func siteOriginParts(site string) (scheme, host, hostport string, ok bool) {
	u, err := url.Parse(site)
	if err != nil || u.Host == "" || u.User != nil {
		return "", "", "", false
	}
	scheme = strings.ToLower(u.Scheme)
	if scheme != "http" && scheme != "https" {
		return "", "", "", false
	}
	host = strings.ToLower(u.Hostname())
	hostport = strings.ToLower(u.Host)
	// An explicit :443 says nothing an https origin does not already mean, and a
	// GORB_SITE written with it must name the same site as one written without.
	if scheme == "https" {
		hostport = strings.TrimSuffix(hostport, ":443")
	}
	return scheme, host, hostport, true
}

// canonicalSiteOrigin is siteOriginParts put back together, lowercase and with :443
// gone, the spelling parseSiteLink answers a typed link in. "" is a value that is not
// an origin at all and is allowed nowhere.
func canonicalSiteOrigin(site string) string {
	scheme, _, hostport, ok := siteOriginParts(site)
	if !ok {
		return ""
	}
	return scheme + "://" + hostport
}

// --- the picture's header -------------------------------------------------------------

// imageHeader is the type and pixel size a picture declares in its header.
type imageHeader struct {
	Type   string // "png" | "jpeg" | "gif" | "webp"
	Width  int
	Height int
}

// artMaxSide is ruling 35's ceiling: the longest side, in pixels, a design's picture may
// declare. The site's ART_MAX_SIDE.
const artMaxSide = 1024

// artMinHeaderBytes is the site's ART_MIN_HEADER_BYTES: fewer bytes than this cannot
// declare a size in any of the four formats, and the hub's own contract carries such
// tiny placeholders. Too short to name a size is not a refusal.
const artMinHeaderBytes = 10

// The three refusals, verbatim; the size sentence is a format over the header's sides.
const (
	artRefusalType       = "an image in it is not a PNG, JPEG, GIF or WebP, the only pictures a design can hold"
	artRefusalUnreadable = "an image in it can't be read as a PNG, JPEG, GIF or WebP"
	artRefusalSizeFormat = "an image in it is %d × %d pixels, and a design's pictures can be at most %d × %d"
)

// artPrefixes is the site's ART_PREFIXES: the four data: prefixes a picture may arrive
// under, compared lower-cased.
var artPrefixes = []string{"data:image/png;base64", "data:image/jpeg;base64", "data:image/gif;base64", "data:image/webp;base64"}

// artRefusal is "" when bytes announced by this data-URL prefix are a picture a design
// may hold, and the sentence otherwise — the site's artRefusal, in its order: the prefix
// first, because it is free; then the too-short pass-through; then the header.
func artRefusal(prefix string, b []byte) string {
	lower := strings.ToLower(prefix)
	known := false
	for _, p := range artPrefixes {
		if p == lower {
			known = true
			break
		}
	}
	if !known {
		return artRefusalType
	}
	if len(b) < artMinHeaderBytes {
		return ""
	}
	h, ok := readImageHeader(b)
	if !ok {
		return artRefusalUnreadable
	}
	if h.Width > artMaxSide || h.Height > artMaxSide {
		return fmt.Sprintf(artRefusalSizeFormat, h.Width, h.Height, artMaxSide, artMaxSide)
	}
	return ""
}

// bundleArtError is a checkBundle refusal that is not damage: the theme arrived exactly
// as its hashes say and rules outside that refuse its art. handleSiteImport words it
// apart from "is damaged" for that reason — the owner's fix is a smaller picture, not a
// re-download.
type bundleArtError struct {
	sentence string
}

func (e *bundleArtError) Error() string { return e.sentence }

// dataURLRe is the site's /^data:[^,\s]*,/i: anything at all shaped like a data URL,
// whatever it claims to be and including the non-base64 spellings that are art's not to
// be. The prefix it names is cut at the first comma.
var dataURLRe = regexp.MustCompile(`(?i)^data:[^,\s]*,`)

// checkInlineArt walks a design document for data URLs — every string at any depth,
// objects and arrays, the orbAssets sidecar left to checkBundleArt — and refuses the
// first one that is not a picture a design may hold. The site asks its splitAssets the
// same question of every string before it stores one; art the hub is handed inline must
// clear the same bar as art in the sidecar.
func checkInlineArt(v any) error {
	switch t := v.(type) {
	case string:
		if !dataURLRe.MatchString(t) {
			return nil // not a data URL: a name, a color, anything else the design keeps
		}
		comma := strings.IndexByte(t, ',')
		prefix := t[:comma]
		if s := artRefusal(prefix, nil); s != "" {
			return &bundleArtError{s}
		}
		body, err := base64.StdEncoding.DecodeString(t[comma+1:])
		if err != nil {
			return &bundleArtError{artRefusalUnreadable}
		}
		if s := artRefusal(prefix, body); s != "" {
			return &bundleArtError{s}
		}
	case []any:
		for _, el := range t {
			if err := checkInlineArt(el); err != nil {
				return err
			}
		}
	case map[string]any:
		keys := make([]string, 0, len(t))
		for k := range t {
			keys = append(keys, k)
		}
		sort.Strings(keys) // so the same document refuses with the same sentence twice
		for _, k := range keys {
			if k == "orbAssets" {
				continue
			}
			if err := checkInlineArt(t[k]); err != nil {
				return err
			}
		}
	}
	return nil
}

// --- the header reader ----------------------------------------------------------------
// A port of the site's src/lib/image-header.ts: bytes only, no image package, and an
// incomplete header of any of the four formats is not a header at all.

var (
	pngSign   = []byte{0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a}
	gif87a    = []byte{0x47, 0x49, 0x46, 0x38, 0x37, 0x61}
	gif89a    = []byte{0x47, 0x49, 0x46, 0x38, 0x39, 0x61}
	riffStart = []byte{0x52, 0x49, 0x46, 0x46}
	jpegSign  = []byte{0xff, 0xd8}
)

func byteAt(b []byte, at int) (byte, bool) {
	if at < 0 || at >= len(b) {
		return 0, false
	}
	return b[at], true
}

func u16le(b []byte, at int) (int, bool) {
	if at < 0 || at+2 > len(b) {
		return 0, false
	}
	return int(b[at]) | int(b[at+1])<<8, true
}

func u24le(b []byte, at int) (int, bool) {
	if at < 0 || at+3 > len(b) {
		return 0, false
	}
	return int(b[at]) | int(b[at+1])<<8 | int(b[at+2])<<16, true
}

func u32le(b []byte, at int) (int, bool) {
	if at < 0 || at+4 > len(b) {
		return 0, false
	}
	return int(b[at]) | int(b[at+1])<<8 | int(b[at+2])<<16 | int(b[at+3])<<24, true
}

func u16be(b []byte, at int) (int, bool) {
	if at < 0 || at+2 > len(b) {
		return 0, false
	}
	return int(b[at])<<8 | int(b[at+1]), true
}

func u32be(b []byte, at int) (int, bool) {
	if at < 0 || at+4 > len(b) {
		return 0, false
	}
	return int(b[at])<<24 | int(b[at+1])<<16 | int(b[at+2])<<8 | int(b[at+3]), true
}

func startsWith(b []byte, sign []byte) bool {
	if len(b) < len(sign) {
		return false
	}
	for i := range sign {
		if b[i] != sign[i] {
			return false
		}
	}
	return true
}

func fourcc(b []byte, at int) (string, bool) {
	if at < 0 || at+4 > len(b) {
		return "", false
	}
	return string(b[at : at+4]), true
}

// colourTableSize is the bytes of a GIF colour table its packed flags name.
func colourTableSize(flags byte) int {
	return 3 * (1 << ((flags & 7) + 1))
}

// pastSubBlocks is the position after a run of length-prefixed sub-blocks, and false
// when one of them is cut short.
func pastSubBlocks(b []byte, at int) (int, bool) {
	pos := at
	for {
		n, ok := byteAt(b, pos)
		if !ok {
			return 0, false
		}
		pos++
		if n == 0 {
			return pos, true
		}
		pos += int(n)
		if pos > len(b) {
			return 0, false
		}
	}
}

func pngHeader(b []byte) (imageHeader, bool) {
	var no imageHeader
	if !startsWith(b, pngSign) {
		return no, false
	}
	if c, ok := fourcc(b, 12); !ok || c != "IHDR" {
		return no, false
	}
	w, ok1 := u32be(b, 16)
	h, ok2 := u32be(b, 20)
	if !ok1 || !ok2 || w == 0 || h == 0 {
		return no, false
	}
	return imageHeader{Type: "png", Width: w, Height: h}, true
}

func gifHeader(b []byte) (imageHeader, bool) {
	var no imageHeader
	if !startsWith(b, gif87a) && !startsWith(b, gif89a) {
		return no, false
	}
	screenW, ok1 := u16le(b, 6)
	screenH, ok2 := u16le(b, 8)
	flags, ok3 := byteAt(b, 10)
	if !ok1 || !ok2 || !ok3 || screenW == 0 || screenH == 0 {
		return no, false
	}
	width, height := screenW, screenH
	pos := 13
	if flags&0x80 != 0 {
		pos += colourTableSize(flags)
	}
	for {
		label, ok := byteAt(b, pos)
		if !ok {
			return no, false
		}
		pos++
		switch {
		case label == 0x21: // an extension: its label byte, then its sub-blocks
			if _, ok := byteAt(b, pos); !ok {
				return no, false
			}
			pos++
			next, ok := pastSubBlocks(b, pos)
			if !ok {
				return no, false
			}
			pos = next
		case label == 0x2c: // an image descriptor: its geometry grows the canvas
			left, ok1 := u16le(b, pos)
			top, ok2 := u16le(b, pos+2)
			frameW, ok3 := u16le(b, pos+4)
			frameH, ok4 := u16le(b, pos+6)
			frameFlags, ok5 := byteAt(b, pos+8)
			if !ok1 || !ok2 || !ok3 || !ok4 || !ok5 {
				return no, false
			}
			pos += 9
			if frameFlags&0x80 != 0 {
				pos += colourTableSize(frameFlags)
			}
			if _, ok := byteAt(b, pos); !ok { // the LZW minimum code size
				return no, false
			}
			pos++
			next, ok := pastSubBlocks(b, pos)
			if !ok {
				return no, false
			}
			pos = next
			// A frame hanging off the screen grows the canvas a browser draws onto.
			if left+frameW > width {
				width = left + frameW
			}
			if top+frameH > height {
				height = top + frameH
			}
		case label == 0x3b:
			return imageHeader{Type: "gif", Width: width, Height: height}, true
		default:
			return no, false
		}
	}
}

func jpegHeader(b []byte) (imageHeader, bool) {
	var no imageHeader
	if !startsWith(b, jpegSign) {
		return no, false
	}
	pos := 2
	for {
		c, ok := byteAt(b, pos)
		if !ok || c != 0xff {
			return no, false
		}
		pos++
		for {
			c, ok := byteAt(b, pos)
			if !ok || c != 0xff {
				break
			}
			pos++ // fill bytes between markers
		}
		marker, ok := byteAt(b, pos)
		if !ok {
			return no, false
		}
		pos++
		if marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7) {
			continue // markers that carry no length
		}
		if marker == 0xda || marker == 0xd9 {
			return no, false // a scan, or the end, before any frame header
		}
		n, ok := u16be(b, pos)
		if !ok || n < 2 {
			return no, false
		}
		content := pos + 2
		pos += n
		if pos > len(b) {
			return no, false
		}
		sof := (marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) ||
			(marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf)
		if !sof {
			continue
		}
		if _, ok := byteAt(b, content); !ok { // the precision byte
			return no, false
		}
		h, ok1 := u16be(b, content+1)
		w, ok2 := u16be(b, content+3)
		if !ok1 || !ok2 || w == 0 || h == 0 {
			return no, false
		}
		return imageHeader{Type: "jpeg", Width: w, Height: h}, true
	}
}

func webpHeader(b []byte) (imageHeader, bool) {
	var no imageHeader
	if !startsWith(b, riffStart) {
		return no, false
	}
	if c, ok := fourcc(b, 8); !ok || c != "WEBP" {
		return no, false
	}
	chunk, ok := fourcc(b, 12)
	if !ok {
		return no, false
	}
	var width, height int
	switch chunk {
	case "VP8 ":
		for _, at := range []int{23, 24, 25} {
			want := []byte{0x9d, 0x01, 0x2a}[at-23]
			if c, ok := byteAt(b, at); !ok || c != want {
				return no, false
			}
		}
		w, ok1 := u16le(b, 26)
		h, ok2 := u16le(b, 28)
		if !ok1 || !ok2 {
			return no, false
		}
		width, height = w&0x3fff, h&0x3fff
	case "VP8L":
		if c, ok := byteAt(b, 20); !ok || c != 0x2f {
			return no, false
		}
		bits, ok := u32le(b, 21)
		if !ok {
			return no, false
		}
		width, height = (bits&0x3fff)+1, ((bits>>14)&0x3fff)+1
	case "VP8X":
		w, ok1 := u24le(b, 24)
		h, ok2 := u24le(b, 27)
		if !ok1 || !ok2 {
			return no, false
		}
		width, height = w+1, h+1
	default:
		return no, false
	}
	if width == 0 || height == 0 {
		return no, false
	}
	return imageHeader{Type: "webp", Width: width, Height: height}, true
}

// readImageHeader is the type and pixel size a picture's header declares, and false when
// the bytes are not a complete, readable PNG, JPEG, GIF or WebP header. It never decodes
// pixels: what the header says is what the bound is measured against.
func readImageHeader(b []byte) (imageHeader, bool) {
	if h, ok := pngHeader(b); ok {
		return h, true
	}
	if h, ok := gifHeader(b); ok {
		return h, true
	}
	if h, ok := jpegHeader(b); ok {
		return h, true
	}
	return webpHeader(b)
}
