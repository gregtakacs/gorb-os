package main

// S3 (ruling 35): the bounds "Import from orb-os.net" keeps to itself.
//
// Two of them. The first is whose links this hub will open at all: only the configured
// site — GORB_SITE, the one origin release.go's resolver answers, so a bench and the
// site's §11 move are the same one string — and orb-os.net under either of its two
// public names, plus loopback, which is how the hub's own tests and a developer's
// machine site are served. A link to any other host is refused here, before a socket
// opens: dialing an unknown host with the design's id would tell that host both that
// this owner imports from orb-os.net and which design they were after.
//
// The second is what a bundle's art may be. The site applies these rules when it stores
// a picture, so a bundle that arrived through the site's own doors already holds to
// them, and one that does not is not a bundle the site served: a picture that is not a
// PNG, JPEG, GIF or WebP, or one whose header names a side longer than 1024 pixels. The
// size is read from the header and nothing else — no pixel is decoded, so a 60-byte file
// claiming 50000 × 50000 is refused as surely as a real one of that size would be. The
// parsing below is the site's src/lib/image-header.ts and design-assets.ts, ported; the
// sentences are the hub's own, worded to finish the owner's "the theme … " sentence.
//
// S3b amends the second bound in two ways. SVG art is now one of the pictures a design
// may hold: an SVG arrives under data:image/svg+xml;base64 and is kept as the bytes it
// came in, so the hub's rules are text rules — a size ceiling, no event handlers, no
// outside links — checked on the decoded text. And a design's name is limited to basic
// characters: the letters, numbers, spaces, dots, underscores and dashes an orb's name
// field and every editor's file system both handle without a surprise.

import (
	"bytes"
	"encoding/base64"
	"encoding/xml"
	"errors"
	"fmt"
	"io"
	"net/url"
	"regexp"
	"sort"
	"strings"
	"unicode/utf8"
)

// orbOSNet is the site's future home (§11): always allowed, https only. Its www spelling
// (ruling 35, as amended 2026-11-11) is the same site behind the other name an owner's
// paste is just as likely to carry, so it is allowed by the same door, and the two are
// named together in the refusal an owner reads.
const (
	orbOSNet    = "orb-os.net"
	orbOSNetWww = "www.orb-os.net"
	// orbOSNetNames is the public half of the allow-list as the sentences spell it.
	orbOSNetNames = orbOSNet + "/" + orbOSNetWww
)

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
	return scheme == "https" && (hostport == orbOSNet || hostport == orbOSNetWww)
}

// siteNotAllowed is the refusal for a link to any other host. It names the two places a
// theme's link can have come from, so the owner reads it as "paste the other link"
// rather than as a failure of the site they meant.
func siteNotAllowed(site string) error {
	conf := canonicalSiteOrigin(siteReleaseBase())
	host := siteHost(site)
	if conf == "https://"+orbOSNet || conf == "https://"+orbOSNetWww {
		return fmt.Errorf("Orb Manager imports themes only from %s, not from %s", orbOSNetNames, host)
	}
	return fmt.Errorf("Orb Manager imports themes only from %s and %s, not from %s", siteHost(conf), orbOSNetNames, host)
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
// declare a size in any of the four formats, and (ruling 35, as amended 2026-11-11)
// that is a refusal — too short to name a size cannot be a picture. The hub's own
// contract fixtures carry real tiny PNGs rather than placeholders under the floor.
const artMinHeaderBytes = 10

// The three refusals, verbatim; the size sentence is a format over the header's sides.
const (
	artRefusalType       = "an image in it is not a PNG, JPEG, GIF or WebP, the only pictures a design can hold"
	artRefusalUnreadable = "an image in it can't be read as a PNG, JPEG, GIF or WebP"
	artRefusalSizeFormat = "an image in it is %d × %d pixels, and a design's pictures can be at most %d × %d"
)

// artPrefixes is the site's ART_PREFIXES: the four data: prefixes a raster picture may
// arrive under, compared lower-cased. S3b adds the SVG prefix beside them, checked apart.
var artPrefixes = []string{"data:image/png;base64", "data:image/jpeg;base64", "data:image/gif;base64", "data:image/webp;base64"}

// svgArtPrefix is S3b's fifth spelling: an SVG picture, kept as the text it arrives as.
const svgArtPrefix = "data:image/svg+xml;base64"

// svgMaxBytes is S3b's ceiling for one SVG: 256 KB of text, the site's SVG_MAX_BYTES. A
// design's rasters are bounded by their declared sides; an SVG declares whatever it
// likes and draws it, so the only honest ceiling is the file's own weight.
const svgMaxBytes = 262144

// The SVG refusals, verbatim. The size sentence counts the bytes up in whole KB, rounded
// up, so the first refused file is 257 KB.
const (
	svgRefusalSizeFormat  = "an SVG in it is %d KB, and a design's SVGs can be at most 256 KB"
	svgRefusalHoldsFormat = "an SVG in it holds %s, which a design's SVGs can't (no scripts, external links or entities)"
	svgRefusalNotText     = "an SVG in it can't be read as text"
	svgRefusalNotSvg      = "an image in it says it is an SVG but isn't one"
)

// fu2-1 (ruling 35, as amended): the rule ids of docs/specs/fu2-svg-rules.md, in the
// order the rules are checked. The first failure is the one reported, in all three
// checkers — this file, the studio's OrbStudio.checkSvg and the site's svg-art.ts.
const (
	svgRuleTooBig         = "svg_too_big"
	svgRuleNotUTF8        = "svg_not_utf8"
	svgRuleNotWellFormed  = "svg_not_well_formed"
	svgRuleMarkup         = "svg_markup"
	svgRuleRoot           = "svg_root"
	svgRuleElement        = "svg_element"
	svgRuleForeignElement = "svg_foreign_element"
	svgRuleEventAttr      = "svg_event_attr"
	svgRuleLink           = "svg_link"
	svgRuleCSS            = "svg_css"
)

// svgRuleHolds is the holds-sentence each refusal rule words into: "an SVG in it holds
// …". The size, text and root refusals keep their own long-standing sentences.
var svgRuleHolds = map[string]string{
	svgRuleNotWellFormed:  "XML that does not parse",
	svgRuleMarkup:         "a DOCTYPE, a CDATA section, a processing instruction or a comment a design's SVGs can't hold",
	svgRuleElement:        "an element a design's SVGs can't hold",
	svgRuleForeignElement: "an element from another namespace",
	svgRuleEventAttr:      "an event handler",
	svgRuleLink:           "an outside link",
	svgRuleCSS:            "CSS that can reach outside the file",
}

// svgRuleSentence is the hub's sentence for one rule id, "" for the accepted rule "".
// The size refusal carries the file's own weight and is worded by its caller.
func svgRuleSentence(rule string) string {
	switch rule {
	case "":
		return ""
	case svgRuleTooBig:
		return ""
	case svgRuleNotUTF8:
		return svgRefusalNotText
	case svgRuleRoot:
		return svgRefusalNotSvg
	}
	return fmt.Sprintf(svgRefusalHoldsFormat, svgRuleHolds[rule])
}

// The namespaces the rules name by URI.
const (
	svgNS        = "http://www.w3.org/2000/svg"
	svgXLinkNS   = "http://www.w3.org/1999/xlink"
	svgXMLNS     = "http://www.w3.org/XML/1998/namespace"
	svgXMLNSDecl = "http://www.w3.org/2000/xmlns/"
)

// svgLinkAttr is rule 9's extra half: the attribute names, beyond href and xlink:href,
// that a browser or an SVG renderer reads as a reference the way it reads href — src on an
// image included, which is how the review's svgprobe set reaches its beacon. Any value in
// one of them is a reach out of the file, whatever it says, because none of them is a
// reference a design's art has any reason to carry.
var svgLinkAttr = []string{
	"src", "background", "poster", "action", "formaction", "dynsrc", "lowsrc",
}

// svgFragRef is the shape of a same-document reference: "#" and an XML name. The \p{}
// classes stand in for XML 1.0's NameStartChar and NameChar; the studio's checkSvg and
// the site's svg-art.ts match this regexp's set of characters exactly.
var svgFragRef = regexp.MustCompile("^#[\\p{L}__:][\\p{L}\\p{N}._:-]*$")

// svgDataURL is rule 9's allowed data: URL: one of the four raster formats the design
// could have carried directly, base64, and a tail of nothing but base64 characters.
var svgDataURL = regexp.MustCompile(`^data:image/(?:png|jpeg|gif|webp);base64,[A-Za-z0-9+/=]+$`)

// svgURLTarget finds a CSS url( reference and its target, past optional whitespace and
// optional quotes, up to the quote or the closing parenthesis.
var svgURLTarget = regexp.MustCompile(`(?i)url\(\s*['"]?([^'")]*)`)

// svgValueOutside is rule 9's substring half: a value that holds one of these cannot be
// anything but a reach out of the file.
var svgValueOutside = []string{"javascript:", "data:", "image-set("}

// svgStyleBad is rule 10's substring half: an escape, a comment opening, an at-rule or a
// tag opening — any of which hides a reference from the plain url( scan — plus the two
// CSS image sources, matched lower-cased like rule 9's image-set.
var svgStyleBad = []string{`\`, "/*", "@", "<", "image-set", "expression"}

// svgAllowedElement is rule 6's list: the elements, in the SVG namespace, a design's SVG
// may hold. Everything else in the SVG namespace is refused, notably script, style,
// foreignObject, every animation element, feImage, a, iframe, embed, object, video, audio
// and the old SVG Tiny handler/cursor/font shapes. Art is baked to pixels for a
// 466 × 466 display, so animation and styling sheets have no use.
var svgAllowedElement = map[string]bool{}

// svgForeignForbiddenLocal is rule 7's catch: even an element in an allowed foreign
// namespace is refused when its local name, case-insensitively, names an SVG or HTML
// element, because an HTML parser inlining the document ignores prefixes and would raise a
// real element out of it.
var svgForeignForbiddenLocal = map[string]bool{}

func init() {
	for _, n := range strings.Fields(`svg g defs symbol use path rect circle ellipse line polyline polygon
		text tspan textPath title desc metadata linearGradient radialGradient stop pattern clipPath mask
		marker image filter feBlend feColorMatrix feComponentTransfer feComposite feDiffuseLighting
		feDisplacementMap feDistantLight feDropShadow feFlood feFuncA feFuncB feFuncG feFuncR feGaussianBlur
		feMerge feMergeNode feMorphology feOffset fePointLight feSpecularLighting feSpotLight feTile
		feTurbulence switch view`) {
		svgAllowedElement[n] = true
		svgForeignForbiddenLocal[n] = true
	}
	for _, n := range strings.Fields(`a abbr acronym address applet area article aside audio b base basefont
		bdi bdo bgsound big blink blockquote body br button canvas caption center cite code col colgroup
		content data datalist dd del details dfn dialog dir div dl dt em embed fieldset figcaption figure
		font footer form frame frameset h1 h2 h3 h4 h5 h6 head header hgroup hr html i iframe image img
		input ins isindex kbd keygen label legend li link listing main map mark marquee menu menuitem meta
		meter nav noembed noframes nobr ol optgroup option output p param picture plaintext pre progress q
		rp rt ruby s samp script section select shadow small source spacer span strike strong style sub
		summary sup table tbody td template textarea tfoot th thead time title tr track tt u ul var video
		wbr xmp`) {
		svgForeignForbiddenLocal[n] = true
	}
}

// svgEditorNS is rule 7's unconditional foreign namespaces: the two Inkscape and Sodipodi
// write into a plain save, elements and attributes alike.
var svgEditorNS = map[string]bool{
	"http://www.inkscape.org/namespaces/inkscape":        true,
	"http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd": true,
}

// svgMetaNS is rule 7's conditional ones: RDF, Creative Commons and Dublin Core, allowed
// only inside a metadata element, which is where an editor puts a file's authorship.
var svgMetaNS = map[string]bool{
	"http://www.w3.org/1999/02/22-rdf-syntax-ns#": true,
	"http://creativecommons.org/ns#":              true,
	"http://web.resource.org/cc/":                 true,
	"http://purl.org/dc/elements/1.1/":            true,
}

// checkSvgArt is the SVG check of docs/specs/fu2-svg-rules.md: the decoded text of one
// picture announced under the SVG prefix. It returns "" when a design may hold the SVG and
// the refusal sentence otherwise. The order is the rule set's: weight first (a body over
// the ceiling is refused without parsing it), then the bytes, then a walk of the markup
// that decides rules 3 and 4, then the parse, then the elements, handlers, links and CSS
// in rule order.
func checkSvgArt(b []byte) string {
	_, sentence := checkSvgArtRule(b)
	return sentence
}

// checkSvgArtRule is the same check with the rule id first, so the hub, the studio and the
// site's svg-art.ts can be held to one answer per vector. The sentence is the hub's own
// wording of that rule; the rule id is the fact the shared vectors assert.
func checkSvgArtRule(b []byte) (string, string) {
	if len(b) > svgMaxBytes {
		return svgRuleTooBig, fmt.Sprintf(svgRefusalSizeFormat, (len(b)+1023)/1024)
	}
	if !utf8.Valid(b) || bytes.IndexByte(b, 0) >= 0 {
		return svgRuleNotUTF8, svgRuleSentence(svgRuleNotUTF8)
	}
	text := strings.TrimPrefix(string(b), "\ufeff") // a leading BOM is allowed and belongs to no markup
	rule := svgScanMarkup(text)
	if rule == "" {
		rule = svgParseRule([]byte(text))
	}
	return rule, svgRuleSentence(rule)
}

// svgParseRule parses the document with a strict, namespace-aware parser and answers the
// first rule its answer fails, "" when it fails none. Rules 3 and 4 came from the markup
// walk, or from the three leniency points encoding/xml keeps where a strict XML parser
// does not — a duplicated attribute, an undeclared prefix, a document that is not one
// element; rules 5 to 10 are the elements, attributes and styles the parse handed back.
func svgParseRule(b []byte) string {
	doc := svgWalkParse(b)
	if doc.rule != "" {
		return doc.rule
	}
	if !doc.rootSeen || doc.rootNS != svgNS || doc.rootName != "svg" {
		return svgRuleRoot
	}
	for _, f := range []struct {
		rule string
		bad  bool
	}{
		{svgRuleElement, doc.badElement},
		{svgRuleForeignElement, doc.foreignElement},
		{svgRuleEventAttr, doc.eventAttr},
		{svgRuleLink, doc.link},
		{svgRuleCSS, doc.css},
	} {
		if f.bad {
			return f.rule
		}
	}
	return ""
}

// svgDoc is one document as the walk saw it: the rule the parse itself failed, if any, the
// root's name and namespace, and one flag per element rule. Every flag is the whole
// document's answer to one rule, because the rules are asked in order and the first failure
// is the one reported: a file holding both a <script> and an outside link is refused for
// its element, wherever in the file each of them sits.
type svgDoc struct {
	rule           string
	rootSeen       bool
	rootName       string
	rootNS         string
	badElement     bool
	foreignElement bool
	eventAttr      bool
	link           bool
	css            bool
}

// svgScanMarkup walks the raw text the way an XML parser walks it — over the text between
// tags, inside a start tag past quoted attribute values, through comments, declarations and
// processing instructions — and answers the two rules a parse alone does not settle the
// same way in all three checkers: svg_not_well_formed for markup a strict parser stops at,
// and svg_markup for the constructs a design may not hold at all. "" is the answer for
// markup a design may hold. The studio's checkSvg makes the same walk by hand, so all three
// checkers refuse the same construct under the same rule id.
func svgScanMarkup(text string) string {
	markup := "" // the first rule-4 construct, held until the walk is sure there is no rule 3
	i := 0
	if strings.HasPrefix(text, "\ufeff") {
		i = len("\ufeff") // a leading BOM is allowed and belongs to no markup
	}
	for i < len(text) {
		lt := strings.IndexByte(text[i:], '<')
		if lt < 0 {
			break
		}
		i += lt
		switch {
		case strings.HasPrefix(text[i:], "<!--"):
			end := strings.Index(text[i+4:], "-->")
			if end < 0 || strings.Contains(text[i+4:i+4+end], "--") {
				return svgRuleNotWellFormed // an unterminated comment, or one holding "--"
			}
			if strings.ContainsAny(text[i+4:i+4+end], "<>") {
				markup = svgRuleMarkup // a comment holding a tag opens a tag inside the file
			}
			i += 4 + end + 3
		case strings.HasPrefix(text[i:], "<![CDATA["):
			end := strings.Index(text[i+9:], "]]>")
			if end < 0 {
				return svgRuleNotWellFormed
			}
			markup = svgRuleMarkup
			i += 9 + end + 3
		case strings.HasPrefix(text[i:], "<!DOCTYPE"):
			end := svgDeclEnd(text[i:])
			if end < 0 {
				return svgRuleNotWellFormed
			}
			markup = svgRuleMarkup
			i += end
		case strings.HasPrefix(text[i:], "<!"):
			return svgRuleNotWellFormed // no other <! is a declaration XML knows
		case strings.HasPrefix(text[i:], "<?"):
			end := strings.Index(text[i+2:], "?>")
			if end < 0 {
				return svgRuleNotWellFormed
			}
			if svgPIName(text[i+2:i+2+end]) != "xml" || i > len("\ufeff") {
				markup = svgRuleMarkup // a processing instruction, or a declaration out of place
			}
			i += 2 + end + 2
		case strings.HasPrefix(text[i:], "</"):
			end := strings.IndexByte(text[i:], '>')
			if end < 0 {
				return svgRuleNotWellFormed
			}
			i += end + 1
		default:
			next, ok := svgScanTag(text[i:])
			if !ok {
				return svgRuleNotWellFormed
			}
			i += next
		}
	}
	return markup
}

// svgScanTag is the walk through one start tag, from its "<" to the byte past its ">":
// quotes are honoured, so neither a ">" nor a "<!" inside a value moves the walk out of the
// tag. It answers false for whatever a strict parser stops at — a tag with no name, an
// attribute with no value, a value without quotes, a tag that never closes.
func svgScanTag(t string) (int, bool) {
	i := 1
	var seen map[string]bool
	for i < len(t) && !svgTagBreak(t[i]) {
		i++
	}
	if i == 1 {
		return 0, false // '<' then whitespace, '/', '>' or nothing at all: no name
	}
	for i < len(t) {
		for i < len(t) && svgSpaceByte(t[i]) {
			i++
		}
		if i >= len(t) {
			return 0, false
		}
		if t[i] == '>' {
			return i + 1, true
		}
		if t[i] == '/' {
			if i+1 >= len(t) || t[i+1] != '>' {
				return 0, false
			}
			return i + 2, true
		}
		name := i
		for name < len(t) && !svgTagBreak(t[name]) && t[name] != '=' {
			name++
		}
		if name == i {
			return 0, false
		}
		attr := strings.ToLower(t[i:name])
		i = name
		for i < len(t) && svgSpaceByte(t[i]) {
			i++
		}
		if i >= len(t) || t[i] != '=' {
			return 0, false
		}
		i++
		for i < len(t) && svgSpaceByte(t[i]) {
			i++
		}
		if i >= len(t) {
			return 0, false
		}
		if quote := t[i]; quote == '"' || quote == '\'' {
			end := strings.IndexByte(t[i+1:], quote)
			if end < 0 {
				return 0, false
			}
			if strings.IndexByte(t[i+1:i+1+end], '<') >= 0 {
				return 0, false // a "<" in a value has to be &lt; for the file to parse at all
			}
			if seen == nil {
				seen = map[string]bool{}
			}
			if seen[attr] {
				return 0, false // one attribute name, twice, is not well-formed
			}
			seen[attr] = true
			i += 2 + end
			if i < len(t) && !svgSpaceByte(t[i]) && t[i] != '>' && t[i] != '/' {
				return 0, false // two attributes with nothing between them is not a tag
			}
			continue
		}
		return 0, false // an unquoted attribute value is not XML
	}
	return 0, false
}

func svgTagBreak(c byte) bool { return c == '<' || c == '>' || c == '/' || svgSpaceByte(c) }

func svgSpaceByte(c byte) bool { return c == ' ' || c == '\t' || c == '\n' || c == '\r' }

// svgDeclEnd is the length past a <!DOCTYPE …> declaration: the "]>" closing an internal
// subset, or the ">" closing the declaration, and -1 when neither is there.
func svgDeclEnd(t string) int {
	end := strings.IndexByte(t, '>')
	if end < 0 {
		return -1
	}
	if sub := strings.IndexByte(t[:end], '['); sub >= 0 {
		closer := strings.Index(t, "]>")
		if closer < 0 {
			return -1
		}
		end = closer + 1
	}
	return end + 1
}

// svgPIName is a processing instruction's target, the name before its first space.
func svgPIName(inst string) string {
	i := strings.IndexAny(inst, " \t\r\n")
	if i < 0 {
		return inst
	}
	return inst[:i]
}

// svgWalkParse runs the parse and collects what the document is. encoding/xml is strict
// about well-formedness but lenient about three things a strict XML parser is not — a
// duplicated attribute, an undeclared namespace prefix and a document that is not exactly
// one element — so this walk checks those itself and calls them rule 3.
func svgWalkParse(b []byte) svgDoc {
	doc := svgDoc{}
	dec := xml.NewDecoder(bytes.NewReader(b))
	dec.Strict = true
	dec.Entity = nil
	depth, metaAt, roots := 0, -1, 0
	for {
		tok, err := dec.Token()
		if err == io.EOF {
			break
		}
		if err != nil {
			doc.rule = svgRuleNotWellFormed
			return doc
		}
		switch t := tok.(type) {
		case xml.StartElement:
			if depth == 0 {
				roots++
				if roots > 1 {
					doc.rule = svgRuleNotWellFormed // a document is one element, not two
					return doc
				}
				doc.rootSeen, doc.rootName, doc.rootNS = true, t.Name.Local, t.Name.Space
			}
			if svgUndeclared(t.Name.Space) {
				doc.rule = svgRuleNotWellFormed // a prefix the document never declared
				return doc
			}
			doc.startElement(t, metaAt >= 0)
			if doc.rule != "" {
				return doc
			}
			if t.Name.Local == "metadata" && t.Name.Space == svgNS {
				metaAt = depth
			}
			depth++
		case xml.EndElement:
			depth--
			if depth == metaAt {
				metaAt = -1
			}
		case xml.CharData:
			if depth == 0 && strings.TrimSpace(string(t)) != "" {
				doc.rule = svgRuleNotWellFormed // text outside the document element
				return doc
			}
		}
	}
	if !doc.rootSeen {
		doc.rule = svgRuleNotWellFormed // a document with no element in it at all
	}
	return doc
}

// startElement records what one start tag does to the document's answers: rule 6 for an
// element in the SVG namespace, rule 7 for one in any other, rule 8, 9 or 10 for one of its
// attributes. Rule 3 is the only one that can stop the walk on its way, because it outranks
// every rule the rest of the document could still answer.
func (doc *svgDoc) startElement(t xml.StartElement, inMeta bool) {
	local, space := t.Name.Local, t.Name.Space
	switch {
	case space == svgNS:
		if !svgAllowedElement[local] {
			doc.badElement = true
		}
	case svgEditorNS[space], inMeta && svgMetaNS[space]:
		if svgForeignForbiddenLocal[strings.ToLower(local)] {
			doc.foreignElement = true // an editor's namespace wrapped around a real element name
		}
	default:
		doc.foreignElement = true
	}
	seen := make(map[string]bool, len(t.Attr))
	for _, a := range t.Attr {
		if dup := a.Name.Space + "\x00" + a.Name.Local; seen[dup] {
			doc.rule = svgRuleNotWellFormed // two attributes with one name is not well-formed
			return
		} else {
			seen[dup] = true
		}
		name := strings.ToLower(a.Name.Local)
		if strings.HasPrefix(name, "on") {
			doc.eventAttr = true // rule 8 asks every attribute, in every namespace, first
			continue
		}
		if svgUndeclared(a.Name.Space) {
			doc.rule = svgRuleNotWellFormed
			return
		}
		if a.Name.Space == "xmlns" || (a.Name.Space == "" && name == "xmlns") {
			continue // a namespace declaration is neither a reference nor a style
		}
		doc.attr(a, name)
	}
}

// attr records what one attribute does: rule 9 for a reference, rule 10 for a style.
func (doc *svgDoc) attr(a xml.Attr, name string) {
	value := a.Value
	if name == "base" && a.Name.Space == svgXMLNS {
		doc.link = true // xml:base moves every relative reference in the file
		return
	}
	if name == "href" && (a.Name.Space == "" || a.Name.Space == svgXLinkNS) {
		if !svgRefOK(value) {
			doc.link = true
		}
		return
	}
	if name == "style" && a.Name.Space == "" {
		if !svgStyleOK(value) {
			doc.css = true
		}
		return
	}
	lower := strings.ToLower(value)
	// Presentation attributes (fill, filter, mask, cursor …) are parsed as CSS values, so a
	// CSS escape there turns into url( after the scans below have looked (Chromium fetches
	// fill="\75rl(…)"). An escape or a comment opening in any attribute value is rule 10.
	if strings.Contains(value, `\`) || strings.Contains(value, "/*") {
		doc.css = true
	}
	for _, ref := range svgLinkAttr {
		if name == ref {
			doc.link = true // an attribute a renderer reads as a reference, href or no href
			return
		}
	}
	for _, out := range svgValueOutside {
		if strings.Contains(lower, out) {
			doc.link = true
			return
		}
	}
	if svgURLOutside(value) {
		doc.link = true
	}
}

// svgUndeclared reports whether a name carries a prefix the document never declared:
// encoding/xml hands the prefix itself back where a strict parser hands back a namespace.
func svgUndeclared(space string) bool {
	return space != "" && space != "xmlns" && !strings.Contains(space, ":")
}

// svgRefOK is rule 9's href answer: a fragment of the same file, or an embedded raster of
// one of the four formats a design could have carried directly. The value arrives with the
// parser's entities already decoded, so an &#x6a;avascript: URL is a javascript: URL here.
func svgRefOK(value string) bool {
	t := strings.Trim(value, " \t\r\n")
	return svgFragment(t) || svgDataURL.MatchString(strings.ToLower(t))
}

// svgFragment: the reference is "#" and an XML name, which is a same-document reference.
func svgFragment(t string) bool { return svgFragRef.MatchString(t) }

// svgStyleOK is rule 10: a style attribute with nothing in it that could hide a reference
// or reach out of the file. The style attribute is the one attribute rule 9's substring
// scan leaves to this rule, so a url( in a style is a CSS refusal and not a link refusal.
func svgStyleOK(value string) bool {
	lower := strings.ToLower(value)
	for _, bad := range svgStyleBad {
		if strings.Contains(lower, bad) {
			return false
		}
	}
	return !svgURLOutside(value)
}

// svgURLOutside reports whether a value holds a url( whose target is anything but a
// fragment of the same file. Every url( counts, not just the first: a filter list
// (filter="url(#a) url(…)") names several, and Chromium fetches each one.
func svgURLOutside(value string) bool {
	for _, m := range svgURLTarget.FindAllStringSubmatch(value, -1) {
		if !svgFragment(strings.TrimSpace(m[1])) {
			return true
		}
	}
	return false
}

// artPrefixRefusal is "" when a data-URL prefix names one of the five picture kinds a
// design may hold, and the type sentence otherwise. It judges the prefix alone and never
// looks at bytes: this is the question a reused art ref (a data URL with no bytes yet in
// hand) is asked, and fu2-2 rules that such a ref is never refused for length.
func artPrefixRefusal(prefix string) string {
	lower := strings.ToLower(prefix)
	if lower == svgArtPrefix {
		return ""
	}
	for _, p := range artPrefixes {
		if p == lower {
			return ""
		}
	}
	return artRefusalType
}

// artRefusal is "" when bytes announced by this data-URL prefix are a picture a design
// may hold, and the sentence otherwise — the site's artRefusal, in its order: the prefix
// first, because it is free; then the floor and the header. S3b's SVG prefix takes the
// text check instead of the header check: an SVG's declared sides are its own business,
// its weight, handlers and links are not. fu2-2 (ruling 35, as amended): a raster under
// artMinHeaderBytes is refused as unreadable, and a nil payload (the prefix-only pass)
// is judged by artPrefixRefusal instead, so a reused ref is never refused for length.
func artRefusal(prefix string, b []byte) string {
	lower := strings.ToLower(prefix)
	if lower == svgArtPrefix {
		if b == nil {
			return artPrefixRefusal(svgArtPrefix) // the prefix-only pass: the bytes come next
		}
		return checkSvgArt(b)
	}
	if s := artPrefixRefusal(lower); s != "" {
		return s
	}
	if len(b) < artMinHeaderBytes {
		return artRefusalUnreadable
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
// as its hashes say and rules outside that refuse it — its art (S3), or the name its
// shared name carries (S3b). handleSiteImport words it apart from "is damaged" for that
// reason — the owner's fix is a smaller picture or a plainer name, not a re-download.
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
		if s := artPrefixRefusal(prefix); s != "" { // a ref is judged by its prefix alone (fu2-2)
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

// --- S3b: the name rules -----------------------------------------------------------------
// A design's name is shown back verbatim, is baked into a theme's orb name field and is
// the file an editor's draft lives under. Three places, three things that go wrong with
// the wrong characters in a name: a control character reaches the orb's display, a
// non-ASCII letter reaches the file system under a normalising unicode form and the
// draft and the theme are two files for one name, an emoji reaches all three and is a
// box on the card. So a new name is limited to the basic characters, everywhere a name
// is first chosen: a design save, a keep, and the shared name an import brings in.
// Names already in the library keep working under the old rules — open, list, forget and
// install all answer them — and a save that repeats a name exactly replaces what is
// under it, as it did.

// themeNameRe is S3b's character rule: letters, numbers, spaces, dots, underscores and
// dashes, and nothing else. A name must hold at least one of them; emptiness and leading
// and trailing space are the trimmer's and the empty refusal's business, not the rule's.
var themeNameRe = regexp.MustCompile(`^[A-Za-z0-9 ._-]+$`)

// themeNameRefusalChars is the sentence the character rule answers with, the allowed set
// written out because the rule refuses names the author can see are "letters".
const themeNameRefusalChars = "a design's name can use only letters (A–Z, a–z), numbers, spaces, dots, underscores and dashes"

// handleRe is the owner's orb-os.net handle inside a shared name: a lower-case word of
// 3 to 24 characters, the site's HANDLE_RE.
var handleRe = regexp.MustCompile(`^[a-z][a-z0-9_-]{2,23}$`)

// sharedNameSep is what the site puts between a theme's name and its author's handle.
const sharedNameSep = " by @"

// themeNameOK is S3b's character rule answering about a whole name, either form a name
// reaches the hub in: the plain name an editor's field holds, or the shared form
// "<name> by @<handle>" a bake carries after an import, whose separator is legal only
// with a legal name and a legal handle on its two sides.
func themeNameOK(n string) bool {
	if name, handle, found := strings.Cut(n, sharedNameSep); found {
		return name != "" && themeNameRe.MatchString(name) && handleRe.MatchString(handle)
	}
	return themeNameRe.MatchString(n)
}

// checkThemeName is S3b's rule for a name being chosen now: trimmed, not empty, and made
// only of the basic characters (or the shared form of them). It returns the name as it
// will be filed. It says nothing about length — a design's own 64-character bound still
// applies, and an import's shared name carries the orb's 55-byte ceiling checkBundle
// already enforces.
func checkThemeName(raw string) (string, error) {
	n := strings.TrimSpace(raw)
	if n == "" {
		return "", errors.New("a design needs a name")
	}
	if !themeNameOK(n) {
		return "", errors.New(themeNameRefusalChars)
	}
	return n, nil
}

// checkSharedName reads the site's shared name and answers with the theme-name part and
// the handle, or the sentence that refuses the bundle. A shared name carries the author's
// handle after " by @", and the first such separator is the one the site wrote: a
// theme's own name cannot hold an "@". A shared name with no separator at all is a
// design's plain name — which is what a bundle for an unshared design serves — and the
// plain character rule reads it.
func checkSharedName(shared string) (name string, handle string, sentence string) {
	if i := strings.Index(shared, sharedNameSep); i >= 0 {
		name, handle = shared[:i], shared[i+len(sharedNameSep):]
		if name == "" || !themeNameRe.MatchString(name) {
			return "", "", themeNameRefusalChars
		}
		if !handleRe.MatchString(handle) {
			return "", "", "its shared name holds a handle that is not allowed"
		}
		return name, handle, ""
	}
	if _, err := checkThemeName(shared); err != nil {
		return "", "", err.Error()
	}
	return shared, "", ""
}
